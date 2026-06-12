# Desc: Boot initialization and login sequence for RPCortex - Pulsar OS
# File: /Core/initialization.py
# Last Updated: 6/10/2026
# Lang: MicroPython, English
# Version: v0.9.1
# Author: dash1101

from Core.RPCortex import multi, fatal, error, info, warn, ok, inpt, masked_inpt, OS_VERSION, OS_CODENAME, OS_BUILD
import Core.regedit as regedit
from Core.launchpad import launchpad_init as _boot
from Core.launchpad import recovery_init  as _recovery
from Core.usrmgmt  import decode, add_user, is_nopass
import uos, utime, sys

# ---------------------------------------------------------------------------
# System file integrity (populated in a future release)
# ---------------------------------------------------------------------------

system_files = {}   # path -> description; checked on every boot

def get_exist():
    """Verify all required system files exist. Always passes while dict is empty."""
    for path in system_files:
        try:
            uos.stat(path)
        except OSError:
            error("Missing system file: {}".format(path))
            return False
    return True

# ---------------------------------------------------------------------------
# Startup mode messages  (registry key: Settings.Startup)
# ---------------------------------------------------------------------------

_STARTUP_MSGS = {
    "0": None,
    "1": ("warn", "Previous session ended unexpectedly (power loss or crash)."),
    "2": ("warn", "System is unstable or has missing files."),
    "3": ("warn", "A system update failed on the last boot."),
    "4": ("ok",   "System update installed successfully — all good!"),
    "5": ("warn", "Booting in safe mode."),
    "6": ("warn", "Clock adjustment failed last boot — core clocking disabled."),
    "7": ("warn", "Boot clock crashed last boot — boot clock has been disabled. Use 'pulse boot <MHz>' to reconfigure."),
}

# ---------------------------------------------------------------------------
# First-run setup wizard
# ---------------------------------------------------------------------------

def setup_seq():
    """
    First-run setup wizard.  Runs once when Settings.Setup != 'true'.
    Creates root + guest accounts, configures WiFi/timezone/clock, adds the
    official repo, applies boot prefs.
    """
    multi("")
    info("=== RPCortex {} - First Run Setup ===".format(OS_VERSION))
    multi("")
    info("Welcome! Let's get your device configured.")
    multi("  Six quick steps - everything here can be changed later.")
    multi("")

    # --- Step 1: Root account ------------------------------------------------
    info("[1/6] Root account")
    multi("  'root' is the system administrator.")
    multi("")
    if decode('root', silent=True):
        info("  Root account already exists - skipping.")
    else:
        while True:
            pw = masked_inpt("  Set root password")
            if not pw.strip():
                warn("  Password cannot be blank.")
                continue
            confirm = masked_inpt("  Confirm password")
            if pw != confirm:
                error("  Passwords do not match.  Try again.")
                continue
            if add_user('root', pw):
                ok("  Root account created.")
            else:
                error("  Could not create root account.")
            break
    multi("")

    # --- Step 2: Owner -------------------------------------------------------
    info("[2/6] Owner")
    multi("  Optional - who owns this device? (shown in sysinfo)")
    multi("")
    owner = inpt("  Owner name [skip]").strip()
    if owner:
        try:
            regedit.save("System.Owner", owner)
            ok("  Owner set to '{}'.".format(owner))
        except Exception:
            pass
    else:
        ok("  Skipped.  Set later: reg set System.Owner <name>")
    multi("")

    # --- Step 3: Device name (hostname) -------------------------------------
    info("[3/6] Device name")
    multi("  Optional - appears in the shell prompt:  user@<name>")
    multi("  Leave blank to keep the default 'pulsar'.")
    multi("")
    devid = inpt("  Device name [pulsar]").strip()
    if devid:
        devid = devid.split()[0]   # keep it shell-safe: one token, no spaces
        try:
            regedit.save("System.Device_ID", devid)
            ok("  Device name set to '{}'.".format(devid))
        except Exception:
            pass
    else:
        ok("  Keeping 'pulsar'.  Change later: reg set System.Device_ID <name>")
    multi("")

    # --- Step 4: WiFi --------------------------------------------------------
    info("[4/6] WiFi")
    wifi_ok = False
    try:
        import net
        have_wifi = net.is_available()
    except Exception:
        have_wifi = False

    if not have_wifi:
        ok("  No WiFi hardware detected - skipping.")
    else:
        multi("  Connect now so we can sync the clock and fetch packages.")
        multi("")
        join = inpt("  Set up WiFi now? [Y/n]").strip().lower()
        if join in ('', 'y', 'yes'):
            ssid = None
            try:
                info("  Scanning...")
                nets = net.scan()
            except Exception:
                nets = []
            if nets:
                seen = []
                shown = []
                for n in nets:
                    s = n.get('ssid', '')
                    if s and s not in seen:
                        seen.append(s)
                        shown.append(n)
                    if len(shown) >= 9:
                        break
                multi("")
                for i in range(len(shown)):
                    n = shown[i]
                    multi("   {:>2}. {:<24} {:>4} dBm  {}".format(
                        i + 1, n['ssid'][:24], n['rssi'], n.get('security', '')))
                multi("    0. Other / hidden network (type the name)")
                multi("")
                pick = inpt("  Choose a network [number, or blank to skip]").strip()
                if pick.isdigit():
                    idx = int(pick)
                    if 1 <= idx <= len(shown):
                        ssid = shown[idx - 1]['ssid']
                    elif idx == 0:
                        ssid = inpt("  Network name (SSID)").strip()
            else:
                warn("  No networks found.")
                ssid = inpt("  Network name (SSID) [skip]").strip()

            if ssid:
                pwd = masked_inpt("  Password for '{}' (blank for open)".format(ssid))
                try:
                    info("  Connecting to '{}'...".format(ssid))
                    if net.connect(ssid, pwd):
                        net.add_saved(ssid, pwd)   # persist for autoconnect
                        ok("  Connected and saved.")
                        wifi_ok = True
                        ac = inpt("  Reconnect automatically on boot? [Y/n]").strip().lower()
                        if ac in ('', 'y', 'yes'):
                            try:
                                regedit.save("Settings.Network_Autoconnect", "true")
                                ok("  Autoconnect enabled.")
                            except Exception:
                                pass
                    else:
                        warn("  Could not connect.  Set up later: wifi connect {}".format(ssid))
                except Exception as e:
                    warn("  WiFi error: {}".format(e))
            else:
                ok("  Skipped.  Set up later with: wifi scan / wifi connect <ssid>")
        else:
            ok("  Skipped.  Set up later with: wifi scan / wifi connect <ssid>")
    multi("")

    # --- Step 5: Time (timezone + NTP) --------------------------------------
    info("[5/6] Time")
    multi("  Timezone offset from UTC in whole hours (e.g. -5 = US Eastern).")
    multi("")
    tz = inpt("  Timezone offset [0]").strip()
    if tz:
        if tz.lstrip('+-').isdigit():
            try:
                regedit.save("System.TZ_Offset", tz.lstrip('+'))
                ok("  Timezone set to UTC{}{}.".format(
                    '' if tz.startswith('-') else '+', tz.lstrip('+')))
            except Exception:
                pass
        else:
            warn("  Not a number - skipping.  Set later: reg set System.TZ_Offset <hours>")
    else:
        ok("  Keeping UTC (0).")

    if wifi_ok:
        sync = inpt("  Sync the clock now over the internet (NTP)? [Y/n]").strip().lower()
        if sync in ('', 'y', 'yes'):
            try:
                if '/Packages/NTP' not in sys.path:
                    sys.path.append('/Packages/NTP')
                import ntp as _ntp
                _ntp.ntp('sync')
            except Exception as e:
                warn("  Clock sync unavailable: {}".format(e))
                multi("    Sync later with:  ntp sync")
    else:
        multi("  (No WiFi - sync later with 'ntp sync', or set the clock with 'date set'.)")
    multi("")

    # --- Step 6: Boot preferences -------------------------------------------
    info("[6/6] Boot preferences")
    multi("  Verbose boot shows detailed POST checks on each startup.")
    multi("  Off by default - useful for debugging, noisy day-to-day.")
    multi("")
    vb = inpt("  Enable verbose boot? [y/N]").strip().lower()
    if vb in ('y', 'yes'):
        try:
            regedit.save("Settings.Verbose_Boot", "true")
            ok("  Verbose boot enabled.")
        except Exception:
            pass
    else:
        ok("  Verbose boot off.  Toggle anytime: reg set Settings.Verbose_Boot true")
    multi("")

    # --- Silent: create guest account ---------------------------------------
    if not decode('guest', silent=True):
        add_user('guest', '', nopass=True)

    # --- Silent: add official package repo ----------------------------------
    _REPO = 'https://raw.githubusercontent.com/dash1101/RPCortex-repo/main/repo/index.json'
    try:
        for _d in ('/Pulsar/pkg', '/Pulsar/pkg/cache'):
            try:
                uos.mkdir(_d)
            except OSError:
                pass
        try:
            with open('/Pulsar/pkg/repos.cfg', 'r') as _f:
                _existing = _f.read()
        except Exception:
            _existing = ''
        if _REPO not in _existing:
            with open('/Pulsar/pkg/repos.cfg', 'a') as _f:
                _f.write(_REPO + '\n')
    except Exception:
        pass   # non-fatal; user can add manually with `pkg repo add`

    # --- Done ---------------------------------------------------------------
    regedit.save("Settings.Setup", "true")
    multi("")
    ok("All set!  Official package repo added automatically.")
    multi("  Log in with 'root' or 'guest'.")
    if wifi_ok:
        multi("  Run 'pkg update' to fetch the latest package list.")
    else:
        multi("  Connect WiFi (wifi connect <ssid>) then 'pkg update' for packages.")
    multi("")

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def start(arg):
    try:
        if arg != "Startup":
            warn("Unknown start argument: '{}'".format(arg))
            return

        # Sync the registry version + codename with the running code.  After an
        # OS update the new code boots against the old registry values — without
        # this the post-update banner, `ver`, and `fetch` would keep reporting
        # the old release name.
        if regedit.read("Settings.Version") != OS_VERSION:
            try:
                regedit.save("Settings.Version", OS_VERSION)
            except Exception:
                pass
        if regedit.read("System.Codename") != OS_CODENAME:
            try:
                regedit.save("System.Codename", OS_CODENAME)
            except Exception:
                pass
        if regedit.read("System.Build") != OS_BUILD:
            try:
                regedit.save("System.Build", OS_BUILD)
            except Exception:
                pass

        version = regedit.read("Settings.Version") or "Unknown"
        info("RPCortex {} — starting up...".format(version))

        # File integrity check
        info("Checking system file integrity...")
        if not get_exist():
            fatal("Critical system files are missing!")
            recovery_mode(errStr="Missing critical system files.")
            return
        ok("System file check passed.")

        # Print current registry snapshot
        info("Reading system configuration...")
        ok("  Version       : {}".format(regedit.read("Settings.Version")     or "?"))
        ok("  Active User   : {}".format(regedit.read("Settings.Active_User") or "none"))
        ok("  Net Autoconn  : {}".format(regedit.read("Settings.Network_Autoconnect") or "?"))
        ok("  Clockable     : {}".format(regedit.read("Hardware.Clockable")   or "?"))

        # Startup mode banner — use the pre-POST value captured by post.py.
        # POST arms Settings.Startup to "1" at its end (session-active
        # sentinel), so reading the registry here would show the unexpected-
        # shutdown warning on every boot, clean or not.
        try:
            import Core.post as _post
            mode = getattr(_post, 'boot_startup_mode', '0') or '0'
        except Exception:
            mode = regedit.read("Settings.Startup") or "0"
        entry = _STARTUP_MSGS.get(mode)
        if entry:
            level, msg = entry
            if level == "warn":
                warn(msg, p="Boot")
            elif level == "ok":
                ok(msg, p="Boot")

        # First-run setup
        if regedit.read("Settings.Setup") != 'true':
            setup_seq()

        ok("System ready.  Proceeding to login.", p="Boot")
        login_seq()

    except Exception as e:
        error("Startup error: {}".format(e))
        recovery_mode(errStr=str(e))

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# _xfer at login — receive a file over serial without being logged in.
# Same base64 protocol as _crit_xfer in launchpad.py.
# Called when the username field contains "_xfer <path>".
# ---------------------------------------------------------------------------

def _login_xfer(dest):
    """Run the _xfer receive protocol at the login prompt (no session needed)."""
    if not dest:
        sys.stdout.write("XFER_ERR:no path\r\n")
        sys.stdout.write("XFER_COMPLETE\r\n")
        return

    # Create parent directories
    parts = [p for p in dest.split('/') if p]
    cur = ''
    for p in parts[:-1]:
        cur += '/' + p
        try:
            uos.mkdir(cur)
        except OSError:
            pass

    sys.stdout.write("XFER_READY\r\n")

    total = 0
    first_chunk = True
    line_buf = []

    while True:
        try:
            ch = sys.stdin.read(1)
        except Exception:
            sys.stdout.write("XFER_ERR:read_error\r\n")
            sys.stdout.write("XFER_COMPLETE\r\n")
            return

        if ch in ('\r', '\n'):
            s = ''.join(line_buf).strip()
            line_buf = []
            if not s:
                continue
            if s == 'XFER_END':
                break
            try:
                import ubinascii
                chunk = ubinascii.a2b_base64(s)
            except ImportError:
                try:
                    import binascii
                    chunk = binascii.a2b_base64(s)
                except Exception as e:
                    sys.stdout.write("XFER_ERR:{}\r\n".format(e))
                    sys.stdout.write("XFER_COMPLETE\r\n")
                    return
            except Exception as e:
                sys.stdout.write("XFER_ERR:{}\r\n".format(e))
                sys.stdout.write("XFER_COMPLETE\r\n")
                return
            try:
                mode = 'wb' if first_chunk else 'ab'
                first_chunk = False
                with open(dest, mode) as f:
                    f.write(chunk)
                total += len(chunk)
            except Exception as e:
                sys.stdout.write("XFER_ERR:{}\r\n".format(e))
                sys.stdout.write("XFER_COMPLETE\r\n")
                return
        else:
            line_buf.append(ch)

    sys.stdout.write("XFER_OK:{}\r\n".format(total))

    if dest.endswith('.pkg'):
        sys.stdout.write("XFER_INSTALLING\r\n")
        try:
            import pkgmgr
            result = pkgmgr.install(dest)
            if result:
                sys.stdout.write("XFER_INSTALLED\r\n")
            else:
                sys.stdout.write("XFER_INSTALL_FAILED\r\n")
        except Exception as e:
            sys.stdout.write("XFER_INSTALL_ERR:{}\r\n".format(e))

    sys.stdout.write("XFER_COMPLETE\r\n")

# Login sequence
# ---------------------------------------------------------------------------

def login_seq():
    """
    Main login loop.
    - NOPASS accounts (guest) are logged in immediately without a password prompt.
    - Normal accounts require password with 3-attempt lockout.
    """
    info("=== Login ===")
    multi("  Type 'root' or 'guest' to log in.")
    multi("")
    while True:
        username = inpt("Username").strip()

        if not username:
            warn("Please enter a username.")
            continue

        # _xfer at login — file transfer without an active session.
        # Allows the web package browser to push files even after a device reset.
        if username.startswith('_xfer'):
            _login_xfer(username[5:].strip())
            continue

        # rawrepl at login — drop to the bare MicroPython REPL so the Web
        # Installer can flash a fresh image even when no one is logged in.
        # SystemExit is a BaseException: it propagates past start()'s
        # `except Exception` and out of main.py, leaving the >>> prompt active.
        if username == 'rawrepl':
            multi("Exiting to MicroPython REPL...")
            raise SystemExit(0)

        # _pkgs at login — emit the machine-readable installed-package manifest
        # so the web package browser can show install state without a session.
        if username == '_pkgs':
            try:
                from Core.launchpad import emit_pkg_manifest
                emit_pkg_manifest()
            except Exception:
                sys.stdout.write("PKGS_BEGIN\r\nPKGS_END\r\n")
            continue

        if not decode(username, silent=True):
            warn("User '{}' not found.".format(username))
            multi("  Available accounts: root, guest  |  New users: run 'mkacct' after login")
            continue

        # NOPASS account (e.g. guest) — skip password prompt
        if is_nopass(username):
            info("No password required for '{}'.".format(username))
            regedit.save("Settings.Startup", "0")
            regedit.save("Settings.Active_User", username)
            ok("Welcome, {}!".format(username))
            Startup_Process(username, '')
            return

        # Normal password authentication
        attempts = 0
        while True:
            password = masked_inpt("Password")
            if not password:
                warn("Password cannot be blank.")
                continue
            if decode(username, password, silent=True):
                regedit.save("Settings.Startup", "0")
                regedit.save("Settings.Active_User", username)
                ok("Welcome, {}!".format(username))
                Startup_Process(username, password)
                return
            utime.sleep_ms(500)
            attempts += 1
            if attempts < 3:
                warn("Incorrect password.  Attempt {}/3.".format(attempts))
            else:
                error("Too many failed attempts.  Returning to username prompt.")
                break

# ---------------------------------------------------------------------------
# Recovery mode
# ---------------------------------------------------------------------------

def recovery_mode(errStr=None):
    cause = errStr or "No cause specified."
    info("Entering recovery mode — reason: {}".format(cause))
    _recovery(cause)

# ---------------------------------------------------------------------------
# Startup and recovery dispatch
# ---------------------------------------------------------------------------

def Startup_Process(username, password):
    # Clear startup sentinel now that login was successful
    try:
        regedit.save("Settings.Startup", "0")
    except Exception:
        pass

    # Show pending one-shot notifications (e.g. post-update confirmation)
    _note = regedit.read("Settings.Note")
    if _note and _note != "0":
        if _note == "update_ok":
            _ver = regedit.read("Settings.Version") or "Unknown"
            multi("")
            ok("─" * 52, p="Boot")
            ok("OS update applied successfully!", p="Boot")
            ok("Now running RPCortex {}.".format(_ver), p="Boot")
            ok("─" * 52, p="Boot")
        try:
            regedit.save("Settings.Note", "0")
        except Exception:
            pass
        multi("")

    info("Launching shell for '{}'...".format(username))
    import gc
    gc.collect()
    _boot(username, password)
    # Shell returned — re-enter login loop
    info("Shell exited.  Returning to login.")
    login_seq()
