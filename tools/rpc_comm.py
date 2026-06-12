#!/usr/bin/env python3
"""
rpc_comm.py - RPCortex Communicator

A PC-side companion for a device running RPCortex. Talks to the board over
USB serial using MicroPython's raw REPL (the same handshake the Web Installer
uses), so it works whether the device is sitting at the RPCortex shell or the
login screen - it sends RPCortex's `rawrepl` escape to drop to the firmware
REPL first, then drives it directly.

Requires: pyserial  ->  pip install pyserial

Usage:
  python rpc_comm.py --port COM7 <command> [args]
  python rpc_comm.py ports                      # list serial ports (no --port)

Commands:
  info                          Show OS version + platform
  reg get <Section.Key>         Read a registry value
  reg set <Section.Key> <val>   Write a registry value
  time-sync                     Set the device clock to this PC's local time
  ls [path]                     List a directory on the device (default /)
  push <local> <devpath>        Copy a host file to the device
  pull <devpath> <local>        Copy a device file to the host
  rm <devpath>                  Delete a file on the device
  pkg-install <file.pkg>        Install a local package (push + install)
  pkg-online <Name>             Install a package from the repo (device WiFi)
  os-update <file.rpc>          Apply a local OS image (preserves user data)
  os-update-online              Fetch the latest OS image here, then apply it
  shell "<command>"             Run one RPCortex shell command (advanced)

Notes:
  - os-update / os-update-online use the device's own ZIP extractor
    (rpc_install), which preserves /Users and /Pulsar (accounts, settings,
    WiFi, packages). The device reboots when done.
  - Close PuTTY/Thonny before running - only one program can own the port.
"""

import sys
import os
import time
import base64
import argparse

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")


CHUNK = 256            # bytes per raw-REPL file-write chunk (base64-encoded)
OFFICIAL_LATEST = "https://rpc.novalabs.app/releases/latest.json"


class DeviceError(Exception):
    pass


# ===========================================================================
# Raw-REPL transport
# ===========================================================================

class Device:
    def __init__(self, port, baud=115200, verbose=True):
        self.verbose = verbose
        try:
            self.ser = serial.Serial(port, baud, timeout=0.1)
        except Exception as e:
            raise DeviceError("Cannot open {}: {}".format(port, e))
        self._buf = b''

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _log(self, msg):
        if self.verbose:
            print(msg)

    def _write(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.ser.write(data)
        self.ser.flush()

    def _read_until(self, marker, timeout=8):
        """Read until `marker` (bytes) appears; return everything before it."""
        if isinstance(marker, str):
            marker = marker.encode()
        deadline = time.time() + timeout
        while time.time() < deadline:
            idx = self._buf.find(marker)
            if idx != -1:
                before = self._buf[:idx]
                self._buf = self._buf[idx + len(marker):]
                return before
            chunk = self.ser.read(256)
            if chunk:
                self._buf += chunk
            else:
                time.sleep(0.01)
        raise DeviceError("Timeout waiting for {!r}".format(marker))

    def _clear(self):
        # drain anything pending
        time.sleep(0.05)
        while True:
            chunk = self.ser.read(256)
            if not chunk:
                break
        self._buf = b''

    # ---- RPCortex-aware REPL entry (mirrors serial-device.js) --------------
    def ensure_repl(self):
        self._write(b'\r\x03\x03')
        time.sleep(0.3)
        self._clear()
        self._write(b'\r\n')
        time.sleep(0.4)
        pending = self.ser.read(512)
        self._buf += pending
        if b'>>>' in self._buf:
            self._clear()
            return True
        # RPCortex is running (shell or login) - use its rawrepl escape.
        self._clear()
        self._write(b'rawrepl\r\n')
        try:
            self._read_until(b'>>>', timeout=7)
            self._clear()
            return True
        except DeviceError:
            pass
        self._write(b'\r\x03\x03')
        time.sleep(0.4)
        self._clear()
        self._write(b'\r\n')
        time.sleep(0.4)
        ok = b'>>>' in (self._buf + self.ser.read(512))
        self._clear()
        return ok

    def enter_raw(self):
        self._log("[:] Connecting to the RPCortex device...")
        if not self.ensure_repl():
            raise DeviceError("Could not reach the device REPL. Is RPCortex running and the port free?")
        self._write(b'\x03\x03')
        time.sleep(0.3)
        self._clear()
        self._write(b'\x01')                     # Ctrl-A -> raw REPL
        self._read_until(b'raw REPL', timeout=5)
        time.sleep(0.15)
        self._clear()
        try:
            self.exec_('0')                      # sync / absorb stray bytes
        except DeviceError:
            pass
        self._log("[@] Connected.")

    def exit_raw(self):
        self._write(b'\x02')                     # Ctrl-B -> friendly REPL
        time.sleep(0.2)

    def exec_(self, code, timeout=20):
        """Run a line of MicroPython; return stdout. Raises on device error."""
        self._clear()
        self._write(code.encode() + b'\x04')     # Ctrl-D -> execute
        self._read_until(b'OK', timeout=6)
        out = self._read_until(b'\x04', timeout=timeout)   # stdout
        err = self._read_until(b'\x04', timeout=timeout)   # stderr
        try:
            self._read_until(b'>', timeout=3)
        except DeviceError:
            pass
        if err.strip():
            raise DeviceError(err.decode('utf-8', 'replace').strip())
        return out.decode('utf-8', 'replace')

    def reboot(self):
        self._log("[:] Rebooting device...")
        self._write(b'import machine; machine.reset()\r\n')
        time.sleep(0.3)


# ===========================================================================
# Device-side helpers (small MicroPython snippets run over raw REPL)
# ===========================================================================

_REG_PRELUDE = "import sys\nif '/Core' not in sys.path: sys.path.append('/Core')\nimport regedit\n"


def _pyrepr(s):
    """Safe single-quoted Python literal for a string argument."""
    return "'" + s.replace('\\', '\\\\').replace("'", "\\'") + "'"


def cmd_info(dev):
    code = ("import sys\n"
            "if '/Core' not in sys.path: sys.path.append('/Core')\n"
            "import RPCortex as R\n"
            "print(R.OS_VERSION, '|', R.OS_CODENAME, '|', sys.platform)")
    print(dev.exec_(code).strip())


def cmd_reg_get(dev, key):
    out = dev.exec_(_REG_PRELUDE + "print(regedit.read({}))".format(_pyrepr(key)))
    print(out.strip())


def cmd_reg_set(dev, key, value):
    dev.exec_(_REG_PRELUDE + "regedit.save({}, {})".format(_pyrepr(key), _pyrepr(value)))
    print("[@] {} = {}".format(key, value))


def cmd_time_sync(dev):
    t = time.localtime()
    # MicroPython machine.RTC().datetime(): (Y, M, D, weekday, H, Min, S, subsec)
    # Python tm_wday: Mon=0..Sun=6 -> MicroPython expects Mon=0..Sun=6 as well.
    tup = (t.tm_year, t.tm_mon, t.tm_mday, t.tm_wday, t.tm_hour, t.tm_min, t.tm_sec, 0)
    dev.exec_("import machine; machine.RTC().datetime({})".format(repr(tup)))
    print("[@] Device clock set to {} (this PC's local time).".format(
        time.strftime('%Y-%m-%d %H:%M:%S', t)))


def cmd_ls(dev, path):
    path = path or '/'
    code = ("import uos\n"
            "p={}\n".format(_pyrepr(path)) +
            "for n in uos.listdir(p):\n"
            " f=(p.rstrip('/')+'/'+n) if p!='/' else '/'+n\n"
            " try:\n"
            "  st=uos.stat(f)\n"
            "  print(('d' if st[0]&0x4000 else 'f'), st[6], n)\n"
            " except Exception:\n"
            "  print('?', 0, n)")
    out = dev.exec_(code)
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3:
            kind, size, name = parts
            tag = '[DIR]' if kind == 'd' else '{:>8}'.format(size)
            print("  {}  {}".format(tag, name))
    print("  ({})".format(path))


def _ensure_dirs(dev, devpath):
    parts = [p for p in devpath.split('/') if p]
    cur = ''
    for p in parts[:-1]:
        cur += '/' + p
        dev.exec_("import uos\ntry:\n uos.mkdir({})\nexcept OSError:\n pass".format(_pyrepr(cur)))


def cmd_push(dev, local, devpath):
    if not os.path.isfile(local):
        raise DeviceError("No such local file: " + local)
    data = open(local, 'rb').read()
    _ensure_dirs(dev, devpath)
    if not data:
        dev.exec_("f=open({},'w'); f.close()".format(_pyrepr(devpath)))
    else:
        total = len(data)
        for i in range(0, total, CHUNK):
            chunk = data[i:i + CHUNK]
            b64 = base64.b64encode(chunk).decode()
            mode = 'wb' if i == 0 else 'ab'
            dev.exec_("import ubinascii\n"
                      "f=open({},'{}')\n".format(_pyrepr(devpath), mode) +
                      "f.write(ubinascii.a2b_base64('{}'))\n".format(b64) +
                      "f.close()")
            done = min(i + CHUNK, total)
            sys.stdout.write("\r[:] Pushing {} ... {}/{} B".format(devpath, done, total))
            sys.stdout.flush()
    print()
    # verify
    sz = dev.exec_("import uos; print(uos.stat({})[6])".format(_pyrepr(devpath))).strip()
    if sz.isdigit() and int(sz) == len(data):
        print("[@] {} written & verified ({} B).".format(devpath, sz))
    else:
        raise DeviceError("Size mismatch after write (got {}).".format(sz))


def cmd_pull(dev, devpath, local):
    sz = dev.exec_("import uos; print(uos.stat({})[6])".format(_pyrepr(devpath))).strip()
    if not sz.isdigit():
        raise DeviceError("Cannot stat device file: " + devpath)
    total = int(sz)
    out = open(local, 'wb')
    pos = 0
    while pos < total:
        code = ("import ubinascii\n"
                "f=open({},'rb')\n".format(_pyrepr(devpath)) +
                "f.seek({})\n".format(pos) +
                "print(ubinascii.b2a_base64(f.read({})).decode().strip())\n".format(CHUNK) +
                "f.close()")
        b64 = dev.exec_(code).strip()
        if not b64:
            break
        out.write(base64.b64decode(b64))
        pos += CHUNK
        sys.stdout.write("\r[:] Pulling {} ... {}/{} B".format(devpath, min(pos, total), total))
        sys.stdout.flush()
    out.close()
    print()
    print("[@] Saved to {} ({} B).".format(local, os.path.getsize(local)))


def cmd_rm(dev, devpath):
    dev.exec_("import uos; uos.remove({})".format(_pyrepr(devpath)))
    print("[@] Removed {}.".format(devpath))


def cmd_pkg_install(dev, pkgfile):
    if not pkgfile.endswith('.pkg'):
        print("[?] Warning: expected a .pkg file.")
    tmp = '/_comm_install.pkg'
    cmd_push(dev, pkgfile, tmp)
    print("[:] Installing on device...")
    out = dev.exec_("import sys\n"
                    "if '/Core' not in sys.path: sys.path.append('/Core')\n"
                    "import pkgmgr\n"
                    "print('RESULT', pkgmgr.install({}, force=True))".format(_pyrepr(tmp)),
                    timeout=40)
    dev.exec_("import uos\ntry:\n uos.remove({})\nexcept OSError:\n pass".format(_pyrepr(tmp)))
    print(out.strip())


def cmd_pkg_online(dev, name):
    print("[:] Installing '{}' from the repo (device must be on WiFi)...".format(name))
    out = dev.exec_("import sys\n"
                    "if '/Core' not in sys.path: sys.path.append('/Core')\n"
                    "import pkgmgr\n"
                    "print('RESULT', pkgmgr.install_online({}, force=True))".format(_pyrepr(name)),
                    timeout=60)
    print(out.strip())


def cmd_os_update(dev, rpcfile):
    if not os.path.isfile(rpcfile):
        raise DeviceError("No such .rpc file: " + rpcfile)
    cmd_push(dev, rpcfile, '/update.rpc')
    print("[:] Applying update on device (preserving /Users and /Pulsar)...")
    out = dev.exec_("import sys\n"
                    "if '/Core' not in sys.path: sys.path.append('/Core')\n"
                    "import rpc_install\n"
                    "print('RESULT', rpc_install.install_rpc('/update.rpc'))",
                    timeout=90)
    print(out.strip())
    dev.exit_raw()
    dev.reboot()
    print("[@] Update applied; device rebooting.")


def cmd_os_update_online(dev):
    try:
        from urllib.request import urlopen
        import json
    except ImportError:
        raise DeviceError("urllib/json unavailable on this Python.")
    print("[:] Fetching {} ...".format(OFFICIAL_LATEST))
    manifest = json.loads(urlopen(OFFICIAL_LATEST, timeout=20).read().decode())
    url = manifest['url']
    ver = manifest.get('version', '?')
    print("[:] Latest is {} - downloading {} ...".format(ver, url))
    data = urlopen(url, timeout=60).read()
    tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), '_latest.rpc')
    open(tmp, 'wb').write(data)
    print("[@] Downloaded {} B to {}".format(len(data), tmp))
    try:
        cmd_os_update(dev, tmp)
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass


def cmd_shell(dev, command):
    """Run one RPCortex shell command via the live engine (advanced)."""
    code = ("import sys\n"
            "if '/Core' not in sys.path: sys.path.append('/Core')\n"
            "from Core import launchpad as L\n"
            "L.load_commands()\n"
            "L._run_line({})".format(_pyrepr(command)))
    out = dev.exec_(code, timeout=40)
    if out.strip():
        print(out.rstrip())


# ===========================================================================
# CLI
# ===========================================================================

def list_serial_ports():
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    print("Serial ports:")
    for p in ports:
        print("  {:<12} {}".format(p.device, p.description))


def main():
    ap = argparse.ArgumentParser(
        prog='rpc_comm.py', description='RPCortex Communicator (serial companion tool)')
    ap.add_argument('--port', help='Serial port, e.g. COM7 or /dev/ttyACM0')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('-q', '--quiet', action='store_true', help='Less chatter')
    sub = ap.add_subparsers(dest='cmd')

    sub.add_parser('ports', help='List serial ports')
    sub.add_parser('info', help='Show OS version + platform')

    p = sub.add_parser('reg', help='Registry get/set')
    p.add_argument('action', choices=['get', 'set'])
    p.add_argument('key')
    p.add_argument('value', nargs='?')

    sub.add_parser('time-sync', help="Set device clock to this PC's local time")

    p = sub.add_parser('ls'); p.add_argument('path', nargs='?', default='/')
    p = sub.add_parser('push'); p.add_argument('local'); p.add_argument('devpath')
    p = sub.add_parser('pull'); p.add_argument('devpath'); p.add_argument('local')
    p = sub.add_parser('rm'); p.add_argument('devpath')
    p = sub.add_parser('pkg-install'); p.add_argument('pkgfile')
    p = sub.add_parser('pkg-online'); p.add_argument('name')
    p = sub.add_parser('os-update'); p.add_argument('rpcfile')
    sub.add_parser('os-update-online', help='Download the latest OS image and apply it')
    p = sub.add_parser('shell'); p.add_argument('command')

    args = ap.parse_args()

    if args.cmd == 'ports' or args.cmd is None and not args.port:
        if args.cmd == 'ports':
            list_serial_ports()
            return
        ap.print_help()
        return

    if not args.port:
        sys.exit("--port is required (run 'rpc_comm.py ports' to list them).")

    dev = Device(args.port, args.baud, verbose=not args.quiet)
    try:
        dev.enter_raw()
        c = args.cmd
        if c == 'info':                 cmd_info(dev)
        elif c == 'reg':
            if args.action == 'get':    cmd_reg_get(dev, args.key)
            else:
                if args.value is None:  sys.exit("reg set needs a value.")
                cmd_reg_set(dev, args.key, args.value)
        elif c == 'time-sync':          cmd_time_sync(dev)
        elif c == 'ls':                 cmd_ls(dev, args.path)
        elif c == 'push':               cmd_push(dev, args.local, args.devpath)
        elif c == 'pull':               cmd_pull(dev, args.devpath, args.local)
        elif c == 'rm':                 cmd_rm(dev, args.devpath)
        elif c == 'pkg-install':        cmd_pkg_install(dev, args.pkgfile)
        elif c == 'pkg-online':         cmd_pkg_online(dev, args.name)
        elif c == 'os-update':          cmd_os_update(dev, args.rpcfile)
        elif c == 'os-update-online':   cmd_os_update_online(dev)
        elif c == 'shell':              cmd_shell(dev, args.command)
        else:
            ap.print_help()
        if c not in ('os-update', 'os-update-online'):
            dev.exit_raw()
    except DeviceError as e:
        sys.exit("[-] " + str(e))
    except KeyboardInterrupt:
        sys.exit("\n[-] Interrupted.")
    finally:
        dev.close()


if __name__ == '__main__':
    main()
