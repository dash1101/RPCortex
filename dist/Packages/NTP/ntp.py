# Desc: NTP — network time sync for RPCortex (no RTC required)
# File: /Packages/NTP/ntp.py
# Version: 1.0.0
# Author: dash1101
#
# Sets the system clock from an internet time server over UDP, so log
# timestamps and `date` are correct even on boards with no battery-backed RTC.
# Pair it with a startup task to re-sync on every boot:  startup add ntp sync
#
# Usage:
#   ntp                       sync from the configured server (default pool.ntp.org)
#   ntp sync [-s] [server]    sync now; -s = silent unless error
#   ntp status                show the current clock (UTC) and configured server
#   ntp server <host>         set the default NTP server (saved in the registry)
#
# The clock is set in UTC; the `date` command applies System.TZ_Offset for
# local display, so set your offset with:  reg set System.TZ_Offset <hours>

import sys

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import error, info, ok, warn, multi

_DEFAULT_SERVER = 'pool.ntp.org'
_REG_SERVER     = 'Apps.NTP_Server'


def _epoch_delta():
    """Seconds between the NTP epoch (1900) and this build's time epoch.

    Embedded MicroPython uses a 2000-01-01 epoch; CPython/unix use 1970.
    """
    import time
    year = time.gmtime(0)[0]
    if year == 1970:
        return 2208988800
    return 3155673600   # 2000-01-01 epoch (RP2040/RP2350/ESP32)


def _query(host):
    """Send an NTP request to host and return seconds since the local epoch."""
    import socket
    try:
        import struct
    except ImportError:
        import ustruct as struct

    pkt = bytearray(48)
    pkt[0] = 0x1B   # LI=0, Version=3, Mode=3 (client)

    addr = socket.getaddrinfo(host, 123)[0][-1]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.settimeout(5)
        s.sendto(pkt, addr)
        msg = s.recv(48)
    finally:
        s.close()

    if not msg or len(msg) < 44:
        raise OSError("short NTP reply")
    secs = struct.unpack('!I', msg[40:44])[0]   # transmit timestamp (seconds)
    return secs - _epoch_delta()


def _set_clock(secs):
    """Set the hardware RTC (in UTC) from a local-epoch timestamp."""
    import machine
    import time
    tm = time.gmtime(secs)
    # RTC tuple: (year, month, day, weekday, hours, minutes, seconds, subsec)
    machine.RTC().datetime((tm[0], tm[1], tm[2], tm[6], tm[3], tm[4], tm[5], 0))


def _server(override):
    if override:
        return override
    try:
        import regedit
        v = regedit.read(_REG_SERVER)
        if v:
            return v
    except Exception:
        pass
    return _DEFAULT_SERVER


def _online():
    """True if WiFi is up; prints guidance and returns False otherwise."""
    try:
        import net
    except ImportError:
        return True   # no net module — let the socket call try anyway
    if not net.is_available():
        error("WiFi not available on this board.")
        return False
    try:
        if not net.status().get('connected'):
            error("Not connected to WiFi. Run: wifi connect")
            return False
    except Exception:
        pass
    return True


def _sync(server, silent=False):
    """Sync clock. silent=True: print nothing on success, only errors."""
    host = _server(server)
    if not _online():
        return
    if not silent:
        info("Syncing time from {} ...".format(host))
    try:
        secs = _query(host)
    except Exception as e:
        error("NTP sync failed: {}".format(e))
        if not silent:
            info("Check WiFi, or try another server: ntp sync time.google.com")
        return
    try:
        _set_clock(secs)
    except Exception as e:
        error("Could not set RTC: {}".format(e))
        return
    if not silent:
        import time
        t = time.gmtime(secs)
        ok("Clock synced (UTC): {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}".format(
            t[0], t[1], t[2], t[3], t[4], t[5]))
        info("Local time uses System.TZ_Offset — see 'date'.")


def _status():
    import time
    t = time.localtime()
    multi("  Current clock : {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}".format(
        t[0], t[1], t[2], t[3], t[4], t[5]))
    multi("  NTP server    : {}".format(_server(None)))
    multi("  Sync now      : ntp sync")


def ntp(args=None):
    if not args or not args.strip():
        _sync(None)
        return
    parts = args.split(None, 1)
    sub  = parts[0].lower()
    rest = parts[1].strip() if len(parts) > 1 else ''

    if sub == 'sync':
        # Parse optional -s / --silent flag
        silent = False
        server = rest or None
        if server and (server.startswith('-s') or server.startswith('--silent')):
            silent = True
            server = server.split(None, 1)[1].strip() if ' ' in server else None
        _sync(server, silent=silent)
    elif sub == 'status':
        _status()
    elif sub == 'server':
        if not rest:
            warn("Usage: ntp server <host>")
            return
        try:
            import regedit
            regedit.save(_REG_SERVER, rest)
            ok("NTP server set to '{}'.".format(rest))
        except Exception as e:
            error("Could not save server: {}".format(e))
    else:
        # Treat `ntp <host>` as a sync from that host
        _sync(args.strip())
