# Desc: Power-On Self Test (POST) for RPCortex - Nebula OS
# File: /Core/post.py
# Last Updated: 3/27/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta3
# Author: dash1101

import uos, gc, sys, utime, machine
import Core.regedit as regedit
import Core.RPCortex as core
import Core.pulse as pulse

registry_content = """[Networks]

[Hardware]
beeper_pin: None
Min_Clock:
Max_Clock:
Boot_Clock:
Clockable: false

[System]
Codename: RPCortex B81 - Nebula
State: 0
Time: 0
Session: 0

[Settings]
Startup: 0
Version: v0.8.1-beta3
Note: 0
Active_User:
Setup: false
Network_Autoconnect: false
OC_On_Boot: false
Verbose_Boot: false

[Features]
Program_Execution: true
Serial: true
beeper: false
SD_Support: false
Nova: false

[Globals]
user_dir: /Users/
sandbox_dir: /Sandbox/
programs_dir: /Programs/
"""

errors = []
beeper = False   # module-level default; set by beeper_check()

try:
    core.post_check = True
except Exception as err:
    print(err)

def check_core():
    try:
        uos.stat("/Core/RPCortex.py")
        return True
    except OSError as err:
        core.fatal("RPCORTEX CORE LIBRARY NOT FOUND!")
        core.fatal("PLEASE REINSTALL RPCORTEX! {}".format(err))
        return False

def check_pulse():
    try:
        uos.stat("/Core/pulse.py")
        return True
    except OSError as err:
        core.fatal("RPCORTEX PULSE SOFTWARE NOT FOUND!")
        core.fatal("PLEASE REINSTALL RPCORTEX! {}".format(err))
        return False

def check_registry():
    try:
        uos.stat("/Core/regedit.py")
        try:
            uos.stat("/Nebula/Registry/registry.cfg")
            core.ok("Registry found!")
        except OSError as err:
            core.warn("File not found: '/Nebula/Registry/registry.cfg'")
            core.info("Creating /Nebula/Registry/")
            try:
                uos.mkdir("/Nebula")
            except OSError:
                pass   # already exists — fine
            try:
                uos.mkdir("/Nebula/Registry")
                core.ok("Registry directory created")
            except OSError:
                pass   # already exists — fine
            
            core.info("Building Registry '/Nebula/Registry/registry.cfg'")
            try:
                with open("/Nebula/Registry/registry.cfg", "w") as f:
                    f.write(registry_content)
                    core.ok("Registry created")
            except OSError as err:
                core.fatal("AN UNEXPECTED ISSUE IS CAUSING RPCORTEX TO NOT BOOT!")
                core.fatal("PLEASE REINSTALL RPCORTEX! {}".format(err))
                return False
        return True
    except OSError as err:
        core.fatal("AN UNEXPECTED ISSUE IS CAUSING RPCORTEX TO NOT BOOT!")
        core.fatal("PLEASE REINSTALL RPCORTEX! {}".format(err))
        return False

def wlan_check():
    """
    Check WiFi hardware availability and attempt autoconnect if configured.
    Works for Raspberry Pi Pico W and ESP32-based boards.
    """
    try:
        import Core.net as net
    except ImportError as err:
        core.warn("net module not available — WiFi support disabled.")
        return False

    if not net.is_available():
        core.warn("WiFi hardware not detected (no 'network' module).", p="POST")
        core.info("Supported boards: Pico W, ESP32, ESP32-S2/S3.", p="POST")
        return False

    core.ok("WiFi hardware detected.", p="POST")

    autoconn = regedit.read('Settings.Network_Autoconnect')
    if autoconn != 'true':
        core.info("Autoconnect is off. Use 'wifi connect' in the shell.", p="POST")
        return True   # hardware available, not an error

    # Autoconnect: try saved networks
    ssid1 = regedit.read('Networks.WiFi_SSID_1') or ''
    ssid2 = regedit.read('Networks.WiFi_SSID_2') or ''
    if not ssid1.strip() and not ssid2.strip():
        core.warn("Autoconnect enabled but no networks saved. Use 'wifi add'.", p="POST")
        return True

    core.info("Autoconnect enabled. Attempting connection...", p="POST")
    try:
        connected = net.connect_saved(timeout=15)
        if connected:
            s = net.status()
            core.ok("WiFi connected:  {}  ({})".format(
                s.get('ssid', '?'), s.get('ip', '?')), p="POST")
            return True
        else:
            core.warn("Autoconnect failed. Proceeding offline.", p="POST")
            return False
    except Exception as err:
        core.error("Autoconnect error: {}".format(err), p="POST")
        return False

def beeper_check():
    global beeper
    try:
        beeper_val = regedit.read("Features.beeper")
        beeper_pin = regedit.read("Hardware.beeper_pin")
        if beeper_val == "true":
            if beeper_pin != "None":
                beeper_pin = int(beeper_pin)
                beeper = machine.Pin(beeper_pin, machine.Pin.OUT)
                return True
        else:
            beeper = False
            return False
    except Exception as err:
        core.error("Unable to initialize beeper at pin {}".format(regedit.read('Hardware.beeper_pin')))
        return False

def check_oc():
    """
    One-time clock calibration — runs only when Hardware.Clockable != 'true'.
    Sets Settings.Startup to 6 as a crash sentinel; clears it on success.
    For RP2040/RP2350, uses known-safe limits without probing.
    For other platforms, applies the limits directly via pulse.set_clock().
    """
    try:
        regedit.save("Settings.Startup", "6")
        regedit.save("Hardware.Clockable", "false")
        core.info("Calibrating clock range...", p="POST")

        _plat = sys.platform.lower()
        if 'rp2' in _plat:
            # RP2040/RP2350 — known-safe limits, no probing needed
            minoc = "30.0MHz"
            maxoc = "220.0MHz"
        else:
            minoc = pulse.set_clock(30, verbose=False)
            maxoc = pulse.set_clock(250, verbose=False)

        regedit.save("Hardware.Min_Clock", minoc)
        regedit.save("Hardware.Max_Clock", maxoc)
        regedit.save("Hardware.Clockable", "true")
        regedit.save("Settings.Startup", "0")
        core.ok("Clock range: {} — {}".format(minoc, maxoc), p="POST")
    except Exception as err:
        core.error("Clock calibration failed: {}".format(err), p="POST")
        core.warn("System will continue at default clock speed.", p="POST")


def _apply_boot_clock():
    """
    Apply the boot clock on startup.
    Reads Hardware.Boot_Clock first; falls back to Hardware.Max_Clock.
    Sets Settings.Startup to 7 as a crash sentinel before machine.freq().
    If the device crashes here, the sentinel survives the reset and POST
    will detect it on the next boot, disabling OC_On_Boot automatically.
    """
    boot_clock = regedit.read("Hardware.Boot_Clock") or ""
    if not boot_clock.strip():
        boot_clock = regedit.read("Hardware.Max_Clock") or ""
    try:
        hz = int(float(boot_clock.replace("MHz", "").strip()) * 1_000_000)
    except Exception:
        hz = 125_000_000  # stock RP2040 speed as safe fallback
    regedit.save("Settings.Startup", "7")  # OC boot crash sentinel
    try:
        machine.freq(hz)
        core.ok("Boot clock applied: {} MHz".format(hz // 1_000_000), p="POST")
    except Exception as e:
        core.warn("Boot clock failed: {}. Running at default.".format(e), p="POST")

def check_cores():
    """Check and print available CPU cores."""
    try:
        if hasattr(machine, "ncores"):
            max_cores = machine.ncores()
            core.ok("Multi-core support detected. Max cores: {}".format(max_cores))
        else:
            core.ok("Single-core processor.")
    except Exception as err:
        core.error("Error checking CPU cores: {}".format(err))

def _ensure_log_dir():
    """Create /Nebula/Logs/ on first boot if it doesn't exist."""
    try:
        uos.stat("/Nebula/Logs")
    except OSError:
        try:
            uos.mkdir("/Nebula/Logs")
        except OSError:
            pass  # non-fatal — log writes will just silently fail


def _warn_unexpected_shutdown():
    """
    Called when Settings.Startup == '1', meaning the last session ended without
    a clean shutdown (power loss, hard crash, watchdog reset, etc.).
    Prints a warning and shows the last few lines from the previous session log.
    """
    core.warn("=" * 44, p="POST")
    core.warn("LAST SHUTDOWN WAS UNEXPECTED", p="POST")
    core.warn("The device may have lost power or crashed.", p="POST")
    core.warn("=" * 44, p="POST")

    last_user = regedit.read("Settings.Active_User") or "unknown"
    core.info("Last active user : {}".format(last_user), p="POST")

    log_path = "/Nebula/Logs/latest.log"
    try:
        uos.stat(log_path)
        with open(log_path, "r") as f:
            lines = f.readlines()
        if lines:
            core.info("Last log entries:", p="POST")
            tail = lines[-6:] if len(lines) >= 6 else lines
            for line in tail:
                core.multi("    " + line.rstrip())
        core.info("Full log: read /Nebula/Logs/latest.log", p="POST")
    except OSError:
        core.info("No previous log found.", p="POST")


def script():
    core.info("=== Power-On Self Test (POST) ===", p="POST")
    core.info("Checking system integrity before boot...", p="POST")

    # --- Registry FIRST ---
    # All subsequent steps read/write the registry; the directories and file
    # must exist before any regedit call is made.
    if not check_registry():
        core.fatal("Registry check FAILED. Cannot continue.", p="POST")
        return False
    core.ok("Registry OK.", p="POST")

    # Now the registry exists — read verbose boot preference safely.
    # Use a list so the closure always sees the current value (MicroPython safe).
    _vb = [False]
    _vb[0] = (regedit.read('Settings.Verbose_Boot') or 'false') == 'true'
    def _vi(msg, p='POST'):
        if _vb[0]:
            core.info(msg, p=p)

    # Guarantee /Nebula/Logs/ exists now that /Nebula/ is confirmed.
    _ensure_log_dir()
    gc.collect()

    # --- Shutdown sentinel check ---
    # "1" means a session was active and never cleanly shut down.
    startup_val = regedit.read("Settings.Startup") or "0"
    if startup_val == "1":
        _warn_unexpected_shutdown()

    # --- Clock setup (registry safe to read/write now) ---
    # Sentinel 6: crashed during first-boot clock calibration
    if startup_val == "6":
        regedit.save("Hardware.Clockable", "false")
        core.warn("Last boot failed during clock calibration. Clocking disabled.", p="POST")
    # Sentinel 7: crashed while applying boot clock
    elif startup_val == "7":
        regedit.save("Settings.OC_On_Boot", "false")
        core.warn("Last boot crashed during clock application. Boot clock disabled.", p="POST")
        core.warn("Use 'pulse set <MHz>' to change clock manually.", p="POST")

    if regedit.read("Hardware.Clockable") != "true":
        core.info("First boot — running clock calibration...", p="POST")
        check_oc()

    if regedit.read("Settings.OC_On_Boot") == "true":
        _apply_boot_clock()
    else:
        core.info("CPU at {} MHz. Use 'pulse boot <MHz>' to set a boot clock.".format(
            machine.freq() // 1_000_000), p="POST")

    # --- Critical file checks ---
    _vi("Checking core files...", p="POST")
    if not check_core():
        core.fatal("Core file check FAILED. Cannot continue.", p="POST")
        return False
    if not check_pulse():
        core.fatal("Pulse module check FAILED. Cannot continue.", p="POST")
        return False
    gc.collect()

    _vi("Checking CPU...", p="POST")
    if not pulse.cpu_check():
        core.fatal("CPU check FAILED. Cannot continue.", p="POST")
        return False
    core.ok("CPU OK.", p="POST")
    gc.collect()

    _vi("Checking memory...", p="POST")
    if not pulse.mem_check():
        core.fatal("Memory check FAILED. Cannot continue.", p="POST")
        return False
    core.ok("Memory OK.", p="POST")
    gc.collect()

    _vi("Checking CPU core count...", p="POST")
    check_cores()
    gc.collect()

    # --- Non-critical checks ---
    _vi("Checking WLAN...", p="POST")
    if not wlan_check():
        errors.append("WLAN not available")
    gc.collect()

    _vi("Checking beeper...", p="POST")
    if not beeper_check():
        errors.append("Beeper not configured or unavailable")

    # --- Summary ---
    if errors:
        core.warn("POST completed with warnings:", p="POST")
        for msg in errors:
            core.warn("  - {}".format(msg), p="POST")
        core.info("Non-critical hardware may be unavailable this session.", p="POST")
        if beeper:
            beeper.on()
            utime.sleep_ms(175)
            beeper.off()
    else:
        core.ok("All checks passed. System is ready.", p="POST")
        if beeper:
            beeper.on()
            utime.sleep_ms(25)
            beeper.off()

    # Sentinel "1": session is now active. A clean reboot/shutdown writes "0"
    # before calling machine.reset(); if "1" survives to the next POST it means
    # the previous session ended without a clean shutdown.
    regedit.save("Settings.Startup", "1")
    gc.collect()
    return True