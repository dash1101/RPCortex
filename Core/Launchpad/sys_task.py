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
    else:
        error("Unknown subcommand '{}'.".format(sub))
        info("Usage: task [list|add <secs> <cmd>|remove <n>|clear|run]")
