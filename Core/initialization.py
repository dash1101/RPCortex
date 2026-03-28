# Desc: Boot initialization and login sequence for RPCortex - Nebula OS
# File: /Core/initialization.py
# Last Updated: 3/26/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta3
# Author: dash1101

from Core.RPCortex import multi, fatal, error, info, warn, ok, inpt
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
    "1": ("warn", "Previous boot entered recovery mode."),
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
    Creates root + guest accounts, adds the official repo, applies boot prefs.
    """
    multi("")
    info("=== RPCortex v0.8.1-beta3 — First Run Setup ===")
    multi("")
    info("Welcome! Let's get your device configured.")
    multi("  Everything here can be changed later from the shell.")
    multi("")

    # --- Step 1: Root account ---
    info("[1/2] Root account")
    multi("  'root' is the system administrator.")
    multi("")

    if decode('root', silent=True):
        info("  Root account already exists — skipping.")
    else:
        while True:
            pw = inpt("  Set root password")
            if not pw.strip():
                warn("  Password cannot be blank.")
                continue
            confirm = inpt("  Confirm password")
            if pw != confirm:
                error("  Passwords do not match.  Try again.")
                continue
            if add_user('root', pw):
                ok("  Root account created.")
            else:
                error("  Could not create root account.")
            break
    multi("")

    # --- Step 2: Boot preferences ---
    info("[2/2] Boot preferences")
    multi("  Verbose boot shows detailed POST checks on each startup.")
    multi("  Off by default — useful for debugging, annoying day-to-day.")
    multi("")
    vb = inpt("  Enable verbose boot? [y/N]").strip().lower()
    if vb == 'y':
        try:
            regedit.save("Settings.Verbose_Boot", "true")
            ok("  Verbose boot enabled.")
        except Exception:
            pass
    else:
        ok("  Verbose boot off.  Toggle anytime: reg set Settings.Verbose_Boot true")
    multi("")

    # --- Silent: create guest account ---
    if not decode('guest', silent=True):
        add_user('guest', '', nopass=True)

    # --- Silent: add official package repo ---
    _REPO = 'https://raw.githubusercontent.com/dash1101/RPCortex-repo/main/repo/index.json'
    try:
        for _d in ('/Nebula/pkg', '/Nebula/pkg/cache'):
            try:
                uos.mkdir(_d)
            except OSError:
                pass
        try:
            with open('/Nebula/pkg/repos.cfg', 'r') as _f:
                _existing = _f.read()
        except Exception:
            _existing = ''
        if _REPO not in _existing:
            with open('/Nebula/pkg/repos.cfg', 'a') as _f:
                _f.write(_REPO + '\n')
    except Exception:
        pass   # non-fatal; user can add manually with `pkg repo add`

    # --- Done ---
    regedit.save("Settings.Setup", "true")
    multi("")
    ok("All set!  Official package repo added automatically.")
    multi("  Log in with 'root' or 'guest'.")
    multi("  Run 'pkg update' to fetch the latest package list.")
    multi("")

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def start(arg):
    try:
        if arg != "Startup":
            warn("Unknown start argument: '{}'".format(arg))
            return

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
        ok("  Nova GUI      : {}".format(regedit.read("Features.Nova")        or "false"))

        # Startup mode banner
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

        if not decode(username, silent=True):
            warn("User '{}' not found.".format(username))
            multi("  Available accounts: root, guest  |  New users: run 'mkacct' after login")
            continue

        # NOPASS account (e.g. guest) — skip password prompt
        if is_nopass(username):
            info("No password required for '{}'.".format(username))
            regedit.save("Settings.Active_User", username)
            ok("Welcome, {}!".format(username))
            Startup_Process(username, '')
            return

        # Normal password authentication
        attempts = 0
        while True:
            password = inpt("Password")
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
