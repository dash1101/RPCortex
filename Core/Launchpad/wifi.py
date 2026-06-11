# Desc: WiFi management shell commands for RPCortex - Pulsar OS
# File: /Core/Launchpad/wifi.py
# Last Updated: 6/9/2026
# Lang: MicroPython, English
# Version: v0.8.2
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
      wifi connect [-s] [ssid] Connect to a network (-s = silent unless error)
      wifi autoconnect         Connect to strongest saved network, no prompts
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
        # Parse optional -s / --silent flag
        silent = False
        target = rest
        if rest and rest.startswith('-s'):
            silent = True
            target = rest[2:].strip() or None
        elif rest and rest.startswith('--silent'):
            silent = True
            target = rest[8:].strip() or None
        _connect(target, silent=silent)
    elif sub in ('autoconnect', 'auto'):
        _autoconnect()
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
    multi("  wifi status                Connection status")
    multi("  wifi scan                  Scan for nearby networks")
    multi("  wifi connect [-s] [ssid]   Connect to a network  (-s = quiet)")
    multi("  wifi autoconnect           Connect to strongest saved network")
    multi("  wifi disconnect            Disconnect")
    multi("  wifi list                  List saved networks")
    multi("  wifi add <ssid>            Save a network")
    multi("  wifi forget <ssid>         Remove a saved network")


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


def _connect(ssid_arg, silent=False):
    import net
    if not net.is_available():
        error("WiFi hardware not available.")
        return

    if ssid_arg:
        ssid = ssid_arg
    elif silent:
        # In silent mode, fall back to autoconnect
        _autoconnect(silent=True)
        return
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

    # Check for saved password
    saved_pw = None
    try:
        for s, p in net._read_networks():
            if s.lower() == ssid.lower():
                saved_pw = p
                break
    except Exception:
        pass

    if saved_pw is not None:
        if not silent:
            multi("Using saved password for '{}'.".format(ssid))
        password = saved_pw
    elif silent:
        # No saved password and in silent mode — can't prompt
        error("No saved password for '{}'. Use 'wifi add' first.".format(ssid))
        return
    else:
        password = masked_inpt("Password (blank for open network)").strip()

    net.connect(ssid, password, silent=silent)


def _autoconnect(silent=False):
    """Scan saved networks, connect to the one with strongest RSSI. No prompts."""
    import net
    if not net.is_available():
        error("WiFi hardware not available.")
        return

    saved = net._read_networks()
    if not saved:
        if not silent:
            warn("No saved networks. Use 'wifi add <ssid>' first.")
        else:
            error("No saved networks for autoconnect.")
        return

    if not silent:
        info("Scanning for saved networks...")

    try:
        scan_results = net.scan()
    except Exception:
        scan_results = []

    # Build SSID->RSSI map from scan
    rssi_map = {}
    for r in scan_results:
        ssid = r.get('ssid', '')
        if ssid and ssid not in rssi_map:
            rssi_map[ssid] = r.get('rssi', -100)

    # Match saved networks against scan, pick strongest
    best_ssid = None
    best_pw   = None
    best_rssi = -200
    for ssid, pw in saved:
        rssi = rssi_map.get(ssid, None)
        if rssi is None:
            # try case-insensitive match
            for k, v in rssi_map.items():
                if k.lower() == ssid.lower():
                    rssi = v
                    break
        if rssi is not None and rssi > best_rssi:
            best_rssi = rssi
            best_ssid = ssid
            best_pw   = pw

    if best_ssid is None:
        # Fall back: just try saved networks in order
        best_ssid, best_pw = saved[0]
        if not silent:
            warn("No saved network visible; trying '{}' anyway.".format(best_ssid))
    else:
        if not silent:
            info("Best match: '{}' ({} dBm)".format(best_ssid, best_rssi))

    net.connect(best_ssid, best_pw, silent=silent)


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
    password = masked_inpt("Password (blank for open network)").strip()
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
