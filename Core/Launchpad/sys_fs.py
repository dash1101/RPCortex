# Desc: Filesystem shell commands - RPCortex Nebula OS
# File: /Core/Launchpad/sys_fs.py
# Last Updated: 3/25/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta3

import sys
import uos
import utime

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, error, info, ok, multi, inpt

_CD = "\033[96m"   # cyan   — directories
_CF = "\033[93m"   # yellow — files
_CT = "\033[90m"   # gray   — timestamps / sizes
_CR = "\033[0m"    # reset


def _fmt_size(n):
    if n < 1024:
        return "{}B".format(n)
    elif n < 1048576:
        k = n / 1024.0
        return "{:.0f}K".format(k) if k >= 10 else "{:.1f}K".format(k)
    else:
        m = n / 1048576.0
        return "{:.0f}M".format(m) if m >= 10 else "{:.1f}M".format(m)


def ls(args=None):
    cwd = uos.getcwd()
    if args:
        target = args if args.startswith('/') else cwd.rstrip('/') + '/' + args
    else:
        target = cwd
    try:
        items = uos.listdir(target)
    except OSError as e:
        error("Cannot list directory '{}': {}".format(target, e))
        return
    if not items:
        warn("Directory is empty.")
        return
    multi("  {:<5}  {:<7}  {:<19}  {}".format("TYPE", "SIZE", "MODIFIED", "NAME"))
    multi("  " + "-" * 58)
    for item in sorted(items):
        try:
            full   = target.rstrip('/') + '/' + item
            st     = uos.stat(full)
            is_dir = st[0] & 0x4000
            lt     = utime.localtime(st[8])
            ts     = "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}".format(
                         lt[0], lt[1], lt[2], lt[3], lt[4], lt[5])
            size_s = '-' if is_dir else _fmt_size(st[6])
            color  = _CD if is_dir else _CF
            typ    = 'DIR' if is_dir else 'FILE'
            name   = item + '/' if is_dir else item
            multi(
                "  " + color + "{:<5}".format(typ) + _CR +
                "  " + _CT + "{:<7}".format(size_s) + _CR +
                "  " + _CT + ts + _CR +
                "  " + color + name + _CR
            )
        except OSError:
            multi("  ERROR  {:<7}  {:<19}  {}".format("?", "?", item))


def cd(args):
    home = globals().get('_shell_state', {}).get('home', '/')
    if not args or args.strip() == '~':
        target = home.rstrip('/') or '/'
    elif args.startswith('~/'):
        target = home.rstrip('/') + args[1:]
    elif args.startswith('/'):
        target = args
    else:
        target = uos.getcwd().rstrip('/') + '/' + args
    try:
        uos.chdir(target)
    except OSError:
        error("Cannot find path '{}'".format(args or '~'))


def pwd(args=None):
    multi(uos.getcwd())


def touch(args):
    if not args:
        warn("Usage: touch <file>")
        return
    path = args if args.startswith('/') else uos.getcwd().rstrip('/') + '/' + args
    try:
        uos.stat(path)
        warn("'{}' already exists.".format(path))
    except OSError:
        try:
            with open(path, 'w'):
                pass
            ok("Created '{}'".format(path))
        except OSError as e:
            error("Cannot create '{}': {}".format(path, e))


def mkdir(args):
    if not args:
        warn("Usage: mkdir <path>")
        return
    path = args if args.startswith('/') else uos.getcwd().rstrip('/') + '/' + args
    try:
        uos.mkdir(path)
        ok("Created directory '{}'".format(path))
    except OSError as e:
        error("Cannot create '{}': {}".format(path, e))


def delete(args):
    if not args:
        warn("Usage: rm <path>")
        return
    path = args if args.startswith('/') else uos.getcwd().rstrip('/') + '/' + args
    try:
        uos.stat(path)
    except OSError:
        error("'{}' does not exist.".format(path))
        return

    def _del(p, _mode=None):
        # Returns: 'y' deleted, 'n' skipped, 'a' all-delete mode, 'c' cancel all
        if _mode == 'c':
            return 'c'
        try:
            st = uos.stat(p)
        except OSError as e:
            error("Cannot access '{}': {}".format(p, e))
            return 'n'
        if st[0] & 0x4000:
            # Directory — recurse, then rmdir only if every entry was deleted
            cur_mode = _mode
            can_rmdir = True
            try:
                entries = uos.listdir(p)
            except OSError as e:
                error("Cannot list '{}': {}".format(p, e))
                return 'n'
            for entry in entries:
                child = p.rstrip('/') + '/' + entry
                result = _del(child, cur_mode)
                if result == 'c':
                    return 'c'
                if result == 'a':
                    cur_mode = 'a'
                if result == 'n':
                    can_rmdir = False
            if can_rmdir:
                try:
                    uos.rmdir(p)
                    info("Removed dir: {}".format(p))
                except OSError as e:
                    warn("Could not remove '{}': {}".format(p, e))
                    return 'n'
                return cur_mode if cur_mode else 'y'
            else:
                info("'{}' kept — some entries were skipped.".format(p))
                return 'n'
        else:
            # File
            if _mode == 'a':
                try:
                    uos.remove(p)
                    info("Deleted: {}".format(p))
                except OSError as e:
                    error("Error: {}".format(e))
                return 'a'
            while True:
                r = inpt("Delete '{}' ? (y/n/a/c)".format(p)).strip().lower()
                if r == 'y':
                    try:
                        uos.remove(p)
                        info("Deleted: {}".format(p))
                    except OSError as e:
                        error("Error: {}".format(e))
                    return 'y'
                elif r == 'n':
                    info("Skipped: {}".format(p))
                    return 'n'
                elif r == 'a':
                    try:
                        uos.remove(p)
                        info("Deleted: {}".format(p))
                    except OSError as e:
                        error("Error: {}".format(e))
                    return 'a'
                elif r == 'c':
                    info("Cancelled.")
                    return 'c'
                else:
                    warn("Enter y, n, a, or c.")
    _del(path)


def read(args):
    if not args:
        warn("Usage: read <file>")
        return
    path = args if args.startswith('/') else uos.getcwd().rstrip('/') + '/' + args
    try:
        if uos.stat(path)[0] & 0x4000:
            error("'{}' is a directory.".format(path))
            return
        with open(path, 'r') as f:
            multi(f.read())
    except OSError as e:
        error("Cannot read '{}': {}".format(path, e))


def head(args):
    if not args:
        warn("Usage: head <file> [n]")
        return
    parts    = args.split(None, 1)
    filepath = parts[0]
    n = 10
    if len(parts) > 1:
        try:
            n = int(parts[1])
        except ValueError:
            warn("Invalid line count, defaulting to 10.")
    path = filepath if filepath.startswith('/') else uos.getcwd().rstrip('/') + '/' + filepath
    try:
        with open(path, 'r') as f:
            for i, line in enumerate(f):
                if i >= n:
                    break
                multi(line.rstrip('\n'))
    except OSError as e:
        error("Cannot read '{}': {}".format(path, e))


def tail(args):
    if not args:
        warn("Usage: tail <file> [n]")
        return
    parts    = args.split(None, 1)
    filepath = parts[0]
    n = 10
    if len(parts) > 1:
        try:
            n = int(parts[1])
        except ValueError:
            warn("Invalid line count, defaulting to 10.")
    path = filepath if filepath.startswith('/') else uos.getcwd().rstrip('/') + '/' + filepath
    try:
        with open(path, 'r') as f:
            lines = f.readlines()
        for line in lines[-n:]:
            multi(line.rstrip('\n'))
    except OSError as e:
        error("Cannot read '{}': {}".format(path, e))


def execute(args):
    if not args:
        warn("Usage: exec <file.py>")
        return
    path = args if args.startswith('/') else uos.getcwd().rstrip('/') + '/' + args
    try:
        uos.stat(path)
    except OSError:
        error("File '{}' does not exist.".format(path))
        return
    try:
        with open(path, 'r') as f:
            code = f.read()
        exec(code)
    except Exception as e:
        error("Error executing '{}': {}".format(path, e))


def rename(args):
    if not args:
        warn("Usage: rename <old_path> <new_path>")
        return
    parts = _split_two(args)
    if parts is None:
        error("Provide exactly two absolute paths.")
        return
    old, new = parts
    if not old.startswith('/') or not new.startswith('/'):
        error("Both paths must be absolute.")
        return
    try:
        uos.rename(old, new)
        ok("Renamed '{}' to '{}'".format(old, new))
    except OSError as e:
        error("Cannot rename: {}".format(e))


def move(args):
    if not args:
        warn("Usage: mv <source> <dest>")
        return
    parts = _split_two(args)
    if parts is None:
        error("Provide exactly two absolute paths.")
        return
    src, dst = parts
    if not src.startswith('/') or not dst.startswith('/'):
        error("Both paths must be absolute.")
        return
    try:
        try:
            if uos.stat(dst)[0] & 0x4000:
                dst = dst.rstrip('/') + '/' + src.split('/')[-1]
        except OSError:
            pass
        with open(src, 'rb') as sf:
            data = sf.read()
        with open(dst, 'wb') as df:
            df.write(data)
        uos.remove(src)
        ok("Moved '{}' to '{}'".format(src, dst))
    except OSError as e:
        error("Cannot move: {}".format(e))


def copy(args):
    if not args:
        warn("Usage: cp <source> <dest>")
        return
    parts = _split_two(args)
    if parts is None:
        error("Provide exactly two absolute paths.")
        return
    src, dst = parts
    if not src.startswith('/') or not dst.startswith('/'):
        error("Both paths must be absolute.")
        return
    try:
        try:
            if uos.stat(dst)[0] & 0x4000:
                dst = dst.rstrip('/') + '/' + src.split('/')[-1]
        except OSError:
            pass
        with open(src, 'rb') as sf:
            data = sf.read()
        with open(dst, 'wb') as df:
            df.write(data)
        ok("Copied '{}' to '{}'".format(src, dst))
    except OSError as e:
        error("Cannot copy: {}".format(e))


def diskfree(args=None):
    try:
        sv    = uos.statvfs('/')
        total = sv[0] * sv[2]
        free  = sv[0] * sv[3]
        used  = total - free
        pct   = used * 100 // total if total else 0
        multi("  Total : {} KB".format(total // 1024))
        multi("  Used  : {} KB  ({}%)".format(used // 1024, pct))
        multi("  Free  : {} KB".format(free // 1024))
    except OSError as e:
        error("Cannot retrieve disk info: {}".format(e))


def tree(args=None):
    root = args.strip() if args else uos.getcwd()
    multi(root)

    def _tree(path, prefix=""):
        try:
            items = sorted(uos.listdir(path))
        except OSError:
            return
        for i, item in enumerate(items):
            last      = (i == len(items) - 1)
            connector = "\u2514\u2500\u2500 " if last else "\u251c\u2500\u2500 "
            full      = path.rstrip('/') + '/' + item
            multi(prefix + connector + item)
            try:
                if uos.stat(full)[0] & 0x4000:
                    _tree(full, prefix + ("    " if last else "\u2502   "))
            except OSError:
                pass
    _tree(root)


def _split_two(args):
    if args.startswith(("'", '"')):
        parts, current, in_q, qchar = [], "", False, None
        for ch in args:
            if ch in ("'", '"') and (not in_q or ch == qchar):
                in_q = not in_q
                qchar = ch if in_q else None
            elif ch == " " and not in_q:
                if current:
                    parts.append(current)
                    current = ""
            else:
                current += ch
        if current:
            parts.append(current)
    else:
        parts = args.split(None, 1)
    return parts if len(parts) == 2 else None
