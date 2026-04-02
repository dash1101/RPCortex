# Desc: WiFi management shell commands for RPCortex - Nebula OS
# File: /Core/Launchpad/wifi.py
# Last Updated: 4/1/2026
# Lang: MicroPython, English
# Version: v0.8.1
# Author: dash1101
#
# Loaded once into a cached exec scope by launchpad.py.
# Dispatches 'wifi' subcommands to Core/net.py.

import sys

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import ok, warn, error, info, multi, inpt, masked_inpt

# ---------------------------------------------------------------------------
# Shell entry point
# ---------------------------------------------------------------------------

def wifi(args=None):
    """
    WiFi management.

    Usage:
      wifi status              Show current connection state
      wifi scan                Scan for nearby networks
      wifi connect [ssid]      Connect to a network (prompts for password)
      wifi disconnect          Disconnect from current network
      wifi list                List saved networks
      wifi add <ssid>          Save a new network (prompts for password)
      wifi forget <ssid>       Remove a saved network
    """
    if not args:
        _usage()
        return

    parts = args.strip().split(None, 1)
    sub   = parts[0].lower()
    rest  = parts[1].strip() if len(parts) > 1 else None

    if sub == 'status':
        _status()
    elif sub == 'scan':
        _scan()
    elif sub in ('connect', 'conn'):
        _connect(rest)
    elif sub in ('disconnect', 'disc', 'down'):
        _disconnect()
    elif sub == 'list':
        _list_saved()
    elif sub == 'add':
        _add(rest)
    elif sub == 'forget':
        _forget(rest)
    else:
        warn("Unknown subcommand '{}'.".format(sub))
        _usage()


def _usage():
    info("=== WiFi Commands ===")
    multi("  wifi status              Connection status")
    multi("  wifi scan                Scan for nearby networks")
    multi("  wifi connect [ssid]      Connect to a network")
    multi("  wifi disconnect          Disconnect")
    multi("  wifi list                List saved networks")
    multi("  wifi add <ssid>          Save a network")
    multi("  wifi forget <ssid>       Remove a saved network")


# ---------------------------------------------------------------------------
# Subcommand implementations
# ---------------------------------------------------------------------------

def _status():
    import net
    if not net.is_available():
        warn("WiFi hardware not detected on this board.")
        multi("  Supported: Pico W, ESP32, ESP32-S2, ESP32-S3")
        return

    s = net.status()
    multi("")
    if s['connected']:
        ok("Connected")
        multi("  SSID : {}".format(s['ssid'] or '?'))
        multi("  IP   : {}".format(s['ip']   or '?'))
    elif s['active']:
        warn("Interface active, not connected.")
    else:
        multi("  Interface inactive.")
    multi("")


def _scan():
    import net
    if not net.is_available():
        error("WiFi hardware not available.")
        return

    info("Scanning... (this may take a few seconds)")
    results = net.scan()
    if not results:
        warn("No networks found.")
        return

    multi("")
    multi("  {:<4}  {:<6}  {:<10}  {}".format("RSSI", "CH", "SECURITY", "SSID"))
    multi("  " + "-" * 48)
    for r in results:
        ssid = r['ssid'] or '(hidden)'
        multi("  {:>4}  {:>5d}  {:<10}  {}".format(
            r['rssi'], r['channel'], r['security'], ssid))
    multi("")
    multi("  {} network(s) found.".format(len(results)))
    multi("")


def _connect(ssid_arg):
    import net
    if not net.is_available():
        error("WiFi hardware not available.")
        return

    if ssid_arg:
        ssid = ssid_arg
    else:
        # Try saved networks first
        saved = net.list_saved()
        if saved:
            multi("Saved networks:")
            for slot, s in saved:
                multi("  [{}] {}".format(slot, s))
            choice = inpt("SSID (blank to connect to first saved)").strip()
            if not choice and saved:
                ssid = saved[0][1]
            elif choice:
                ssid = choice
            else:
                warn("No SSID entered.")
                return
        else:
            ssid = inpt("SSID").strip()
            if not ssid:
                warn("No SSID entered.")
                return

    # Check if this SSID has a saved password
    saved_pw = None
    import regedit
    for slot in range(1, 3):
        stored_ssid = regedit.read('Networks.WiFi_SSID_{}'.format(slot))
        if stored_ssid and stored_ssid.strip() == ssid:
            saved_pw = regedit.read('Networks.WiFi_Password_{}'.format(slot)) or ''
            break

    if saved_pw is not None:
        multi("Using saved password for '{}'.".format(ssid))
        password = saved_pw
    else:
        password = masked_inpt("Password (blank for open network)").strip()

    net.connect(ssid, password)


def _disconnect():
    import net
    if not net.is_available():
        error("WiFi hardware not available.")
        return
    net.disconnect()


def _list_saved():
    import net
    if not net.is_available():
        error("WiFi hardware not available.")
        return

    saved = net.list_saved()
    if not saved:
        multi("  (no saved networks)")
        return
    multi("")
    for slot, ssid in saved:
        multi("  [{}] {}".format(slot, ssid))
    multi("")


def _add(ssid_arg):
    import net
    if not net.is_available():
        error("WiFi hardware not available.")
        return

    ssid = ssid_arg.strip() if ssid_arg else inpt("SSID").strip()
    if not ssid:
        warn("No SSID entered.")
        return
    password = inpt("Password (blank for open network)").strip()
    net.add_saved(ssid, password)


def _forget(ssid_arg):
    import net
    if not net.is_available():
        error("WiFi hardware not available.")
        return

    if not ssid_arg:
        _list_saved()
        ssid_arg = inpt("SSID to forget").strip()
    if not ssid_arg:
        warn("No SSID entered.")
        return
    net.forget_saved(ssid_arg)
