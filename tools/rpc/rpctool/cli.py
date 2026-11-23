"""rpc: drive an RPCortex v2 device from a computer, over its USB serial console.

Exit status: 0 success; 1 failure (for exec, the command's own status);
2 a usage mistake; 124 an exec that ran past --timeout; 130 Ctrl+C.
"""
import argparse
import getpass
import os
import posixpath
import re
import sys
import time

from . import __version__
from . import backup as _backup
from . import flash as _flash
from . import repo as _repo
from . import term as _term
from . import transport
from . import web as _web
from .device import Device
from .errors import LinkError, NotSupported, RpcError
from .link import Timing
from .pkg import install_bytes
from .progress import Progress
from .util import human, quote, rate

EXIT_TIMEOUT = 124
EXIT_INTERRUPTED = 130


# --- output -----------------------------------------------------------------

def out(text=""):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def out_bytes(data):
    buf = getattr(sys.stdout, "buffer", None)
    if buf is not None:
        sys.stdout.flush()
        buf.write(data)
        buf.flush()
    else:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()


def err(text):
    sys.stderr.write(text + "\n")
    sys.stderr.flush()


class Ctx(object):
    """The connection, opened the first time a command needs it."""

    def __init__(self, args, timing=None):
        self.args = args
        self.timing = timing or Timing()
        self.dev = None

    def note(self, text):
        if not getattr(self.args, "quiet", False):
            err(text)

    def _noise(self, line):
        if line.strip() and not getattr(self.args, "quiet", False):
            err("[device] " + line)

    def _password(self):
        env = os.environ.get("RPC_PASSWORD")
        if env:
            return env
        if sys.stdin.isatty():
            return getpass.getpass("Password for {} on the device: ".format(self.args.user))
        return None

    def _sudo_password(self):
        """The signed-in account's password, for sudo, asked for at the
        terminal. ($RPC_PASSWORD, when it is set, is used instead; it is the
        same account's.)"""
        if sys.stdin.isatty():
            who = self.dev.hello.get("user", "this account") if self.dev else "this account"
            return getpass.getpass("Password for {} on the device (sudo): ".format(who))
        return None

    def port(self):
        return transport.choose_port(getattr(self.args, "port", None))

    def device(self):
        if self.dev is None:
            path = self.port()
            ser = transport.open_serial(path)
            # A password from the environment is a string, tried once; one
            # typed is asked for again if the device refuses it.
            dev = Device(transport.Transport(ser, path), user=getattr(self.args, "user", None),
                         password=self._password, timing=self.timing, on_noise=self._noise,
                         sudo_password=os.environ.get("RPC_PASSWORD") or self._sudo_password)
            try:
                dev.connect()
            except BaseException:
                dev.close()
                raise
            self.dev = dev
            if dev.mode == "legacy":
                self.note("note: this firmware has no link mode; working through the prompt "
                          "(legacy: no exit status from files, no get, ls or stat)")
            if getattr(self.args, "sudo", False):
                # Up front, so a long job is not stopped half way to ask.
                if dev.has_sudo:
                    dev.authenticate()
                else:
                    self.note("note: this firmware has no sudo; --sudo does nothing")
        return self.dev

    def progress(self, label):
        quiet = getattr(self.args, "quiet", False)
        return Progress(label, enabled=False if quiet else None)

    def close(self):
        if self.dev is not None:
            self.dev.close()
            self.dev = None


# --- helpers ----------------------------------------------------------------

def _read_local(path):
    with open(path, "rb") as f:
        return f.read()


def _put_one(ctx, dev, data, remote, label):
    p = ctx.progress(label)
    t0 = time.monotonic()
    try:
        digest = dev.put(data, remote, p)
    finally:
        p.done()
    secs = time.monotonic() - t0
    ctx.note("{} -> {}  {}  sha256 verified  ({:.1f} s, {})".format(
        label, remote, human(len(data)), secs, rate(len(data), secs)))
    return digest


def _get_one(ctx, dev, remote, size=None):
    p = ctx.progress(posixpath.basename(remote) or remote)
    t0 = time.monotonic()
    try:
        data = dev.get(remote, p)
    finally:
        p.done()
    secs = time.monotonic() - t0
    return data, secs


def _remote_target(dev, remote, name):
    """Where `name` lands when sent to `remote` (a directory or a file path)."""
    if remote is None:
        return "/" + name
    if remote.endswith("/"):
        return dev.abspath(remote + name)
    path = dev.abspath(remote)
    if dev.mode == "link" and dev.is_dir(path):
        return posixpath.join(path, name)
    return path


def _print_exec(r):
    out_bytes(r.output)


# --- commands ---------------------------------------------------------------

def cmd_ports(ctx, a):
    try:
        cands = transport.list_candidates()
    except RpcError as e:
        err("error: {}".format(e))
        cands = []
    for p in cands:
        out(p.label())
    drives = _flash.find_drives()
    for d in drives:
        out("{:<16} BOOTSEL drive ({})".format(d, _flash.drive_chip(d) or "unknown chip"))
    if not cands and not drives:
        err("No RPCortex device found. Is it plugged in with a data cable?")
        return 1
    return 0


_KB = r"\s*:?\s*(\d+)\s*KB"


def _num(pattern, text):
    m = re.search(pattern, text)
    return int(m.group(1)) if m else None


def cmd_info(ctx, a):
    dev = ctx.device()
    out("{:<10}{}".format("port", dev.t.name))
    out("{:<10}{}".format("mode", "link (protocol {})".format(dev.hello.get("proto", "?"))
                          if dev.mode == "link" else "legacy (firmware without link mode)"))
    if dev.mode == "link":
        order = ["os", "build", "board", "user", "cwd", "chunk", "exec_max"]
        for k in order + sorted(k for k in dev.hello if k not in order and k != "proto"):
            if k in dev.hello:
                out("{:<10}{}".format(k, dev.hello[k]))
    else:
        ver = dev.exec("ver").text.strip().splitlines()
        if ver:
            out("{:<10}{}".format("version", ver[0]))
    mem = dev.exec("meminfo").text
    total, free, big = _num("Total" + _KB, mem), _num("Free" + _KB, mem), _num("Largest block" + _KB, mem)
    if free is not None and total is not None:
        out("{:<10}{} KB free of {} KB{}".format("memory", free, total,
            ", largest block {} KB".format(big) if big is not None else ""))
    df = dev.exec("df").text
    total, used, free = _num("Total" + _KB, df), _num(r"\((\d+)%\)", df), _num("Free" + _KB, df)
    if free is not None and total is not None:
        out("{:<10}{} KB free of {} KB{}".format("storage", free, total,
            " ({}% used)".format(used) if used is not None else ""))
    return 0


def cmd_term(ctx, a):
    path = ctx.port()
    ser = transport.open_serial(path)
    t = transport.Transport(ser, path)
    try:
        _term.run(t)
    finally:
        t.close()
    return 0


def cmd_exec(ctx, a):
    line = " ".join(a.command)
    dev = ctx.device()
    r = dev.exec(line, a.timeout)
    _print_exec(r)
    if r.truncated:
        err("[output cut off at {} bytes by the device]".format(dev.link.exec_max if dev.link else "?"))
    if r.timed_out:
        err("[timed out after {:g} s; interrupted with Ctrl+C]".format(a.timeout))
        return EXIT_TIMEOUT
    return r.rc


def cmd_ls(ctx, a):
    dev = ctx.device()
    path = a.path or "."
    if dev.mode != "link":
        r = dev.exec("ls " + quote(path))
        _print_exec(r)
        return r.rc
    ents = dev.ls(path)
    ents.sort(key=lambda e: (not e.is_dir, e.name.lower()))
    for e in ents:
        if e.is_dir:
            out("d  {:>9}  {}/".format("-", e.name))
        else:
            out("f  {:>9}  {}".format(e.size, e.name))
    return 0


def cmd_stat(ctx, a):
    dev = ctx.device()
    path = dev.abspath(a.path)
    st = dev.stat(path)
    if st is None:
        err("{}: no such file or directory".format(path))
        return 1
    if st[0] == "d":
        out("{}  directory".format(path))
    else:
        out("{}  file  {} bytes".format(path, st[1]))
    return 0


def _push_tree(ctx, dev, local, remote):
    failed = 0
    local = os.path.abspath(local)
    for dirpath, dirnames, filenames in os.walk(local):
        dirnames.sort()
        rel = os.path.relpath(dirpath, local)
        rdir = remote if rel == "." else posixpath.join(remote, *rel.split(os.sep))
        dev.mkdir(rdir, parents=True)
        for fn in sorted(filenames):
            lp = os.path.join(dirpath, fn)
            try:
                _put_one(ctx, dev, _read_local(lp), posixpath.join(rdir, fn), os.path.relpath(lp))
            except (LinkError, OSError) as e:
                failed += 1
                err("could not send {}: {}".format(lp, e))
    return 1 if failed else 0


def cmd_put(ctx, a):
    local = a.local
    if os.path.isdir(local):
        if not a.recursive:
            err("{} is a directory; use -r to send it and everything in it".format(local))
            return 1
        dev = ctx.device()
        base = os.path.basename(os.path.abspath(local))
        remote = dev.abspath(a.remote) if a.remote else "/" + base
        return _push_tree(ctx, dev, local, remote)
    data = _read_local(local)
    dev = ctx.device()
    remote = _remote_target(dev, a.remote, os.path.basename(local))
    _put_one(ctx, dev, data, remote, local)
    return 0


def _write_local(path, data):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def cmd_get(ctx, a):
    dev = ctx.device()
    remote = dev.abspath(a.remote)
    st = dev.stat(remote)
    if st is None:
        err("{}: no such file or directory".format(remote))
        return 1
    if st[0] == "d":
        if not a.recursive:
            err("{} is a directory; use -r to fetch it and everything in it".format(remote))
            return 1
        base = a.local or (posixpath.basename(remote.rstrip("/")) or "device")
        failed = 0
        for d, ents in dev.walk(remote):
            rel = posixpath.relpath(d, remote)
            ldir = base if rel == "." else os.path.join(base, *rel.split("/"))
            os.makedirs(ldir, exist_ok=True)
            for e in ents:
                if e.is_dir:
                    continue
                rp = posixpath.join(d, e.name)
                try:
                    data, secs = _get_one(ctx, dev, rp, e.size)
                    _write_local(os.path.join(ldir, e.name), data)
                    ctx.note("{} -> {}  {}  sha256 verified".format(
                        rp, os.path.join(ldir, e.name), human(len(data))))
                except LinkError as ex:
                    failed += 1
                    err("could not fetch {}: {}".format(rp, ex))
        return 1 if failed else 0
    name = posixpath.basename(remote)
    local = a.local or name
    if os.path.isdir(local):
        local = os.path.join(local, name)
    data, secs = _get_one(ctx, dev, remote, st[1])
    _write_local(local, data)
    ctx.note("{} -> {}  {}  sha256 verified  ({:.1f} s, {})".format(
        remote, local, human(len(data)), secs, rate(len(data), secs)))
    return 0


def cmd_rm(ctx, a):
    dev = ctx.device()
    failed = 0
    for p in a.paths:
        try:
            r = dev.rm(p, a.recursive)
            _print_exec(r)
        except RpcError as e:
            failed += 1
            err("{}: {}".format(p, e))
    return 1 if failed else 0


def cmd_mkdir(ctx, a):
    dev = ctx.device()
    failed = 0
    for p in a.paths:
        try:
            dev.mkdir(p, parents=a.parents)
        except RpcError as e:
            failed += 1
            err("{}: {}".format(p, e))
    return 1 if failed else 0


def cmd_mv(ctx, a):
    dev = ctx.device()
    r = dev.mv(a.src, a.dst)
    _print_exec(r)
    return 0


def cmd_cat(ctx, a):
    dev = ctx.device()
    for p in a.paths:
        if dev.mode != "link":
            r = dev.exec("cat " + quote(p))
            _print_exec(r)
            if r.rc:
                return r.rc
            continue
        out_bytes(dev.get(dev.abspath(p)))
    return 0


def _board_arch(dev):
    board = dev.hello.get("board")
    # The device says which build it runs, from the firmware that added it.
    if dev.hello.get("arch") in ("armv6m", "armv8m"):
        return dev.hello["arch"], board
    if not board and dev.mode != "link":
        m = re.search(r"Platform\s*:\s*(\S+)", dev.exec("ver").text)
        board = m.group(1) if m else None
    return _repo.board_arch(board), board


def cmd_install(ctx, a):
    if a.list:
        return _list_index(ctx, a)
    if not a.targets:
        err("name a package file or a package to install (or --list)")
        return 2
    dev = ctx.device()
    index = None
    failed = 0
    for target in a.targets:
        if os.path.isfile(target) or target.lower().endswith(".app"):
            if not os.path.isfile(target):
                err("{}: no such file".format(target))
                failed += 1
                continue
            data = _read_local(target)
            name = os.path.basename(target)
            ctx.note("Installing {} ({})".format(name, human(len(data))))
        else:
            if index is None:
                index = _repo.load_index(a.index)
            arch, board = _board_arch(dev)
            arch = a.arch or arch
            if arch is None:
                # `pkg install <file>` does not check the architecture, so this
                # is the only thing standing between ARMv8-M code and an RP2040.
                raise RpcError("rpc does not know which architecture the board '{}' runs; "
                               "say with --arch armv6m (RP2040) or --arch armv8m (RP2350)"
                               .format(board or "?"))
            pkg = _repo.choose(index, target, arch)
            ctx.note("Installing {} {} ({}) for {}".format(
                pkg.name, pkg.ver, pkg.arch or "any architecture", board or arch or "this board"))
            p = ctx.progress("download " + pkg.name)
            try:
                data, verified = _repo.download(pkg, p)
            finally:
                p.done()
            if verified:
                ctx.note("Checksum verified against the index.")
            else:
                err("warning: the index publishes no checksum for {}; it is installed unverified"
                    .format(pkg.name))
            name = pkg.name + ".app"
        p = ctx.progress("send " + name)
        try:
            r = install_bytes(dev, data, name, p)
        finally:
            p.done()
        _print_exec(r)
        if r.rc:
            failed += 1
    return 1 if failed else 0


def _list_index(ctx, a):
    pkgs = _repo.load_index(a.index)
    arch = a.arch or (_repo.board_arch(a.board) if a.board else None)
    for p in sorted(pkgs, key=lambda p: p.name.lower()):
        tail = ""
        if p.kind and p.kind != "app":
            tail += "  [{}]".format(p.kind)
        if arch and not _repo.arch_runs_here(p.arch, arch):
            tail += "  (not for {})".format(arch)
        out("{:<14} {:<8} {:<8} {}{}".format(p.name, p.ver, p.arch or "any", p.desc, tail))
    return 0


def cmd_fetch(ctx, a):
    p = ctx.progress("download")
    try:
        data = _repo.fetch(a.url, p)
    finally:
        p.done()
    dev = ctx.device()
    remote = _remote_target(dev, a.remote, _repo.url_basename(a.url))
    _put_one(ctx, dev, data, remote, _repo.url_basename(a.url))
    return 0


def cmd_wget(ctx, a):
    dev = ctx.device()
    line = "wget " + quote(a.url)
    if a.remote:
        line += " " + quote(a.remote)
    r = dev.exec(line, a.timeout)
    _print_exec(r)
    if r.timed_out:
        return EXIT_TIMEOUT
    return r.rc


def cmd_backup(ctx, a):
    dev = ctx.device()
    if dev.mode != "link":
        raise NotSupported("backup needs link mode, which this firmware does not have")
    log = (lambda s: None) if a.quiet else (lambda s: err(s))
    n, size, skipped = _backup.backup(
        dev, a.dest, a.with_packages, log=log, warn=lambda s: err("warning: " + s),
        progress=lambda path, sz: ctx.progress(posixpath.basename(path)),
        with_system=a.with_system)
    out("Backed up {} file{}, {}, to {}{}".format(
        n, "" if n == 1 else "s", human(size), a.dest,
        "  ({} skipped)".format(skipped) if skipped else ""))
    out("Not included:")
    for what, option in _backup.not_included(a.with_packages, a.with_system):
        out("  {}{}".format(what, "  (add with {})".format(option) if option else ""))
    return 1 if skipped else 0


def _confirm_system(a):
    def ask(notice):
        err(notice)
        if a.yes:
            return True
        if not sys.stdin.isatty():
            err("Not asked, since nothing here can answer; --yes restores them, "
                "--skip-system leaves them.")
            return False
        sys.stderr.write("Replace them? [y/N] ")
        sys.stderr.flush()
        return sys.stdin.readline().strip().lower() in ("y", "yes")
    return ask


def cmd_restore(ctx, a):
    dev = ctx.device()
    log = (lambda s: None) if a.quiet else (lambda s: err(s))
    done, failed = _backup.restore(
        dev, a.src, packages=not a.skip_packages, dry_run=a.dry_run, log=log,
        warn=lambda s: err("warning: " + s),
        progress=lambda path, sz: ctx.progress(posixpath.basename(path)),
        system=not a.skip_system, confirm=_confirm_system(a))
    out("{} {} item{}{}".format("Would restore" if a.dry_run else "Restored", done,
                                "" if done == 1 else "s",
                                ", {} failed".format(failed) if failed else ""))
    if dev.gone:
        out("The device is restarting with the restored settings.")
    return 1 if failed else 0


def cmd_reboot(ctx, a):
    dev = ctx.device()
    path = dev.t.name
    r = dev.reboot()
    if r is not None:
        _print_exec(r)
        return r.rc or 1
    ctx.close()
    out("Rebooting.")
    if a.wait:
        end = time.monotonic() + a.wait
        time.sleep(2.0)
        while time.monotonic() < end:
            try:
                s = transport.open_serial(path)
                s.close()
                out("Back on {}.".format(path))
                return 0
            except RpcError:
                time.sleep(1.0)
        err("{} did not come back within {} s".format(path, a.wait))
        return 1
    return 0


def _exec_simple(command):
    def run(ctx, a):
        r = ctx.device().exec(command)
        _print_exec(r)
        return r.rc
    return run


def cmd_flash(ctx, a):
    data = _read_local(a.file)
    chip = _flash.uf2_chip(data)
    drive = a.drive
    port = None
    if not drive and not a.no_reset:
        try:
            port = ctx.port()
        except RpcError:
            port = None
        if port is None:
            present = _flash.find_drives()
            if not present:
                err("No device found, and no BOOTSEL drive is mounted. Hold BOOTSEL while "
                    "plugging the board in, then run this again with --no-reset.")
                return 1
            drive = present[0]
        else:
            ctx.note("Resetting {} into BOOTSEL (1200 baud)...".format(port))
            transport.touch_1200(port)
    if not drive:
        ctx.note("Waiting for the BOOTSEL drive...")
        drive = _flash.wait_for_drive(a.wait)
    if not drive:
        if sys.platform.startswith("linux") and _flash.picotool_load(a.file):
            out("Loaded {} with picotool.".format(a.file))
            return 0
        if _flash.bootsel_on_usb():
            err("The board is in BOOTSEL, but nothing mounted its drive. Mount the drive "
                "labelled RP2350 or RPI-RP2 (or install picotool) and run "
                "'rpc flash {} --no-reset'.".format(a.file))
        else:
            err("No BOOTSEL drive appeared. Hold BOOTSEL while plugging the board in, "
                "then run 'rpc flash {} --no-reset'.".format(a.file))
        return 1
    dchip = _flash.drive_chip(drive)
    if chip and dchip and chip != dchip and not a.force:
        err("{} is built for {}, but the board on {} is an {}. Nothing was written."
            .format(a.file, chip, drive, dchip))
        return 1
    _flash.copy_uf2(data, drive, os.path.basename(a.file))
    out("Wrote {} ({}) to {}. The board restarts on its own; the files on it are kept."
        .format(os.path.basename(a.file), human(len(data)), drive))
    if a.no_wait_port:
        return 0
    end = time.monotonic() + 30
    while time.monotonic() < end:
        try:
            back = [p for p in transport.list_candidates() if p.rpcortex]
        except RpcError:
            break
        if back:
            out("Back on {}.".format(back[0].device))
            return 0
        time.sleep(1.0)
    ctx.note("The console has not come back yet; give it a moment ('rpc ports').")
    return 0


def cmd_web(ctx, a):
    d = _web.find_web_dir(a.dir)
    return _web.serve(d, a.web_port, open_browser=not a.no_browser)


def cmd_watch(ctx, a):
    dev = ctx.device()
    line = " ".join(a.command)
    tty = sys.stdout.isatty()
    try:
        while True:
            r = dev.exec(line, a.timeout)
            if tty:
                sys.stdout.write("\x1b[H\x1b[2J")
            out("every {:g} s: {}    {}  rc={}".format(a.every, line, time.strftime("%H:%M:%S"), r.rc))
            out()
            _print_exec(r)
            if a.count:
                a.count -= 1
                if a.count == 0:
                    return 0
            time.sleep(a.every)
    except KeyboardInterrupt:
        return 0


# --- the parser -------------------------------------------------------------

def _common(p, suppress, short_port=True, port=True):
    d = (lambda v: argparse.SUPPRESS) if suppress else (lambda v: v)
    if port:
        flags = ("-p", "--port") if short_port else ("--port",)
        p.add_argument(*flags, default=d(None),
                       help="serial port (default: $RPC_PORT, else the one RPCortex device found)")
    p.add_argument("-u", "--user", default=d(None),
                   help="log in as this account if the device is at a login prompt "
                        "(password asked for, or $RPC_PASSWORD)")
    p.add_argument("-q", "--quiet", action="store_true", default=d(False),
                   help="no progress or notes, only results and errors")
    p.add_argument("--sudo", action="store_true", default=d(False),
                   help="give the device the password for sudo before starting (it is "
                        "otherwise asked for when first needed; $RPC_PASSWORD is used if set)")


def build_parser():
    ap = argparse.ArgumentParser(
        prog="rpc", description="Drive an RPCortex v2 device over its USB serial console.",
        epilog="'rpc <command> --help' describes each command. Exit status: 0 ok, 1 failed, "
               "2 usage, 124 exec timed out, 130 interrupted.")
    ap.add_argument("--version", action="version", version="rpctool " + __version__)
    _common(ap, False)
    sub = ap.add_subparsers(dest="cmd", metavar="<command>")
    common = argparse.ArgumentParser(add_help=False)
    _common(common, True)
    no_short = argparse.ArgumentParser(add_help=False)       # mkdir keeps -p for --parents
    _common(no_short, True, short_port=False)
    no_port = argparse.ArgumentParser(add_help=False)        # web's --port is the HTTP port
    _common(no_port, True, port=False)

    def add(name, func, help_, aliases=(), parent=common):
        p = sub.add_parser(name, help=help_, description=help_, parents=[parent],
                           aliases=list(aliases))
        p.set_defaults(func=func)
        return p

    add("ports", cmd_ports, "list connected devices and BOOTSEL drives")
    add("info", cmd_info, "show the device's version, board, memory and storage")
    add("term", cmd_term, "an interactive terminal on the console (Ctrl+] quits)",
        aliases=("shell",))

    p = add("exec", cmd_exec, "run a command line and exit with its status")
    p.add_argument("command", nargs="+", help="the command line (quote it to keep ; | > for the device)")
    p.add_argument("-t", "--timeout", type=float, default=None,
                   help="send Ctrl+C after this many seconds (exit status 124)")

    p = add("ls", cmd_ls, "list a directory")
    p.add_argument("path", nargs="?", default=None)

    p = add("stat", cmd_stat, "show whether a path is a file or a directory, and its size")
    p.add_argument("path")

    p = add("put", cmd_put, "send a file (or with -r a directory) to the device")
    p.add_argument("local")
    p.add_argument("remote", nargs="?", default=None,
                   help="path on the device (default: /<name>); a directory puts it inside")
    p.add_argument("-r", "--recursive", action="store_true")

    p = add("get", cmd_get, "fetch a file (or with -r a directory) from the device")
    p.add_argument("remote")
    p.add_argument("local", nargs="?", default=None, help="default: its name, here")
    p.add_argument("-r", "--recursive", action="store_true")

    p = add("rm", cmd_rm, "remove files (-r: directories and what is in them)")
    p.add_argument("paths", nargs="+")
    p.add_argument("-r", "--recursive", action="store_true")

    p = add("mkdir", cmd_mkdir, "make directories", parent=no_short)
    p.add_argument("paths", nargs="+")
    p.add_argument("-p", "--parents", action="store_true",
                   help="make missing parents too; an existing directory is fine")

    p = add("mv", cmd_mv, "move or rename a file or directory")
    p.add_argument("src")
    p.add_argument("dst")

    p = add("cat", cmd_cat, "print files")
    p.add_argument("paths", nargs="+")

    p = add("install", cmd_install, "install a package file (.app) or a package from the repository")
    p.add_argument("targets", nargs="*", metavar="file.app|name")
    p.add_argument("--list", action="store_true", help="show what the repository offers")
    p.add_argument("--index", default=None,
                   help="package index URL or file (default: the official repository)")
    p.add_argument("--arch", choices=("armv6m", "armv8m"), default=None,
                   help="override the architecture taken from the board")
    p.add_argument("--board", default=None, help="with --list: mark what does not run on this board")

    p = add("fetch", cmd_fetch, "download a URL on this computer and send it to the device")
    p.add_argument("url")
    p.add_argument("remote", nargs="?", default=None, help="default: /<name from the URL>")

    p = add("wget", cmd_wget, "have the device download a URL itself (it must be online)")
    p.add_argument("url")
    p.add_argument("remote", nargs="?", default=None)
    p.add_argument("-t", "--timeout", type=float, default=None)

    p = add("backup", cmd_backup, "copy the device's files to a directory or a .zip")
    p.add_argument("dest", metavar="dir|file.zip")
    p.add_argument("--with-packages", action="store_true",
                   help="include the installed packages (/os/pkg/*.app)")
    p.add_argument("--with-system", action="store_true",
                   help="include the settings and Wi-Fi networks, the accounts, the device key "
                        "and the SSH host key (needs sudo)")

    p = add("restore", cmd_restore, "put a backup back on the device")
    p.add_argument("src", metavar="dir|file.zip")
    p.add_argument("--skip-packages", action="store_true",
                   help="do not reinstall packages the backup holds")
    p.add_argument("--skip-system", action="store_true",
                   help="leave the system files as they are, even if the backup holds them")
    p.add_argument("-y", "--yes", action="store_true",
                   help="replace the system files without asking")
    p.add_argument("-n", "--dry-run", action="store_true", help="say what would be done")

    p = add("reboot", cmd_reboot, "restart the device")
    p.add_argument("--wait", type=float, nargs="?", const=60.0, default=None,
                   help="wait (up to this many seconds, default 60) for the port to come back")

    add("log", _exec_simple("logdump"), "show the device's log (logdump)")
    add("ps", _exec_simple("ps"), "list the device's tasks")

    p = add("flash", cmd_flash, "write a firmware .uf2 through the BOOTSEL drive (files are kept)")
    p.add_argument("file", metavar="file.uf2")
    p.add_argument("--no-reset", action="store_true",
                   help="the board is already in BOOTSEL; do not reset it")
    p.add_argument("--drive", default=None, help="the BOOTSEL drive's mount point")
    p.add_argument("--wait", type=float, default=20.0, help="seconds to wait for the drive")
    p.add_argument("--force", action="store_true", help="write even if the chip does not match")
    p.add_argument("--no-wait-port", action="store_true",
                   help="do not wait for the console to come back")

    p = add("web", cmd_web, "serve the browser tool on localhost and open it", parent=no_port)
    p.add_argument("--port", dest="web_port", type=int, default=8765,
                   help="the HTTP port on localhost (default 8765)")
    p.add_argument("--dir", default=None, help="the tools/web directory to serve")
    p.add_argument("--no-browser", action="store_true")

    p = add("watch", cmd_watch, "run a command again every few seconds")
    p.add_argument("command", nargs="+")
    p.add_argument("-n", "--every", type=float, default=2.0, help="seconds between runs (default 2)")
    p.add_argument("-c", "--count", type=int, default=0, help="stop after this many runs")
    p.add_argument("-t", "--timeout", type=float, default=None)
    return ap


def main(argv=None, timing=None):
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = build_parser()
    a = ap.parse_args(argv)
    if not getattr(a, "func", None):
        ap.print_help()
        return 2
    ctx = Ctx(a, timing)
    try:
        return a.func(ctx, a) or 0
    except KeyboardInterrupt:
        err("interrupted")
        return EXIT_INTERRUPTED
    except LinkError as e:
        err("error: {} [{}]".format(e.message or e.code, e.code))
        return 1
    except RpcError as e:
        err("error: {}".format(e))
        return 1
    except BrokenPipeError:
        # `rpc ... | head`: the reader has gone, which is not an error here.
        try:
            os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        except (OSError, AttributeError, ValueError):
            pass
        return 0
    except OSError as e:
        err("error: {}".format(e))
        return 1
    finally:
        try:
            ctx.close()
        except KeyboardInterrupt:
            pass
