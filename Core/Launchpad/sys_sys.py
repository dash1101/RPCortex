# Desc: System info/control shell commands - RPCortex Pulsar OS
# File: /Core/Launchpad/sys_sys.py
# Last Updated: 6/9/2026
# Lang: MicroPython, English
# Version: v0.8.2

import sys
import uos
import utime
import gc
import machine

if '/Core' not in sys.path:
    sys.path.append('/Core')

import regedit
from RPCortex import warn, error, info, ok, multi, inpt


# NOTE: reboot / sreboot / freeup below are shadowed at dispatch time by the
# inline versions in launchpad._CRITICAL (_crit_reboot / _crit_sreboot /
# _crit_freeup), which are reached FIRST so they still work when the heap is too
# fragmented to import a command file. These copies stay as the command-table
# targets (so the names register for help / tab-completion) but are not normally
# executed. Keep the two in sync if you change shutdown behaviour.


def reboot(args=None):
    info("Rebooting system...")
    from RPCortex import close_session_log
    close_session_log()
    try:
        regedit.save("Settings.Startup", "0")  # clean shutdown
    except Exception:
        pass
    machine.reset()


def sreboot(args=None):
    info("Performing soft reboot...")
    from RPCortex import close_session_log
    close_session_log()
    try:
        regedit.save("Settings.Startup", "0")  # clean shutdown
    except Exception:
        pass
    machine.soft_reset()


def uptime(args=None):
    ms = utime.ticks_ms()
    s  = ms // 1000
    m  = s  // 60
    h  = m  // 60
    s  = s  % 60
    m  = m  % 60
    if h > 0:
        multi("Uptime: {}h {}m {}s".format(h, m, s))
    elif m > 0:
        multi("Uptime: {}m {}s".format(m, s))
    else:
        multi("Uptime: {}s".format(s))


def sysinfo(args=None):
    gc.collect()
    free_ram  = gc.mem_free()
    alloc_ram = gc.mem_alloc()
    total_ram = free_ram + alloc_ram
    try:
        sv = uos.statvfs("/")
        free_flash = sv[0] * sv[3]
    except OSError:
        free_flash = -1

    info("=== RPCortex Pulsar — System Info ===")
    multi("  OS Version  : {}".format(regedit.read('Settings.Version') or 'Unknown'))
    multi("  Build       : {}  ({})".format(
        regedit.read('System.Build') or _os_build(),
        regedit.read('System.Stage') or _os_stage()))
    _owner = regedit.read('System.Owner')
    if _owner:
        multi("  Owner       : {}".format(_owner))
    multi("  Device ID   : {}".format(regedit.read('System.Device_ID') or 'pulsar'))
    multi("  Active User : {}".format(regedit.read('Settings.Active_User') or 'Unknown'))
    multi("  Platform    : {}".format(sys.platform))
    multi("  MicroPython : {}".format(sys.version))
    multi("  CPU Freq    : {} MHz".format(machine.freq() // 1_000_000))
    multi("  Boot Clock  : {}".format(regedit.read('Hardware.Boot_Clock') or 'not set'))
    multi("  Max Clock   : {}".format(regedit.read('Hardware.Max_Clock') or 'Unknown'))
    multi("  Clockable   : {}".format(regedit.read('Hardware.Clockable') or 'Unknown'))
    multi("  RAM Total   : {} KB".format(total_ram // 1024))
    multi("  RAM Free    : {} KB  ({}%)".format(
        free_ram // 1024, free_ram * 100 // total_ram if total_ram else 0))
    if free_flash >= 0:
        multi("  Flash Free  : {} KB".format(free_flash // 1024))


def meminfo(args=None):
    gc.collect()
    free  = gc.mem_free()
    alloc = gc.mem_alloc()
    total = free + alloc
    pct   = alloc * 100 // total if total else 0
    multi("  Total : {} KB".format(total // 1024))
    multi("  Used  : {} KB  ({}%)".format(alloc // 1024, pct))
    multi("  Free  : {} KB".format(free // 1024))


def date(args=None):
    if args:
        sp = args.strip().split(None, 1)
        if sp[0].lower() == 'set':
            if len(sp) < 2:
                warn("Usage: date set YYYY-MM-DD [HH:MM:SS]")
                return
            _set_rtc(sp[1].strip())
            return
        warn("Usage: date   |   date set YYYY-MM-DD [HH:MM:SS]")
        return
    # Apply the configured timezone offset (hours) for display, if set.
    off = 0
    try:
        off = int(regedit.read('System.TZ_Offset') or 0)
    except Exception:
        off = 0
    t = utime.localtime(utime.time() + off * 3600) if off else utime.localtime()
    if off:
        multi("{:04d}-{:02d}-{:02d}  {:02d}:{:02d}:{:02d}  (UTC{:+d})".format(
            t[0], t[1], t[2], t[3], t[4], t[5], off))
    else:
        multi("{:04d}-{:02d}-{:02d}  {:02d}:{:02d}:{:02d}".format(
            t[0], t[1], t[2], t[3], t[4], t[5]))


def _set_rtc(s):
    """Set the hardware RTC from 'YYYY-MM-DD [HH:MM:SS]'."""
    try:
        dt = s.split(None, 1)
        ymd = dt[0].split('-')
        y, mo, d = int(ymd[0]), int(ymd[1]), int(ymd[2])
        hh = mm = ss = 0
        if len(dt) > 1 and dt[1].strip():
            hms = dt[1].strip().split(':')
            hh = int(hms[0])
            mm = int(hms[1]) if len(hms) > 1 else 0
            ss = int(hms[2]) if len(hms) > 2 else 0
    except (ValueError, IndexError):
        error("Bad format. Use: date set YYYY-MM-DD HH:MM:SS")
        return
    try:
        # RTC.datetime() tuple: (year, month, day, weekday, hours, min, sec, subsec)
        # weekday is recomputed from the timestamp by localtime(), so 0 is fine.
        machine.RTC().datetime((y, mo, d, 0, hh, mm, ss, 0))
        ok("Clock set: {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}".format(
            y, mo, d, hh, mm, ss))
    except Exception as e:
        error("Could not set RTC: {}".format(e))


def watch(args):
    """watch [-n <secs>] <command>  — re-run a command periodically (Ctrl+C to stop)."""
    if not args:
        warn("Usage: watch [-n <secs>] <command>")
        return
    interval = 2.0
    cmdline  = args.strip()
    if cmdline.startswith('-n'):
        rest = cmdline[2:].strip()
        sp   = rest.split(None, 1)
        if len(sp) < 2:
            warn("Usage: watch -n <secs> <command>")
            return
        try:
            interval = float(sp[0])
        except ValueError:
            warn("Invalid interval: '{}'".format(sp[0]))
            return
        cmdline = sp[1].strip()
    if not cmdline:
        warn("No command to watch.")
        return

    # Reach the *running* shell engine, not a fresh import.  The live shell is
    # registered as 'Core.launchpad'; a bare `import launchpad` would build a
    # second instance with an empty command table.
    _lp = sys.modules.get('Core.launchpad') or sys.modules.get('launchpad')
    if _lp is None:
        error("watch: shell engine not available.")
        return

    try:
        while True:
            sys.stdout.write("\x1b[2J\x1b[H")
            multi("\033[96mwatch\033[0m every {}s: {}   (Ctrl+C to stop)".format(interval, cmdline))
            multi("")
            try:
                _lp._run_line(cmdline)   # full line: supports pipes, && / ||
            except Exception as e:
                error("watch: command error: {}".format(e))
            utime.sleep(interval)
    except KeyboardInterrupt:
        multi("")
        info("watch stopped.")


def clear(args=None):
    sys.stdout.write("\x1b[2J\x1b[H")


def ver(args=None):
    ver_str  = regedit.read('Settings.Version') or 'Unknown'
    codename = regedit.read('System.Codename')  or 'Pulsar'
    build    = regedit.read('System.Build') or _os_build()
    stage    = regedit.read('System.Stage') or _os_stage()
    multi("RPCortex {}  —  {}".format(ver_str, codename))
    multi("Build: {}  ({})".format(build, stage))
    multi("MicroPython {}   Platform: {}".format(sys.version, sys.platform))


def _os_build():
    """Build id from RPCortex.OS_BUILD; 'source' for an unstamped tree."""
    try:
        from RPCortex import OS_BUILD
        return OS_BUILD or 'source'
    except Exception:
        return 'source'


def _os_stage():
    """Release channel from RPCortex.OS_STAGE; 'dev' for an unstamped tree."""
    try:
        from RPCortex import OS_STAGE
        return OS_STAGE or 'dev'
    except Exception:
        return 'dev'


def keycode(args=None):
    """Show the raw byte(s) each keypress sends — diagnoses terminal keymaps
    (e.g. exactly what Ctrl+Backspace emits on YOUR terminal). Quit with Ctrl+C.

    Most keys are one byte; escape sequences (arrows, Ctrl+Del) arrive as several
    bytes shown on separate lines starting with 0x1b."""
    _names = {0x08: 'Ctrl+H / Backspace', 0x7f: 'DEL / Backspace',
              0x17: 'Ctrl+W', 0x1b: 'ESC (sequence start)', 0x0d: 'Enter (CR)',
              0x0a: 'Enter (LF)', 0x09: 'Tab', 0x7e: '~ (sequence end)'}
    info("Press keys to see their byte(s).  Ctrl+C to quit.")
    while True:
        try:
            ch = sys.stdin.read(1)
        except Exception:
            break
        if ch == '':
            continue
        b = ord(ch[0])
        if b == 0x03:                       # Ctrl+C
            multi("  0x03    Ctrl+C  (quit)")
            break
        name = _names.get(b)
        if name is None:
            name = repr(ch) if 32 <= b < 127 else 'control byte'
        multi("  0x{:02x}    {}".format(b, name))
    ok("keycode: done.")


def env(args=None):
    REG_PATH = '/Pulsar/Registry/registry.cfg'
    section_filter = args.strip().lower() if args else None
    try:
        with open(REG_PATH, 'r') as f:
            lines = f.readlines()
    except OSError as e:
        error("Cannot read registry: {}".format(e))
        return
    current_section = ''
    for line in lines:
        line = line.rstrip('\n')
        stripped = line.strip()
        if stripped.startswith('[') and stripped.endswith(']'):
            current_section = stripped[1:-1].lower()
            if section_filter is None or current_section == section_filter:
                multi('')
                multi('\033[96m[{}]\033[0m'.format(stripped[1:-1]))
        elif stripped and not stripped.startswith('#'):
            if section_filter is None or current_section == section_filter:
                multi('  ' + stripped)
    multi('')


def reg(args=None):
    if not args:
        warn("Usage: reg get <Key>  |  reg set <Key> <value>")
        return
    parts = args.strip().split(None, 2)
    sub   = parts[0].lower()
    if sub == 'get':
        if len(parts) < 2:
            warn("Usage: reg get <Section.Key>")
            return
        val = regedit.read(parts[1])
        if val is None:
            warn("Key '{}' not found.".format(parts[1]))
        else:
            multi("{}  =  {}".format(parts[1], val))
    elif sub == 'set':
        if len(parts) < 3:
            warn("Usage: reg set <Section.Key> <value>")
            return
        try:
            regedit.save(parts[1], parts[2])
            ok("{}  =  {}".format(parts[1], parts[2]))
        except Exception as e:
            error("Failed to write registry: {}".format(e))
    else:
        warn("Unknown subcommand '{}'. Use: reg get | reg set".format(sub))


def pulse(args):
    if args is None:
        info("=== pulse — CPU clock management ===")
        multi("  Current    : {} MHz".format(machine.freq() // 1_000_000))
        multi("  Boot Clock : {}".format(regedit.read('Hardware.Boot_Clock') or 'not set'))
        multi("  Max Clock  : {}".format(regedit.read('Hardware.Max_Clock') or 'unknown'))
        multi("  Min Clock  : {}".format(regedit.read('Hardware.Min_Clock') or 'unknown'))
        multi("  Boot OC    : {}".format(regedit.read('Settings.OC_On_Boot') or 'false'))
        multi("")
        multi("  pulse status          Show clock info")
        multi("  pulse set <MHz>       Set clock now         (e.g. pulse set 220)")
        multi("  pulse min <MHz>       Save minimum clock    (dynamic-clock idle; e.g. pulse min 80)")
        multi("  pulse max <MHz>       Save maximum clock    (dynamic-clock busy; e.g. pulse max 220)")
        multi("  pulse boot <MHz>      Set boot clock + enable  (e.g. pulse boot 200)")
        multi("  pulse boot on|off     Enable/disable boot clock")
        return

    parts = args.strip().split(None, 1)
    sub   = parts[0].lower()
    rest  = parts[1].strip() if len(parts) > 1 else None

    if sub == 'status':
        multi("  Current    : {} MHz".format(machine.freq() // 1_000_000))
        multi("  Boot Clock : {}".format(regedit.read('Hardware.Boot_Clock') or 'not set  (uses Max Clock)'))
        multi("  Max Clock  : {}".format(regedit.read('Hardware.Max_Clock') or 'unknown'))
        multi("  Min Clock  : {}".format(regedit.read('Hardware.Min_Clock') or 'unknown'))
        multi("  Boot OC    : {}".format(regedit.read('Settings.OC_On_Boot') or 'false'))

    elif sub == 'set':
        from pulse import set_clock as _sc
        if rest:
            try:
                mhz = int(rest)
            except ValueError:
                error("Usage: pulse set <MHz>  e.g. pulse set 220")
                return
        else:
            try:
                mhz = int(inpt("Target frequency (MHz)").strip())
            except ValueError:
                error("Invalid frequency.")
                return
        if sys.platform == 'rp2' and mhz > 220:
            warn("Above 220 MHz exceeds the calibrated safe max for RP2040/RP2350. Stability not guaranteed.")
        result = _sc(target_mhz=mhz)
        ok("Clock set: {}".format(result))

    elif sub == 'min':
        if not rest:
            multi("  Min Clock : {}".format(regedit.read('Hardware.Min_Clock') or 'not set'))
            multi("  Usage: pulse min <MHz>  e.g. pulse min 30")
            return
        try:
            mhz = int(rest)
        except ValueError:
            error("Usage: pulse min <MHz>  e.g. pulse min 30")
            return
        val = "{:.1f}MHz".format(float(mhz))
        regedit.save('Hardware.Min_Clock', val)
        ok("Minimum clock saved: {}".format(val))

    elif sub == 'max':
        if not rest:
            multi("  Max Clock : {}".format(regedit.read('Hardware.Max_Clock') or 'not set'))
            multi("  Usage: pulse max <MHz>  e.g. pulse max 220")
            return
        try:
            mhz = int(rest)
        except ValueError:
            error("Usage: pulse max <MHz>  e.g. pulse max 220")
            return
        if sys.platform == 'rp2' and mhz > 220:
            warn("Above 220 MHz exceeds the calibrated safe max for RP2040/RP2350.")
        val = "{:.1f}MHz".format(float(mhz))
        regedit.save('Hardware.Max_Clock', val)
        ok("Maximum clock saved: {}".format(val))

    elif sub == 'boot':
        if not rest:
            state     = regedit.read('Settings.OC_On_Boot') or 'false'
            boot_clk  = regedit.read('Hardware.Boot_Clock') or 'not set  (uses Max Clock)'
            multi("  Boot OC    : {}".format(state))
            multi("  Boot Clock : {}".format(boot_clk))
            multi("  pulse boot <MHz>  — set clock and enable")
            multi("  pulse boot on     — enable with stored clock")
            multi("  pulse boot off    — disable")
            return
        if rest in ('on', 'true', 'enable'):
            regedit.save('Settings.OC_On_Boot', 'true')
            boot_clk = regedit.read('Hardware.Boot_Clock') or regedit.read('Hardware.Max_Clock') or 'not set'
            ok("Boot clock enabled — will apply {} on next boot.".format(boot_clk))
        elif rest in ('off', 'false', 'disable'):
            regedit.save('Settings.OC_On_Boot', 'false')
            ok("Boot clock disabled.")
        else:
            # Treat as MHz value — set boot clock and enable
            try:
                mhz = int(rest)
            except ValueError:
                warn("Usage: pulse boot <MHz> | on | off")
                return
            if sys.platform == 'rp2' and mhz > 220:
                warn("Above 220 MHz exceeds the calibrated safe max. Stability not guaranteed.")
            val = "{:.1f}MHz".format(float(mhz))
            regedit.save('Hardware.Boot_Clock', val)
            regedit.save('Settings.OC_On_Boot', 'true')
            ok("Boot clock set to {} and enabled.".format(val))

    else:
        error("Unknown subcommand '{}'. Run 'pulse' for usage.".format(sub))


# `bench` lives in the RPCMark package (/Packages/RPCMark/) — formerly
# NebulaMark/PulseMark. `fetch` lives in the PicoFetch package (/Packages/PicoFetch/).
# Both are registered via programs.lp and self-healed by launchpad.load_commands().


# ---------------------------------------------------------------------------
# Factory reset  (soft — restores defaults, wipes users/packages, reboots)
# ---------------------------------------------------------------------------

def factoryreset(args=None):
    """
    Soft factory reset.  Clears all user accounts, resets the registry to
    factory defaults, removes installed packages, wipes user home dirs and
    session logs, then reboots.  First-run setup wizard runs on next boot.
    """
    warn("=" * 54, p="Reset")
    warn("FACTORY RESET  —  THIS CANNOT BE UNDONE", p="Reset")
    warn("=" * 54, p="Reset")
    multi("")
    multi("  This will:")
    multi("    Erase all user accounts (root, guest, all custom)")
    multi("    Reset the registry to factory defaults")
    multi("    Delete all user home directories  (/Users/)")
    multi("    Remove all installed packages")
    multi("    Clear session logs")
    multi("")
    multi("  The device will reboot.  The first-run setup wizard")
    multi("  will run on the next boot — set a new root password.")
    multi("")
    confirm = inpt("Type CONFIRM to proceed (anything else cancels)")
    if confirm.strip() != "CONFIRM":
        info("Factory reset cancelled.")
        return
    multi("")

    # --- 1. Clear user accounts -----------------------------------------
    info("Clearing user accounts...", p="Reset")
    try:
        with open('/Pulsar/Registry/user.cfg', 'w') as f:
            f.write('')
        ok("User accounts cleared.", p="Reset")
    except OSError as e:
        warn("Could not clear user.cfg: {}".format(e), p="Reset")
    # Remove backup if present
    try:
        uos.remove('/Pulsar/Registry/user.cfg.bak')
    except OSError:
        pass

    # --- 2. Reset registry (delete → POST recreates from template) -------
    info("Resetting registry...", p="Reset")
    try:
        regedit._invalidate()
        uos.remove('/Pulsar/Registry/registry.cfg')
        ok("Registry deleted — will be recreated from template on next boot.", p="Reset")
    except OSError as e:
        warn("Could not remove registry: {}".format(e), p="Reset")
    try:
        uos.remove('/Pulsar/Registry/registry.cfg.bak')
    except OSError:
        pass

    # --- 3. Delete user home directories ----------------------------------
    info("Removing user home directories...", p="Reset")
    try:
        for entry in uos.listdir('/Users'):
            _rimtree('/Users/' + entry)
        ok("User home directories removed.", p="Reset")
    except OSError as e:
        warn("Could not clear /Users/: {}".format(e), p="Reset")

    # --- 4. Remove non-builtin packages -----------------------------------
    info("Removing installed packages...", p="Reset")
    try:
        removed = 0
        for entry in uos.listdir('/Packages'):
            cfg_path = '/Packages/{}/package.cfg'.format(entry)
            is_builtin = False
            try:
                with open(cfg_path, 'r') as f:
                    for line in f:
                        if 'pkg.builtin' in line and 'true' in line:
                            is_builtin = True
                            break
            except OSError:
                pass
            if not is_builtin:
                _rimtree('/Packages/' + entry)
                removed += 1
        ok("Removed {} user package(s).".format(removed), p="Reset")
    except OSError as e:
        warn("Could not clear packages: {}".format(e), p="Reset")
    # Wipe programs.lp so stale commands don't appear
    try:
        with open('/Core/Launchpad/programs.lp', 'w') as f:
            f.write('')
    except OSError:
        pass

    # --- 5. Clear session logs -------------------------------------------
    info("Clearing logs...", p="Reset")
    try:
        for fname in uos.listdir('/Pulsar/Logs'):
            try:
                uos.remove('/Pulsar/Logs/' + fname)
            except OSError:
                pass
        ok("Logs cleared.", p="Reset")
    except OSError as e:
        warn("Could not clear logs: {}".format(e), p="Reset")

    # --- 6. Reboot -------------------------------------------------------
    ok("Factory reset complete. Rebooting...", p="Reset")
    import machine as _m
    from RPCortex import close_session_log
    close_session_log()
    _m.reset()


# ---------------------------------------------------------------------------
# Recursive directory removal helper  (used by factoryreset and reinstall)
# ---------------------------------------------------------------------------

def _rimtree(path):
    """Remove a file or directory tree. Logs failures instead of silently dropping them."""
    try:
        stat = uos.stat(path)
    except OSError:
        return
    if stat[0] & 0x4000:   # directory
        try:
            entries = uos.listdir(path)
        except OSError:
            entries = []
        for entry in entries:
            _rimtree(path.rstrip('/') + '/' + entry)
        try:
            uos.rmdir(path)
        except OSError as e:
            warn("  rmdir '{}': {}".format(path, e))
    else:
        try:
            uos.remove(path)
        except OSError as e:
            warn("  rm '{}': {}".format(path, e))


# ---------------------------------------------------------------------------
# Reinstall  (full wipe — writes a boot-time reinstall stub)
# ---------------------------------------------------------------------------

def _full_reinstall(rpc_src=None, online=False):
    """
    Full system wipe + reinstall.  Deletes everything, writes the reinstall
    stub as main.py, and reboots — the stub auto-installs a staged /update.rpc.

      rpc_src : path to a local .rpc to stage (auto-installs on boot)
      online  : download the latest .rpc from the update server, then wipe

    With neither, it wipes only and you restore via the Web Installer.
    Exposed through `update reinstall [online | <path.rpc>]`.
    """
    import machine as _m

    warn("=" * 60, p="Wipe")
    warn("FULL SYSTEM WIPE  —  ALL DATA WILL BE DELETED", p="Wipe")
    warn("=" * 60, p="Wipe")
    multi("")
    multi("  EVERY file on the device will be erased, including:")
    multi("    /Core/   /Packages/   /Pulsar/   /Users/   /Programs/")
    multi("  (a normal 'update online' keeps your data — use that unless you")
    multi("   specifically want a clean factory reinstall.)")
    multi("")
    if online:
        multi("  The latest release will be downloaded, then installed fresh.")
    elif rpc_src:
        multi("  '{}' will be staged and installed fresh on boot.".format(rpc_src))
    else:
        multi("  Restore afterwards with the Web Installer: rpc.novalabs.app/install")
    multi("")
    confirm = inpt("Type WIPE to confirm (anything else cancels)")
    if confirm.strip() != "WIPE":
        info("Full reinstall cancelled.")
        return

    # --- Online: download the latest .rpc to /update.rpc BEFORE wiping ----
    if online:
        if not _download_latest(_FULL_TMP):
            error("Download failed — nothing was wiped.", p="Wipe")
            return
        rpc_src = _FULL_TMP

    # --- Validate a local .rpc argument ----------------------------------
    elif rpc_src:
        try:
            uos.stat(rpc_src)
        except OSError:
            error("Specified .rpc not found: {}".format(rpc_src))
            error("Fix the path or run 'update reinstall' alone to wipe anyway.")
            return

    # --- Read reinstall stub BEFORE wiping /Core/ ------------------------
    info("Loading reinstall stub...", p="Wipe")
    stub_content = None
    try:
        with open('/Core/rpc_stub.py', 'r') as f:
            stub_content = f.read()
        ok("Stub loaded ({} bytes).".format(len(stub_content)), p="Wipe")
    except OSError as e:
        error("Cannot read /Core/rpc_stub.py: {}".format(e), p="Wipe")
        error("Aborting — stub is required for safe recovery.", p="Wipe")
        return

    # --- Stage .rpc if provided ------------------------------------------
    # The online path already downloaded straight to /update.rpc, so only
    # copy when the source is a *different* file.
    if rpc_src and rpc_src != _FULL_TMP:
        info("Staging {} as {}...".format(rpc_src, _FULL_TMP), p="Wipe")
        try:
            # Chunk-copy so we never load the whole archive at once
            with open(rpc_src, 'rb') as _src:
                with open(_FULL_TMP, 'wb') as _dst:
                    while True:
                        _chunk = _src.read(4096)
                        if not _chunk:
                            break
                        _dst.write(_chunk)
                        _chunk = None
            gc.collect()
            ok("Staged {} ({} bytes).".format(
                _FULL_TMP, uos.stat(_FULL_TMP)[6]), p="Wipe")
        except Exception as e:
            warn("Could not stage .rpc: {}".format(e), p="Wipe")
            warn("Continuing wipe — use Web Installer to restore.", p="Wipe")

    # --- Wipe all OS directories -----------------------------------------
    _WIPE = ('/Core', '/Packages', '/Pulsar', '/Users', '/Programs', '/Sandbox')
    info("Wiping OS files...", p="Wipe")
    for d in _WIPE:
        try:
            uos.stat(d)
        except OSError:
            continue   # directory doesn't exist — skip
        gc.collect()   # release any cached module refs before deleting
        _rimtree(d)
        ok("  Wiped {}".format(d), p="Wipe")

    # --- Write reinstall stub as main.py ---------------------------------
    info("Writing reinstall stub...", p="Wipe")
    try:
        with open('/main.py', 'w') as f:
            f.write(stub_content)
        ok("Reinstall stub written to /main.py.", p="Wipe")
    except OSError as e:
        error("CRITICAL: Failed to write stub: {}".format(e), p="Wipe")
        error("Device may be unbootable. Use Web Installer at:", p="Wipe")
        error("  rpc.novalabs.app/install", p="Wipe")
        return

    stub_content = None
    gc.collect()

    # --- Reboot ----------------------------------------------------------
    ok("Wipe complete. Rebooting in 3 seconds...", p="Wipe")
    import utime
    utime.sleep(3)
    _m.reset()


# ---------------------------------------------------------------------------
# OS update  (apply a .rpc archive to the running OS)
# ---------------------------------------------------------------------------

_LATEST_URL = 'https://rpc.novalabs.app/releases/latest.json'
_OTA_TMP    = '/update.rpc'
_FULL_TMP   = '/update.rpc'   # the stub auto-installs this on boot after a wipe


def _installed_build():
    """Read the BUILD id from /Core/buildinfo.py (a source build only — a
    compiled build ships buildinfo.mpy, so this returns None there and the
    new build is shown at next login instead). Best-effort."""
    try:
        with open('/Core/buildinfo.py') as f:
            for line in f:
                if line.strip().startswith('BUILD'):
                    return line.split('=', 1)[1].strip().strip('"\'')
    except Exception:
        pass
    return None


def _download_latest(dest):
    """Download the latest release .rpc from the manifest to `dest`.
    Returns True on success. Used by both 'update online' and 'update reinstall online'."""
    manifest = _fetch_manifest()
    if manifest is None:
        return False
    url = manifest.get('url')
    if not url:
        error("Manifest has no download URL.")
        return False
    sz = manifest.get('size')
    info("Downloading {} ...".format(url), p="Update")
    if sz:
        info("Size: {} KB — this may take a minute.".format(int(sz) // 1024), p="Update")
    gc.collect()
    import net
    try:
        status, written = net.wget(url, dest=dest, verbose=False)
    except MemoryError:
        error("Not enough RAM for download. Run 'freeup' or reboot, then retry.", p="Update")
        return False
    except Exception as e:
        error("Download failed: {}".format(e), p="Update")
        return False
    if status != 200:
        error("HTTP {} — aborting.".format(status), p="Update")
        try:
            uos.remove(dest)
        except OSError:
            pass
        return False
    ok("Downloaded {} KB to {}.".format(written // 1024, dest), p="Update")
    return True


def _vt(v):
    """Parse 'v0.8.2' / '0.8.2-rc1' into a comparable tuple."""
    try:
        return tuple(int(x) for x in v.strip().lstrip('vV').split('-')[0].split('.'))
    except Exception:
        return (0,)


def _fetch_manifest():
    """Download and parse the OTA manifest. Returns dict or None."""
    import net
    if not net.is_available():
        error("WiFi not available. Connect first with: wifi connect")
        return None
    if not net.status().get('connected'):
        error("Not connected to WiFi. Run: wifi connect")
        return None
    # Free as much contiguous heap as possible before TLS
    cache = globals().get('_cmd_cache')
    if cache:
        cache.clear()
    gc.collect()
    try:
        status, body = net.wget(_LATEST_URL, verbose=False)
    except Exception as e:
        error("Could not reach update server: {}".format(e))
        return None
    if status != 200:
        error("Update server returned HTTP {}.".format(status))
        return None
    try:
        import ujson
        return ujson.loads(body.decode('utf-8') if isinstance(body, (bytes, bytearray)) else body)
    except Exception as e:
        error("Bad update manifest: {}".format(e))
        return None


def _update_check():
    """Check for a newer OS version. Returns the manifest if newer, else None."""
    cur = regedit.read('Settings.Version') or 'v0.0.0'
    cur_b = regedit.read('System.Build') or 'source'
    info("Current version : {}  (build {})".format(cur, cur_b), p="Update")
    info("Checking {} ...".format(_LATEST_URL), p="Update")
    manifest = _fetch_manifest()
    if manifest is None:
        return None
    latest = manifest.get('version', '?')
    info("Latest version  : {}  (build {})".format(
        latest, manifest.get('build', '?')), p="Update")

    # Build-aware: a newer version, OR the same version with a different build id
    # (the server re-published this version) both count as an update. This lets
    # beta/dev rebuilds at the same version number still be offered.
    cur_build    = regedit.read('System.Build') or 'source'
    latest_build = manifest.get('build', '')
    newer_version = _vt(latest) > _vt(cur)
    new_build     = (_vt(latest) == _vt(cur)) and bool(latest_build) and (latest_build != cur_build)

    if newer_version or new_build:
        notes = manifest.get('notes', '')
        if newer_version:
            ok("Update available: {} -> {}".format(cur, latest), p="Update")
        else:
            ok("New build available: {} (build {} -> {})".format(
                latest, cur_build, latest_build), p="Update")
        if notes:
            multi("  Changes: {}".format(notes))
        multi("  Install over the air:  update online")
        multi("  Or from a file:        update from-file <path.rpc>")
        return manifest
    ok("You are up to date.", p="Update")
    return None


def _update_online(force=False):
    """OTA: download the latest .rpc and apply it (preserving user data)."""
    manifest = _update_check()
    if manifest is None:
        if not force:
            return
        manifest = _fetch_manifest()
        if manifest is None:
            return
    multi("")
    if not _download_latest(_OTA_TMP):
        return
    _update_from_file(_OTA_TMP, new_build=manifest.get('build'))
    # Only reached if the update was cancelled or failed (success reboots)
    try:
        uos.remove(_OTA_TMP)
        info("Removed temporary {}.".format(_OTA_TMP))
    except OSError:
        pass


def update(args=None):
    """
    OS update management.

    Subcommands:
      update check               Check the update server for a newer version
      update online [--force]    Download the latest release and install it (OTA)
      update from-file <path>    Extract a .rpc to device, preserve user data, reboot
    """
    if not args:
        _update_help()
        return

    parts = args.strip().split(None, 1)
    sub   = parts[0].lower()
    rest  = parts[1].strip() if len(parts) > 1 else None

    if sub == 'from-file':
        if not rest:
            warn("Usage: update from-file <path/to/os.rpc>")
            return
        _update_from_file(rest)

    elif sub == 'check':
        _update_check()

    elif sub == 'online':
        _update_online(force=(rest == '--force'))

    elif sub == 'reinstall':
        # Full factory wipe + reinstall (the old standalone `reinstall` command).
        if rest and rest.lower() == 'online':
            _full_reinstall(online=True)
        elif rest:
            _full_reinstall(rpc_src=rest)
        else:
            _full_reinstall()

    else:
        warn("Unknown subcommand '{}'. Run 'update' for usage.".format(sub))
        _update_help()


def _update_from_file(archive_path, new_build=None):
    """Apply a .rpc archive as an OS update.  new_build (from the OTA manifest)
    is shown on success when known; otherwise the new build appears at next login."""
    # Resolve relative paths
    if not archive_path.startswith('/'):
        try:
            archive_path = uos.getcwd().rstrip('/') + '/' + archive_path
        except Exception:
            pass

    # Minimum version check — updates are only supported from v0.8.0 or above.
    # Earlier builds lack the _xfer protocol and registry layout required.
    try:
        cur_ver = regedit.read("Settings.Version") or ""
        parts = cur_ver.lstrip('v').split('.')
        major = int(parts[0]) if parts else 0
        minor = int(parts[1]) if len(parts) > 1 else 0
        if major < 1 and minor < 8:
            error("Update requires RPCortex v0.8.0 or later (current: {}).".format(cur_ver))
            info("Flash a fresh install from rpc.novalabs.app/install instead.")
            return
    except Exception:
        pass  # If version can't be read, continue and let the user decide

    # Confirm archive exists
    try:
        sz = uos.stat(archive_path)[6]
    except OSError:
        error("Archive not found: {}".format(archive_path))
        return

    multi("")
    warn("=" * 54, p="Update")
    warn("OS UPDATE FROM FILE", p="Update")
    warn("=" * 54, p="Update")
    multi("  Archive : {}  ({} bytes)".format(archive_path, sz))
    multi("")
    multi("  OS files will be overwritten.  User data is preserved:")
    multi("    /Users/              (home directories)")
    multi("    /Pulsar/Registry/    (registry + user accounts)")
    multi("    /Pulsar/pkg/         (package cache + repos)")
    multi("")
    multi("  Tip: run 'freeup' first if you're on a Pico 1 with limited RAM.")
    multi("")
    confirm = inpt("Type UPDATE to proceed (anything else cancels)")
    if confirm.strip() != "UPDATE":
        info("Update cancelled.")
        return

    multi("")
    info("Starting OS update...", p="Update")

    # Set crash sentinel — if device dies mid-update, next boot shows "update failed"
    try:
        regedit.save("Settings.Startup", "3")
    except Exception:
        pass

    gc.collect()

    # Run the installer
    try:
        import rpc_install
        n_installed, ok_flag = rpc_install.install_rpc(archive_path)
    except MemoryError as e:
        error("Out of memory during update: {}".format(e), p="Update")
        error("Run 'freeup' to compact the heap, then try again.", p="Update")
        try:
            regedit.save("Settings.Startup", "3")
        except Exception:
            pass
        return
    except Exception as e:
        error("Update engine error: {}".format(e), p="Update")
        try:
            regedit.save("Settings.Startup", "3")  # keep failure sentinel
        except Exception:
            pass
        return

    if not ok_flag:
        error("Update failed — no files were installed.", p="Update")
        try:
            regedit.save("Settings.Startup", "3")
        except Exception:
            pass
        return

    # Success — clear crash sentinel, schedule login notification
    try:
        regedit.save("Settings.Startup", "0")
    except Exception:
        pass
    try:
        regedit.save("Settings.Note", "update_ok")
    except Exception:
        pass

    # Remove the OTA temp archive — the update is applied, no need to keep
    # ~300 KB on flash through the reboot.
    if archive_path == _OTA_TMP:
        try:
            uos.remove(_OTA_TMP)
        except OSError:
            pass

    nb = new_build or _installed_build()
    if nb:
        ok("Update complete: {} file(s) installed.  Now on build {}.".format(
            n_installed, nb), p="Update")
    else:
        ok("Update complete: {} file(s) installed.".format(n_installed), p="Update")
    multi("")
    info("Rebooting in 3 seconds to apply the update...", p="Update")

    import utime
    import machine as _m
    from RPCortex import close_session_log
    utime.sleep(3)
    close_session_log()
    _m.reset()


def _update_help():
    info("=== OS Update ===")
    multi("")
    multi("  update check               Check the update server (shows version + build)")
    multi("  update online              Download + install the latest release (OTA)")
    multi("  update online --force      Reinstall even if already up to date")
    multi("  update from-file <path>    Apply a local .rpc archive")
    multi("")
    multi("  Full factory reinstall (ERASES everything, then installs fresh):")
    multi("    update reinstall           Wipe; restore via the Web Installer")
    multi("    update reinstall online    Download the latest, then wipe + install")
    multi("    update reinstall <path>    Stage a local .rpc, then wipe + install")
    multi("")
    multi("  check / online / from-file preserve user data:")
    multi("    /Users/  /Pulsar/  programs.lp (installed packages)")
    multi("")
    multi("  You can also update from the browser (no WiFi needed on device):")
    multi("    rpc.novalabs.app/update")


def edit(args=None):
    # Editor is a removable/upgradeable package now (lives in /Packages/Editor).
    if '/Packages/Editor' not in sys.path:
        sys.path.append('/Packages/Editor')
    try:
        from editor import edit as _edit
    except ImportError:
        error("Editor not installed. Restore it with: pkg install Editor")
        return
    _edit(args.strip() if args and args.strip() else None)


# ---------------------------------------------------------------------------
# Memory management
# ---------------------------------------------------------------------------

def freeup(args=None):
    """Clear cached command scopes and run GC to reclaim RAM."""
    gc.collect()
    before = gc.mem_free()
    cache = globals().get('_cmd_cache')
    if cache:
        cache.clear()
    gc.collect()
    after = gc.mem_free()
    ok("Memory freed: {} KB → {} KB free  (+{} KB)".format(
        before // 1024, after // 1024, (after - before) // 1024))


# ---------------------------------------------------------------------------
# Misc shell commands (formerly sys_misc.py)
# ---------------------------------------------------------------------------

def say(args):
    """echo / say — print text; optionally redirect to a file.

    Usage:
      echo Hello World          — print to terminal
      echo "text" > file.txt    — write (overwrite) to file
      echo "text" >> file.txt   — append to file
    """
    if not args:
        warn("Usage: echo <text>  |  echo <text> > file  |  echo <text> >> file")
        return

    # Detect redirect operators (search right-to-left to find the last one)
    out_file = None
    append   = False
    text     = args

    dbl = args.rfind(' >> ')
    sgl = args.rfind(' > ')

    if dbl != -1 and (sgl == -1 or dbl > sgl - 1):
        # '>>' found and is after (or no) '>'
        text     = args[:dbl]
        out_file = args[dbl + 4:].strip()
        append   = True
    elif sgl != -1:
        text     = args[:sgl]
        out_file = args[sgl + 3:].strip()
        append   = False

    # Strip matching outer quotes from text (single or double)
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ('"', "'"):
        text = text[1:-1]

    if out_file:
        if not out_file.startswith('/'):
            out_file = uos.getcwd().rstrip('/') + '/' + out_file
        try:
            mode = 'a' if append else 'w'
            with open(out_file, mode) as f:
                f.write(text + '\n')
        except OSError as e:
            error("Cannot write to '{}': {}".format(out_file, e))
    else:
        multi(text)


def history(args=None):
    hist = globals().get('_cmd_history', [])
    if not hist:
        multi("  (no history yet)")
        return
    start = max(0, len(hist) - 50)
    for i, cmd in enumerate(hist[start:], start=start + 1):
        multi("  {:>4}  {}".format(i, cmd))


# ---------------------------------------------------------------------------
# sleep / which
# ---------------------------------------------------------------------------

def sleep_cmd(args):
    """sleep <seconds>  — pause the shell for the given number of seconds."""
    if not args:
        warn("Usage: sleep <seconds>")
        return
    try:
        secs = float(args.strip().split()[0])
    except ValueError:
        warn("Invalid number: '{}'".format(args.strip()))
        return
    utime.sleep(secs)


def which(args):
    """which <command>  — show where a command is defined."""
    if not args:
        warn("Usage: which <command>")
        return
    name = args.strip().split()[0]

    _CRIT = ('reboot', 'sreboot', 'softreset', 'freeup', 'gc',
             '_xfer', 'alias', 'unalias', 'rawrepl', 'recovery')

    found = False

    if name in _CRIT:
        multi("  {} : built-in critical command".format(name))
        found = True

    cmds = globals().get('_commands', {})
    if name in cmds:
        multi("  {} : {}".format(name, cmds[name]))
        found = True

    aliases = globals().get('_aliases', {})
    if name in aliases:
        multi("  {} = {}  (alias)".format(name, aliases[name]))
        found = True

    if not found:
        warn("'{}': not found.".format(name))


def help(args=None):
    if not args:
        info("=== RPCortex Pulsar — Launchpad ===")
        multi("  Filesystem : ls  cd  pwd  touch  mkdir  rm  read  head  tail  exec  rename  mv  cp  df  du  tree")
        multi("  Text       : grep  wc  find  sort  uniq  hex  basename  dirname")
        multi("  System     : sysinfo  meminfo  uptime  date  watch  ver  reboot  sreboot  rawrepl  recovery  sleep  which  clear  pulse  bench  fetch  edit  env  reg  freeup  settings")
        multi("  Automation : startup  task  autonomy  script   (pipes |  chaining && ||  also supported)")
        multi("  Recovery   : fscheck  diag  logdump  regreset  pkgdisable  pkgenable")
        multi("  OS Mgmt    : update check|online|from-file|reinstall  factoryreset  keycode")
        multi("  Network    : wifi  wget  curl  runurl  ping  nslookup")
        multi("  Packages   : pkg install|remove|list|info|search|update|upgrade|repo")
        multi("  Users      : whoami  users  mkacct  usermod  passwd  rmuser  logout  exit")
        multi("  Misc       : help  echo  history  alias  unalias")
        multi("")
        multi("  Type 'help <category>' for details.")
        multi("  Categories: filesystem  text  system  automation  recovery  network  packages  users  misc  osmgmt")
        return

    a = args.strip().lower()

    if a == 'osmgmt':
        info("=== OS Management ===")
        multi("  update check              Check the update server (version + build)")
        multi("  update online             Download + install latest release (OTA)")
        multi("  update from-file <f>      Apply a local .rpc archive as an OS update")
        multi("  update reinstall [online|<f.rpc>]   Full factory wipe + fresh install")
        multi("  factoryreset              Restore factory defaults + reboot")
        multi("")
        multi("  check/online/from-file keep your data; reinstall ERASES everything.")
        multi("  Browser update (no WiFi required on device):  rpc.novalabs.app/update")

    elif a == "filesystem":
        info("=== Filesystem Commands ===")
        multi("  ls  [path]           List directory contents")
        multi("  cd  [path]           Change directory (no arg or ~ = home)")
        multi("  pwd                  Print working directory")
        multi("  touch <file>         Create empty file")
        multi("  mkdir <dir>          Create directory")
        multi("  rm  <path>           Delete file or directory (interactive)")
        multi("  read/cat/view <f>... Print file contents (multiple files ok)")
        multi("  head <f> [n]         First n lines  (default 10)")
        multi("  tail <f> [n]         Last n lines   (default 10)")
        multi("  exec <f.py>          Execute a Python script")
        multi("  rename <old> <new>   Rename  (relative or absolute)")
        multi("  mv  <src> <dst>      Move    (relative or absolute)")
        multi("  cp  <src> <dst>      Copy    (streamed; relative or absolute)")
        multi("  df                   Disk usage (whole filesystem)")
        multi("  du [path]            Size of a file or directory tree")
        multi("  tree [path]          Directory tree")

    elif a == "text":
        info("=== Text Processing Commands ===")
        multi("  grep <pattern> <file>   Search file for pattern (substring)")
        multi("  wc <file>               Line / word / byte count")
        multi("  find [dir] [pattern]    Recursive file search by name")
        multi("  sort <file>             Print lines sorted alphabetically")
        multi("  uniq <file>             Remove consecutive duplicate lines")
        multi("  hex <file> [n]          Hexdump first n bytes  (default 256)")
        multi("  basename <path>         File name portion of a path")
        multi("  dirname <path>          Directory portion of a path")

    elif a == "system":
        info("=== System Commands ===")
        multi("  sysinfo              System overview")
        multi("  meminfo              RAM usage")
        multi("  uptime               Time since boot")
        multi("  date [set ...]       Show date/time, or 'date set YYYY-MM-DD HH:MM:SS'")
        multi("  watch [-n s] <cmd>   Re-run a command every s seconds (Ctrl+C stops)")
        multi("  ver                  Show OS version")
        multi("  reboot               Hard restart")
        multi("  sreboot              Soft reboot")
        multi("  rawrepl              Exit OS → MicroPython REPL (for Web Installer)")
        multi("  recovery             Enter recovery mode (limited shell, no auth)")
        multi("  sleep <secs>         Pause for the given number of seconds")
        multi("  which <cmd>          Show where a command is defined")
        multi("  clear / cls          Clear the screen")
        multi("  pulse set|min|max|boot  CPU clock management")
        multi("  bench                Run RPCMark benchmark")
        multi("  fetch / neofetch     System info display")
        multi("  edit/nano [file]     Open the text editor")
        multi("  env [section]        Dump registry contents")
        multi("  reg get|set <key>    Read/write a registry key")
        multi("  freeup               Free cached RAM (clear cmd cache + GC)")
        multi("  update check|online  Check for / install OS updates (OTA)")
        multi("  update from-file <f>  Apply a local .rpc archive as an OS update")
        multi("  update reinstall ...  Full factory wipe + fresh install")
        multi("  keycode              Show raw bytes of keys you press")
        multi("  factoryreset         Restore factory defaults and reboot")

    elif a == "network":
        info("=== Network Commands ===")
        multi("  wifi status          Show connection status")
        multi("  wifi scan            Scan for nearby networks")
        multi("  wifi connect [ssid]  Connect to a network")
        multi("  wifi disconnect      Disconnect")
        multi("  wifi list            List saved networks")
        multi("  wifi add <ssid>      Save a network")
        multi("  wifi forget <ssid>   Remove a saved network")
        multi("  wget <url> [file]    Download a file")
        multi("  curl <url> [-v]      Fetch URL, print response body")
        multi("  runurl <url>         Download and execute a .py file")
        multi("  ping <host> [n]      TCP connectivity test (n packets)")
        multi("  nslookup <host>      DNS lookup")

    elif a in ("packages", "pkg"):
        info("=== Package Manager ===")
        multi("  pkg install <name>       Install from repo by name")
        multi("  pkg install <file.pkg>   Install a local .pkg archive")
        multi("  pkg remove  <name>       Uninstall a package")
        multi("  pkg list                 List installed packages")
        multi("  pkg info    <name>       Show package details")
        multi("  pkg search  <query>      Search repo cache")
        multi("  pkg update               Refresh repo indexes")
        multi("  pkg upgrade              Upgrade outdated packages")
        multi("  pkg repo list|add|remove Manage repos")

    elif a == "users":
        info("=== User Management ===")
        multi("  whoami               Show current user")
        multi("  users                List all user accounts (admin/nopass tags)")
        multi("  mkacct [name] [--nopass] [--admin]   Create a new account")
        multi("  usermod <user> passwd                Change a password")
        multi("  usermod <user> rename <newname>      Rename an account (admin)")
        multi("  usermod <user> admin  on|off         Grant/revoke admin (admin)")
        multi("  usermod <user> nopass on|off         Toggle no-password login (admin)")
        multi("  passwd [user]        Change your own password (or another's, admin)")
        multi("  rmuser <user>        Remove a user (root/guest are protected)")
        multi("  logout / exit        Log out of this session")

    elif a == "misc":
        info("=== Misc Commands ===")
        multi("  help [category]      This help message")
        multi("  echo / print <txt>   Print text")
        multi("  history              Command history")
        multi("  alias [name=cmd]     Define or list aliases (saved across reboots)")
        multi("  unalias <name>       Remove an alias")
        multi("  freeup               Free cached RAM")

    elif a == "automation":
        info("=== Automation & Scripting ===")
        multi("  Pipes & chaining     cmd1 | cmd2     ;     cmd1 && cmd2     cmd1 || cmd2")
        multi("    e.g.  cat log | grep ERROR | wc")
        multi("  startup ...          Commands that run once at login:")
        multi("    startup add <cmd> | remove <n> | list | clear | run")
        multi("  task ...             Commands that run on a repeating interval:")
        multi("    task add <secs> <cmd> | remove <n> | list | clear | run")
        multi("    'task run'           foreground scheduler loop (q / Ctrl+C to stop)")
        multi("    'task background on' fire tasks while you're idle at the prompt (v0.9.5)")
        multi("  asyncmode on|off     EXPERIMENTAL async shell — tasks fire while you type")
        multi("  autonomy on [user]   Run with NO login (headless); off | status")
        multi("  script <file.rps>    Run a script: set/$vars, if/else, while, end")
        multi("")
        multi("  Tip: 'startup add task run' makes the device autonomous at boot.")
        multi("  Tip: 'task background on' keeps you at the shell while tasks tick.")

    elif a == "recovery":
        info("=== Recovery & Diagnostics ===")
        multi("  fscheck              Verify core OS files are present + non-empty")
        multi("  diag                 Quick health snapshot (RAM/flash/registry)")
        multi("  logdump [n]          Print the session log (last n lines)")
        multi("  regreset             Delete registry.cfg; POST rebuilds defaults")
        multi("  pkgdisable <name>    Disable a package without removing it")
        multi("  pkgenable  <name>    Re-enable a disabled package")
        multi("  recovery             Enter recovery mode (limited shell)")

    else:
        # Try looking up as an individual command name
        _CMD_HINTS = {
            'ls':           'ls [path]           List directory contents',
            'cd':           'cd [path]           Change directory (no arg / ~ = home)',
            'pwd':          'pwd                 Print working directory',
            'touch':        'touch <file>        Create an empty file',
            'mkdir':        'mkdir <dir>         Create a directory',
            'rm':           'rm <path>           Delete file or directory (interactive)',
            'read':         'read/cat <file>...  Print file contents (multiple ok)',
            'cat':          'read/cat <file>...  Print file contents (multiple ok)',
            'head':         'head <f> [n]        First n lines (default 10)',
            'tail':         'tail <f> [n]        Last n lines (default 10)',
            'exec':         'exec <f.py>         Execute a Python script',
            'rename':       'rename <old> <new>  Rename a file (relative or absolute)',
            'mv':           'mv <src> <dst>      Move a file (relative or absolute)',
            'cp':           'cp <src> <dst>      Copy a file (streamed, large-file safe)',
            'df':           'df                  Disk usage (whole filesystem)',
            'du':           'du [path]           Size of a file or directory tree',
            'tree':         'tree [path]         Directory tree',
            'grep':         'grep <pat> <file>   Search file contents',
            'wc':           'wc <file>           Count lines/words/bytes',
            'find':         'find [path] <name>  Search for files by name',
            'sort':         'sort <file>         Sort lines of a file',
            'uniq':         'uniq <file>         Remove duplicate lines',
            'hex':          'hex <file>          Hex dump of a file',
            'basename':     'basename <path>     Strip directory and suffix',
            'dirname':      'dirname <path>      Parent directory of a path',
            'reboot':       'reboot              Hard restart',
            'sreboot':      'sreboot             Soft reboot',
            'softreset':    'softreset           Alias for sreboot',
            'rawrepl':      'rawrepl             Exit OS to MicroPython REPL',
            'recovery':     'recovery            Enter recovery shell (no auth)',
            'sysinfo':      'sysinfo             Print system information',
            'meminfo':      'meminfo             Show RAM usage',
            'uptime':       'uptime              Time since boot',
            'date':         'date [set ...]      Show or set date/time (date set YYYY-MM-DD HH:MM:SS)',
            'watch':        'watch [-n s] <cmd>  Re-run a command periodically (Ctrl+C stops)',
            'ver':          'ver                 Show OS version',
            'clear':        'clear / cls         Clear the screen',
            'cls':          'clear / cls         Clear the screen',
            'sleep':        'sleep <secs>        Pause for given seconds',
            'which':        'which <cmd>         Show where a command is defined',
            'pulse':        'pulse set|min|max|boot  CPU clock management',
            'bench':        'bench               Run RPCMark benchmark',
            'fetch':        'fetch / neofetch    System info display',
            'neofetch':     'fetch / neofetch    System info display',
            'edit':         'edit [file]         Open the text editor',
            'nano':         'nano [file]         Open the text editor',
            'vi':           'vi [file]           Open the text editor',
            'env':          'env [section]       Dump registry contents',
            'reg':          'reg get|set <key>   Read/write a registry key',
            'freeup':       'freeup              Clear command cache + GC',
            'gc':           'gc / freeup         Clear command cache + GC',
            'settings':     'settings            Open settings TUI panel',
            'update':       'update check|online|from-file|reinstall  OS updates / full reinstall',
            'factoryreset': 'factoryreset        Restore factory defaults',
            'keycode':      'keycode             Show raw byte(s) of each key pressed',
            'wifi':         'wifi status|scan|connect|disconnect|list|add|forget',
            'wget':         'wget <url> [dest]   Download a file',
            'curl':         'curl <url>          Fetch URL and print response',
            'runurl':       'runurl <url>        Download and execute a .py',
            'ping':         'ping <host>         TCP connectivity test',
            'nslookup':     'nslookup <host>     DNS lookup',
            'pkg':          'pkg install|remove|list|info|search|update|upgrade|commands',
            'whoami':       'whoami              Show current user',
            'users':        'users               List all user accounts',
            'mkacct':       'mkacct              Create a new user account',
            'rmuser':       'rmuser <user>       Remove a user account',
            'usermod':      'usermod <user> passwd|rename|admin|nopass   Modify an account',
            'passwd':       'passwd [user]       Change your own (or another user\'s) password',
            'logout':       'logout              Log out of this session',
            'exit':         'exit                Log out of this session',
            'echo':         'echo <text>         Print text',
            'say':          'say <text>          Print text',
            'history':      'history             Show command history',
            'alias':        'alias [name=cmd]    Define or list aliases (saved across reboots)',
            'unalias':      'unalias <name>      Remove an alias',
            'help':         'help [category]     Show help (categories: filesystem text system network packages users misc osmgmt)',
        }
        hint = _CMD_HINTS.get(a)
        if hint:
            info("=== {} ===".format(a))
            multi("  " + hint)
        else:
            warn("Unknown category or command '{}'.  Try: filesystem, text, system, network, packages, users, misc, osmgmt".format(a))
