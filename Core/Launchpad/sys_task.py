# Desc: Startup task management - RPCortex Pulsar OS
# File: /Core/Launchpad/sys_task.py
# Last Updated: 6/10/2026
# Lang: MicroPython, English
# Version: v0.9.1
#
# Startup tasks let a standalone device do work on its own: any commands listed
# here run once, automatically, right after login — before the prompt opens.
# The shell engine (launchpad.py) reads the same file at shell start.
#
#   startup                 list configured tasks
#   startup add <command>   append a command (e.g. startup add wifi connect)
#   startup remove <n>      remove task number <n>  (see 'startup list')
#   startup clear           remove all tasks
#   startup run             run the tasks now, without rebooting

import sys

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, error, info, ok, multi

_CFG = '/Pulsar/Registry/startup.cfg'
_HEADER = (
    "# RPCortex startup tasks — one command per line, run once at login.\n"
    "# Manage with: startup add/remove/list/clear\n"
)


def _read():
    """Return the list of task command strings (comments/blanks stripped)."""
    try:
        with open(_CFG, 'r') as f:
            return [ln.strip() for ln in f
                    if ln.strip() and not ln.strip().startswith('#')]
    except OSError:
        return []


def _write(tasks):
    try:
        with open(_CFG, 'w') as f:
            f.write(_HEADER)
            for t in tasks:
                f.write(t + '\n')
        return True
    except OSError as e:
        error("Could not write startup.cfg: {}".format(e))
        return False


def _list():
    tasks = _read()
    if not tasks:
        multi("  No startup tasks configured.")
        multi("  Add one:  startup add <command>")
        return
    multi("  Startup tasks (run once at login):")
    for i, t in enumerate(tasks):
        multi("  {:>2}. {}".format(i + 1, t))
    multi("")
    multi("  {} task(s).  Run now: 'startup run'".format(len(tasks)))


def _add(cmd):
    if not cmd:
        error("Usage: startup add <command>")
        return
    tasks = _read()
    if cmd in tasks:
        warn("Already a startup task: {}".format(cmd))
        return
    tasks.append(cmd)
    if _write(tasks):
        ok("Added startup task: {}".format(cmd))


def _remove(arg):
    tasks = _read()
    if not tasks:
        warn("No startup tasks to remove.")
        return
    try:
        n = int(arg)
    except (ValueError, TypeError):
        error("Usage: startup remove <number>   (see 'startup list')")
        return
    if n < 1 or n > len(tasks):
        error("No task #{}. There are {} task(s).".format(n, len(tasks)))
        return
    removed = tasks.pop(n - 1)
    if _write(tasks):
        ok("Removed: {}".format(removed))


def _clear():
    tasks = _read()
    if not tasks:
        multi("  Already empty.")
        return
    if _write([]):
        ok("Cleared {} startup task(s).".format(len(tasks)))


def _run_now():
    """Trigger the live shell engine's startup runner (same as at boot)."""
    lp = sys.modules.get('Core.launchpad') or sys.modules.get('launchpad')
    if lp is None:
        error("Shell engine not available.")
        return
    runner = getattr(lp, '_run_startup_tasks', None)
    if runner is None:
        error("Startup runner not found in this build.")
        return
    runner()


def startup(args=None):
    if not args or not args.strip():
        _list()
        return
    parts = args.split(None, 1)
    sub  = parts[0].lower()
    rest = parts[1].strip() if len(parts) > 1 else ''

    if sub == 'list':
        _list()
    elif sub == 'add':
        _add(rest)
    elif sub in ('remove', 'rm', 'del'):
        _remove(rest)
    elif sub == 'clear':
        _clear()
    elif sub == 'run':
        _run_now()
    else:
        error("Unknown subcommand '{}'.".format(sub))
        info("Usage: startup [list|add <cmd>|remove <n>|clear|run]")


# ===========================================================================
# Scheduled tasks  —  run commands on a repeating interval (uptime-based)
#
# `task run` enters a foreground scheduler: it fires due tasks on time and uses
# select() to stay responsive to 'q' / Ctrl+C.  Because the interactive prompt
# blocks on input, true background timing isn't possible without uasyncio — so
# scheduling lives in this dedicated loop instead.  Launch it as a startup task
# (`startup add task run`) for an autonomous, headless device.
# ===========================================================================

_TASKS = '/Pulsar/Registry/tasks.cfg'
_THEADER = (
    "# RPCortex scheduled tasks — '<seconds>\\t<command>' per line.\n"
    "# Manage with: task add <secs> <command> / list / remove <n> / clear\n"
)


def _read_tasks():
    """Return a list of (interval_seconds, command) tuples."""
    out = []
    try:
        with open(_TASKS, 'r') as f:
            for raw in f:
                line = raw.rstrip('\n')
                s = line.strip()
                if not s or s.startswith('#'):
                    continue
                if '\t' in line:
                    a, b = line.split('\t', 1)
                else:
                    a, _, b = line.partition(' ')
                try:
                    interval = int(a.strip())
                except ValueError:
                    continue
                cmd = b.strip()
                if interval > 0 and cmd:
                    out.append((interval, cmd))
    except OSError:
        pass
    return out


def _write_tasks(tasks):
    try:
        with open(_TASKS, 'w') as f:
            f.write(_THEADER)
            for interval, cmd in tasks:
                f.write("{}\t{}\n".format(interval, cmd))
        return True
    except OSError as e:
        error("Could not write tasks.cfg: {}".format(e))
        return False


def _task_list():
    tasks = _read_tasks()
    if not tasks:
        multi("  No scheduled tasks.")
        multi("  Add one:  task add <seconds> <command>")
        return
    multi("  Scheduled tasks:")
    multi("  {:>2}  {:>7}  {}".format("#", "EVERY", "COMMAND"))
    for i, (interval, cmd) in enumerate(tasks):
        multi("  {:>2}  {:>5}s  {}".format(i + 1, interval, cmd))
    multi("")
    multi("  {} task(s).  Start the scheduler: 'task run'".format(len(tasks)))


def _task_add(rest):
    sp = rest.split(None, 1)
    if len(sp) < 2:
        error("Usage: task add <seconds> <command>")
        return
    try:
        interval = int(sp[0])
    except ValueError:
        error("Interval must be a whole number of seconds.")
        return
    if interval <= 0:
        error("Interval must be greater than zero.")
        return
    cmd = sp[1].strip()
    tasks = _read_tasks()
    tasks.append((interval, cmd))
    if _write_tasks(tasks):
        ok("Scheduled: every {}s -> {}".format(interval, cmd))


def _task_remove(arg):
    tasks = _read_tasks()
    if not tasks:
        warn("No scheduled tasks to remove.")
        return
    try:
        n = int(arg)
    except (ValueError, TypeError):
        error("Usage: task remove <number>   (see 'task list')")
        return
    if n < 1 or n > len(tasks):
        error("No task #{}. There are {} task(s).".format(n, len(tasks)))
        return
    interval, cmd = tasks.pop(n - 1)
    if _write_tasks(tasks):
        ok("Removed: every {}s -> {}".format(interval, cmd))


def _task_clear():
    tasks = _read_tasks()
    if not tasks:
        multi("  Already empty.")
        return
    if _write_tasks([]):
        ok("Cleared {} scheduled task(s).".format(len(tasks)))


def _task_background(rest):
    """Arm/disarm the idle background scheduler (Track A multitasking, v0.9.5).

    When ON, scheduled tasks fire while you're idle at the prompt (they pause the
    moment you start typing and while a command is running). Unlike `task run`
    this doesn't block the shell — you keep working between fires."""
    import regedit
    sub = (rest or '').strip().lower()
    cur = (regedit.read('Apps.Task_Background') or 'false') == 'true'
    if sub in ('on', 'enable', 'true', '1'):
        if not _read_tasks():
            warn("No scheduled tasks yet. Add one first:  task add <secs> <command>")
        regedit.save('Apps.Task_Background', 'true')
        ok("Background tasks ON — they fire while you're idle at the prompt.")
        info("They pause while you type and during a running command. 'task background off' to stop.")
    elif sub in ('off', 'disable', 'false', '0'):
        regedit.save('Apps.Task_Background', 'false')
        ok("Background tasks OFF.")
    elif sub in ('', 'status'):
        info("Background tasks: {}".format("ON" if cur else "OFF"))
        if cur:
            multi("  Scheduled tasks fire while idle at the prompt. See:  task list")
        else:
            multi("  Enable with:  task background on")
    else:
        warn("Usage: task background on|off|status")


def _scheduler():
    """Foreground scheduler loop: fire due tasks; quit on 'q' / Ctrl+C."""
    tasks = _read_tasks()
    if not tasks:
        warn("No scheduled tasks. Add one: task add <secs> <command>")
        return
    lp = sys.modules.get('Core.launchpad') or sys.modules.get('launchpad')
    if lp is None:
        error("Shell engine not available.")
        return

    import utime
    try:
        import select
        has_select = True
    except ImportError:
        try:
            import uselect as select   # some MicroPython ports name it uselect
            has_select = True
        except ImportError:
            has_select = False

    info("Scheduler running — {} task(s).".format(len(tasks)))
    multi("  Press 'q' or Ctrl+C to stop and return to the shell.")
    now = utime.ticks_ms()
    due = [utime.ticks_add(now, interval * 1000) for interval, _ in tasks]

    try:
        while True:
            try:
                if not lp._shell_state.get('running', True):
                    break
            except Exception:
                pass
            now = utime.ticks_ms()
            for i in range(len(tasks)):
                if utime.ticks_diff(now, due[i]) >= 0:
                    interval, cmd = tasks[i]
                    multi("\033[90m[task] {}\033[0m".format(cmd))
                    try:
                        lp._run_line(cmd)
                    except Exception as e:
                        error("task error: {}".format(e))
                    due[i] = utime.ticks_add(utime.ticks_ms(), interval * 1000)
            if has_select:
                r, _, _ = select.select([sys.stdin], [], [], 0.5)
                if r:
                    ch = sys.stdin.read(1)
                    if ch in ('q', 'Q', '\x03', '\x04'):
                        break
            else:
                utime.sleep_ms(500)
    except KeyboardInterrupt:
        pass
    info("Scheduler stopped.")


def task(args=None):
    if not args or not args.strip():
        _task_list()
        return
    parts = args.split(None, 1)
    sub  = parts[0].lower()
    rest = parts[1].strip() if len(parts) > 1 else ''

    if sub == 'list':
        _task_list()
    elif sub == 'add':
        _task_add(rest)
    elif sub in ('remove', 'rm', 'del'):
        _task_remove(rest)
    elif sub == 'clear':
        _task_clear()
    elif sub == 'run':
        _scheduler()
    elif sub in ('background', 'bg'):
        _task_background(rest)
    else:
        error("Unknown subcommand '{}'.".format(sub))
        info("Usage: task [list|add <secs> <cmd>|remove <n>|clear|run|background on|off]")


# ===========================================================================
# Background services  —  long-running coroutines (e.g. httpd) supervised by the
# async shell so they run WHILE you keep using the prompt. (v0.9.5 multitasking.)
#
# A line in services.cfg is just a shell command that registers a service, e.g.
# 'httpd start --bg'. The async shell runs services.cfg at login, so anything
# listed here auto-starts. Live state (running/stopped) is read from the engine.
#   service                 list services (live state) + the services.cfg list
#   service add <command>   add a start command to services.cfg (auto-start)
#   service remove <n>      remove line <n> from services.cfg
#   service clear           empty services.cfg
# ===========================================================================

_SERVICES = '/Pulsar/Registry/services.cfg'
_SHEADER = (
    "# RPCortex background services — one start command per line, run by the\n"
    "# async shell at login. e.g.  httpd start --bg\n"
    "# Manage with: service add/remove/list/clear  (needs 'asyncmode on').\n"
)


def _svc_read():
    try:
        with open(_SERVICES, 'r') as f:
            return [ln.strip() for ln in f
                    if ln.strip() and not ln.strip().startswith('#')]
    except OSError:
        return []


def _svc_write(items):
    try:
        with open(_SERVICES, 'w') as f:
            f.write(_SHEADER)
            for it in items:
                f.write(it + '\n')
        return True
    except OSError as e:
        error("Could not write services.cfg: {}".format(e))
        return False


def _svc_list():
    # Live state from the engine (if the async shell is running).
    lp = sys.modules.get('Core.launchpad') or sys.modules.get('launchpad')
    live = []
    if lp is not None and hasattr(lp, 'list_services'):
        try:
            live = lp.list_services()
        except Exception:
            live = []
    if live:
        multi("  Running services (this session):")
        for name, alive in live:
            tag = ("\033[92mrunning\033[0m" if alive else "\033[90mstopped\033[0m")
            multi("    {:<14} {}".format(name, tag))
        multi("")
    cfg = _svc_read()
    if not cfg:
        multi("  No auto-start services configured.")
        multi("  Add one:  service add \"httpd start --bg\"   (then: asyncmode on)")
        return
    multi("  Auto-start at login (services.cfg):")
    for i, c in enumerate(cfg):
        multi("  {:>2}. {}".format(i + 1, c))
    multi("")
    import regedit
    if (regedit.read('Settings.Async_Shell') or 'false') != 'true':
        warn("  Async shell is OFF — services only run there. Enable:  asyncmode on")


def _svc_add(cmd):
    if not cmd:
        error("Usage: service add <command>   e.g.  service add \"httpd start --bg\"")
        return
    items = _svc_read()
    if cmd in items:
        warn("Already a service: {}".format(cmd))
        return
    items.append(cmd)
    if _svc_write(items):
        ok("Added service: {}".format(cmd))
        import regedit
        if (regedit.read('Settings.Async_Shell') or 'false') != 'true':
            info("It runs in the async shell — enable it:  asyncmode on")
        else:
            info("It starts at your next login. Or start it now:  {}".format(cmd))


def _svc_remove(arg):
    items = _svc_read()
    if not items:
        warn("No services to remove.")
        return
    try:
        n = int(arg)
    except (ValueError, TypeError):
        error("Usage: service remove <number>   (see 'service list')")
        return
    if n < 1 or n > len(items):
        error("No service #{}. There are {}.".format(n, len(items)))
        return
    removed = items.pop(n - 1)
    if _svc_write(items):
        ok("Removed: {}".format(removed))
        info("Already-running copies stop at logout, or now with: httpd stop")


def _svc_clear():
    items = _svc_read()
    if not items:
        multi("  Already empty.")
        return
    if _svc_write([]):
        ok("Cleared {} service(s).".format(len(items)))


def service(args=None):
    """Manage background services (run in the async shell). See 'service list'."""
    if not args or not args.strip():
        _svc_list()
        return
    parts = args.split(None, 1)
    sub = parts[0].lower()
    rest = parts[1].strip() if len(parts) > 1 else ''
    # allow quoted commands:  service add "httpd start --bg"
    if rest and len(rest) >= 2 and rest[0] in ('"', "'") and rest[-1] == rest[0]:
        rest = rest[1:-1]
    if sub in ('list', 'status', 'ls'):
        _svc_list()
    elif sub == 'add':
        _svc_add(rest)
    elif sub in ('remove', 'rm', 'del'):
        _svc_remove(rest)
    elif sub == 'clear':
        _svc_clear()
    elif sub in ('help', '-h', '--help', '?'):
        info("service - manage background services (async shell, v0.9.5)")
        multi("  service                 list running services + auto-start list")
        multi("  service add <command>   add an auto-start command (services.cfg)")
        multi("  service remove <n>      remove line <n>")
        multi("  service clear           empty the list")
        multi("  Services run in the async shell. Enable it:  asyncmode on")
        multi("  Example:  service add \"httpd start --bg\"")
    else:
        error("Unknown subcommand '{}'.".format(sub))
        info("Usage: service [list | add <cmd> | remove <n> | clear]")


def autonomy(args=None):
    """Run the device with no login.  autonomy status | on [user] | off"""
    import regedit
    toks = (args or '').split()
    sub  = toks[0].lower() if toks else 'status'

    if sub == 'status':
        cur = (regedit.read('Settings.Autonomous') or '').strip()
        if cur and cur.lower() not in ('false', '0', 'off', 'no'):
            ok("Autonomy mode: ON — boots straight to a shell as '{}' (no login).".format(cur))
        else:
            info("Autonomy mode: OFF — normal login required.")
        return

    from usrmgmt import require_admin, decode
    if sub == 'on':
        user = toks[1] if len(toks) > 1 else (regedit.read('Settings.Active_User') or 'root')
        if not decode(user, silent=True):
            error("User '{}' not found.".format(user))
            return
        if not require_admin("enable autonomy mode"):
            return
        regedit.save('Settings.Autonomous', user)
        ok("Autonomy mode ON — runs as '{}' with no login.".format(user))
        warn("Anyone with physical access now controls the device. Reboot to apply.")
        info("Tip: pair with startup tasks, e.g.  startup add wifi autoconnect -s")
    elif sub == 'off':
        if not require_admin("disable autonomy mode"):
            return
        regedit.save('Settings.Autonomous', 'false')
        ok("Autonomy mode OFF — normal login restored. Reboot to apply.")
    else:
        warn("Usage: autonomy status | on [user] | off")


def asyncmode(args=None):
    """Toggle the EXPERIMENTAL asyncio shell (v0.9.5 multitasking foundation).

    asyncmode status | on | off

    When ON, the next login runs an async shell where background scheduled tasks
    fire even while you type — the groundwork for v1.0 concurrency. It's
    experimental: editing is basic (no history/cursor-nav/completion yet) and a
    long command still blocks the loop. A crash sentinel falls back to the normal
    shell automatically, so it can't lock you out. Most users want the standard
    shell + 'task background on' instead."""
    import regedit
    sub = (args or '').strip().lower()
    cur = (regedit.read('Settings.Async_Shell') or 'false') == 'true'
    if sub in ('', 'status'):
        info("Async shell (experimental): {}".format("ON" if cur else "OFF"))
        if cur:
            multi("  Next login uses the asyncio shell. Disable:  asyncmode off")
        else:
            multi("  Standard shell in use. For background tasks try:  task background on")
            multi("  Enable the experimental async shell:  asyncmode on")
        return
    if sub in ('on', 'enable', 'true', '1'):
        regedit.save('Settings.Async_Shell', 'true')
        ok("Async shell ENABLED (experimental). Applies at the next login/reboot.")
        warn("Editing is basic here; the standard shell stays the safe default.")
    elif sub in ('off', 'disable', 'false', '0'):
        regedit.save('Settings.Async_Shell', 'false')
        ok("Async shell disabled — back to the standard shell at next login.")
    else:
        warn("Usage: asyncmode status | on | off")
