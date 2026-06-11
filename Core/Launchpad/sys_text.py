# Desc: Text processing shell commands - RPCortex Pulsar OS
# File: /Core/Launchpad/sys_text.py
# Last Updated: 6/9/2026
# Lang: MicroPython, English
# Version: v0.8.2
# Author: dash1101
#
# Provides grep, wc, find, sort, uniq, hex, basename, dirname.
# All I/O is line-by-line where possible to keep RAM usage low.

import sys
import uos

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, error, info, ok, multi, is_capturing

_RST  = '\033[0m'
_CY   = '\033[96m'    # cyan  — match highlight
_GR   = '\033[90m'    # gray  — line numbers


def _resolve(path):
    """Resolve a relative path against cwd."""
    if path.startswith('/'):
        return path
    return uos.getcwd().rstrip('/') + '/' + path


def _stdin_text():
    """Piped input text for the current command, or None if not piped."""
    return globals().get('_shell_state', {}).get('stdin')


def _iter_input(filepath, stdin):
    """Yield input lines (with trailing newline) from a file, else piped stdin.

    A file is streamed line-by-line (low RAM); piped text is already in RAM.
    Lets every text command work on either `grep x file` or `... | grep x`.
    """
    if filepath is not None:
        path = _resolve(filepath)
        with open(path, 'r') as f:
            for line in f:
                yield line
    elif stdin is not None:
        parts = stdin.split('\n')
        if parts and parts[-1] == '':
            parts = parts[:-1]   # drop the empty tail from a trailing newline
        for p in parts:
            yield p + '\n'


# ---------------------------------------------------------------------------
# grep  — search file for pattern (substring match)
# ---------------------------------------------------------------------------

def grep(args):
    parts = args.split(None, 1) if args else []
    if not parts:
        warn("Usage: grep <pattern> <file>   (or  ... | grep <pattern>)")
        return
    pattern  = parts[0]
    filepath = parts[1].strip() if len(parts) > 1 else None
    stdin    = _stdin_text()
    if filepath is None and stdin is None:
        warn("Usage: grep <pattern> <file>")
        return
    try:
        count = 0
        lnum  = 0
        for line in _iter_input(filepath, stdin):
            lnum += 1
            if pattern in line:
                count += 1
                multi(_GR + "{:4d}".format(lnum) + _RST + "  " + line.rstrip('\n'))
        if not is_capturing():   # summary is status, not data — skip when piped onward
            if count == 0:
                info("No matches for '{}'.".format(pattern))
            elif filepath:
                ok("{} match(es) in '{}'.".format(count, filepath))
            else:
                ok("{} match(es).".format(count))
    except OSError as e:
        error("Cannot read input: {}".format(e))


# ---------------------------------------------------------------------------
# wc  — word / line / byte count
# ---------------------------------------------------------------------------

def wc(args):
    filepath = args.strip() if args and args.strip() else None
    stdin    = _stdin_text()
    if filepath is None and stdin is None:
        warn("Usage: wc <file>   (or  ... | wc)")
        return
    try:
        lines = 0
        words = 0
        byt   = 0
        for line in _iter_input(filepath, stdin):
            lines += 1
            words += len(line.split())
            byt   += len(line)
        multi("  Lines : {}".format(lines))
        multi("  Words : {}".format(words))
        multi("  Bytes : {}".format(byt))
    except OSError as e:
        error("Cannot read input: {}".format(e))


# ---------------------------------------------------------------------------
# find  — recursive file search by name substring
# ---------------------------------------------------------------------------

def find(args=None):
    """find [dir] [pattern]  — search for files by name"""
    root    = uos.getcwd()
    pattern = None

    if args:
        parts = args.strip().split(None, 1)
        if parts[0].startswith('/'):
            root    = parts[0]
            pattern = parts[1].strip() if len(parts) > 1 else None
        else:
            pattern = parts[0]
            if len(parts) > 1:
                root = parts[1].strip()

    results = []

    def _walk(p, depth=0):
        if depth > 8:
            return
        try:
            entries = uos.listdir(p)
        except OSError:
            return
        for name in entries:
            full = p.rstrip('/') + '/' + name
            if pattern is None or pattern in name:
                results.append(full)
            try:
                if uos.stat(full)[0] & 0x4000:
                    _walk(full, depth + 1)
            except OSError:
                pass

    _walk(root)

    if not results:
        if not is_capturing():
            info("No files found.")
    else:
        for r in sorted(results):
            multi("  " + r)
        if not is_capturing():
            ok("{} result(s).".format(len(results)))


# ---------------------------------------------------------------------------
# sort  — print lines of a file sorted alphabetically
# ---------------------------------------------------------------------------

def sort(args):
    filepath = args.strip() if args and args.strip() else None
    stdin    = _stdin_text()
    if filepath is None and stdin is None:
        warn("Usage: sort <file>   (or  ... | sort)")
        return
    try:
        if filepath is not None:
            path = _resolve(filepath)
            try:
                sz = uos.stat(path)[6]
            except OSError as e:
                error("Cannot access '{}': {}".format(filepath, e))
                return
            if sz > 8192:
                warn("Large file ({} KB) — loading into RAM. Run 'freeup' first if low on memory.".format(
                    sz // 1024))
        lines = [line for line in _iter_input(filepath, stdin)]
        for line in sorted(lines):
            multi(line.rstrip('\n'))
    except OSError as e:
        error("Cannot read input: {}".format(e))
    except MemoryError:
        error("Not enough RAM to sort this input. Run 'freeup' and retry.")


# ---------------------------------------------------------------------------
# uniq  — remove consecutive duplicate lines
# ---------------------------------------------------------------------------

def uniq(args):
    filepath = args.strip() if args and args.strip() else None
    stdin    = _stdin_text()
    if filepath is None and stdin is None:
        warn("Usage: uniq <file>   (or  ... | uniq)")
        return
    try:
        prev = None
        for line in _iter_input(filepath, stdin):
            s = line.rstrip('\n')
            if s != prev:
                multi(s)
                prev = s
    except OSError as e:
        error("Cannot read input: {}".format(e))


# ---------------------------------------------------------------------------
# hex  — hexdump (first n bytes, default 256)
# ---------------------------------------------------------------------------

def hex_dump(args):
    if not args:
        warn("Usage: hex <file> [n]")
        return
    parts = args.strip().split(None, 1)
    filepath = parts[0]
    n = 256
    if len(parts) > 1:
        try:
            n = int(parts[1])
        except ValueError:
            warn("Invalid byte count — using default 256.")
    path = _resolve(filepath)
    try:
        with open(path, 'rb') as f:
            data = f.read(n)
        if not data:
            info("File is empty.")
            return
        for i in range(0, len(data), 16):
            chunk    = data[i:i + 16]
            hex_part = ' '.join('{:02x}'.format(b) for b in chunk)
            asc_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
            multi("  {:04x}  {:<48}  {}".format(i, hex_part, asc_part))
        if len(data) == n:
            ok("{} bytes shown.  Pass a larger n to see more.".format(n))
        else:
            ok("{} bytes (entire file).".format(len(data)))
    except OSError as e:
        error("Cannot read '{}': {}".format(filepath, e))


# ---------------------------------------------------------------------------
# basename / dirname
# ---------------------------------------------------------------------------

def basename(args):
    """basename <path>  — file name portion of a path"""
    if not args:
        warn("Usage: basename <path>")
        return
    p = args.strip().rstrip('/')
    multi(p.split('/')[-1] if '/' in p else p)


def dirname(args):
    """dirname <path>  — directory portion of a path"""
    if not args:
        warn("Usage: dirname <path>")
        return
    p = args.strip().rstrip('/')
    if '/' in p:
        d = p[:p.rfind('/')]
        multi(d if d else '/')
    else:
        multi('.')
