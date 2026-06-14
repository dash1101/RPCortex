# Desc: NTP — network time sync for RPCortex (no RTC required)
# File: /Packages/NTP/ntp.py
# Version: 1.1.0
# Author: dash1101
#
# Sets the system clock from an internet time server over UDP, so log
# timestamps and `date` are correct even on boards with no battery-backed RTC.
# Pair it with a startup task to re-sync on every boot:  startup add ntp sync
#
# Usage:
#   ntp                  sync from the configured server (default pool.ntp.org)
#   ntp sync [server]    sync now, optionally from a specific server
#   ntp status           show the current clock (UTC) and configured server
#   ntp server <host>    set the default NTP server (saved in the registry)
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


def _tz_offset():
    """Return System.TZ_Offset as an int (hours), or 0 if unset/invalid."""
    try:
        import regedit
        return int(regedit.read('System.TZ_Offset') or 0)
    except Exception:
        return 0


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


def _parse_sync_flags(rest):
    """Split a sync arg-string into (server_or_None, silent, auto).
    Strips -s/--silent and -a/--auto; the first non-flag token is the server."""
    server = None
    silent = False
    auto = False
    for tok in (rest or '').split():
        if tok in ('-s', '--silent'):
            silent = True
        elif tok in ('-a', '--auto'):
            auto = True
        elif server is None:
            server = tok
    return server, silent, auto


def _auto_tz(silent=False):
    """Set System.TZ_Offset automatically from the device's public-IP location.

    Uses ip-api.com (HTTP, no key) which returns the current UTC offset in
    seconds (DST-aware). Stored as whole hours — half-hour zones truncate."""
    try:
        import net
    except ImportError:
        return False
    try:
        status, body = net.wget('http://ip-api.com/json/', verbose=False)
        if status != 200:
            raise ValueError("HTTP {}".format(status))
        import ujson
        data = ujson.loads(body.decode('utf-8') if isinstance(body, (bytes, bytearray)) else body)
        off_s = int(data.get('offset', 0))
        zone  = data.get('timezone', '?')
    except Exception as e:
        if not silent:
            warn("Auto-timezone lookup failed: {}".format(e))
        return False
    hours = int(off_s / 3600)   # whole hours; half-hour zones truncate
    try:
        import regedit
        regedit.save('System.TZ_Offset', str(hours))
    except Exception:
        return False
    if not silent:
        ok("Timezone auto-set: UTC{}{}  ({})".format('+' if hours >= 0 else '', hours, zone))
    return True


def _sync(server, silent=False, auto=False):
    host = _server(server)
    if not _online():
        return
    if not silent:
        info("Syncing time from {} ...".format(host))
    try:
        secs = _query(host)
    except Exception as e:
        error("NTP sync failed: {}".format(e))   # errors always print
        info("Check WiFi, or try another server: ntp sync time.google.com")
        return
    try:
        _set_clock(secs)
    except Exception as e:
        error("Could not set RTC: {}".format(e))
        return
    if auto:
        _auto_tz(silent)   # geolocate + set TZ before reporting local time
    if silent:
        return
    import time
    t = time.gmtime(secs)
    ok("Clock synced (UTC): {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}".format(
        t[0], t[1], t[2], t[3], t[4], t[5]))
    # Also show the local wall-clock time using the configured TZ offset, so the
    # user doesn't have to run `date` to see what their actual time now is.
    off = _tz_offset()
    if off:
        lt = time.gmtime(secs + off * 3600)
        sign = '+' if off >= 0 else '-'
        ok("Local time (UTC{}{}): {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}".format(
            sign, abs(off), lt[0], lt[1], lt[2], lt[3], lt[4], lt[5]))
    else:
        info("Local time = UTC (set a zone with: reg set System.TZ_Offset <hrs>)")


def _status():
    import time
    t = time.localtime()
    multi("  UTC clock     : {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}".format(
        t[0], t[1], t[2], t[3], t[4], t[5]))
    off = _tz_offset()
    if off:
        lt = time.gmtime(time.time() + off * 3600)
        sign = '+' if off >= 0 else '-'
        multi("  Local (UTC{}{}): {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}".format(
            sign, abs(off), lt[0], lt[1], lt[2], lt[3], lt[4], lt[5]))
    else:
        multi("  Local         : = UTC (System.TZ_Offset is 0)")
    multi("  NTP server    : {}".format(_server(None)))
    multi("  Sync now      : ntp sync")


def ntp(args=None):
    if not args or not args.strip():
        _sync(None)
        return
    parts = args.split(None, 1)
    sub  = parts[0].lower()
    rest = parts[1].strip() if len(parts) > 1 else ''

    if sub in ('help', '-h', '--help', '?'):
        info("ntp - set the clock from the internet (NTP)")
        multi("  ntp                  sync time from the default server")
        multi("  ntp sync [-s] [--auto]   sync; -s quiet; --auto sets timezone by IP")
        multi("  ntp <host>           sync from a specific NTP server")
        multi("  ntp server <host>    save a default NTP server")
        multi("  ntp status           show the NTP server + last sync")
        return
    if sub == 'sync':
        server, silent, auto = _parse_sync_flags(rest)
        _sync(server, silent, auto)
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
        # Treat `ntp <host>` (and bare `ntp -s` / `ntp --auto`) as a sync.
        server, silent, auto = _parse_sync_flags(args.strip())
        _sync(server, silent, auto)
