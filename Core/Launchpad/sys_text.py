# Desc: Text processing shell commands - RPCortex Nebula OS
# File: /Core/Launchpad/sys_text.py
# Last Updated: 4/1/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta4
# Author: dash1101
#
# Provides grep, wc, find, sort, uniq, hex, basename, dirname.
# All I/O is line-by-line where possible to keep RAM usage low.

import sys
import uos

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, error, info, ok, multi

_RST  = '\033[0m'
_CY   = '\033[96m'    # cyan  — match highlight
_GR   = '\033[90m'    # gray  — line numbers


def _resolve(path):
    """Resolve a relative path against cwd."""
    if path.startswith('/'):
        return path
    return uos.getcwd().rstrip('/') + '/' + path


# ---------------------------------------------------------------------------
# grep  — search file for pattern (substring match)
# ---------------------------------------------------------------------------

def grep(args):
    if not args:
        warn("Usage: grep <pattern> <file>")
        return
    parts = args.split(None, 1)
    if len(parts) < 2:
        warn("Usage: grep <pattern> <file>")
        return
    pattern  = parts[0]
    filepath = parts[1].strip()
    path = _resolve(filepath)
    try:
        count = 0
        lnum  = 0
        with open(path, 'r') as f:
            for line in f:
                lnum += 1
                if pattern in line:
                    count += 1
                    multi(_GR + "{:4d}".format(lnum) + _RST + "  " + line.rstrip('\n'))
        if count == 0:
            info("No matches for '{}'.".format(pattern))
        else:
            ok("{} match(es) in '{}'.".format(count, filepath))
    except OSError as e:
        error("Cannot read '{}': {}".format(path, e))


# ---------------------------------------------------------------------------
# wc  — word / line / byte count
# ---------------------------------------------------------------------------

def wc(args):
    if not args:
        warn("Usage: wc <file>")
        return
    path = _resolve(args.strip())
    try:
        lines = 0
        words = 0
        byt   = 0
        with open(path, 'r') as f:
            for line in f:
                lines += 1
                words += len(line.split())
                byt   += len(line)
        multi("  Lines : {}".format(lines))
        multi("  Words : {}".format(words))
        multi("  Bytes : {}".format(byt))
    except OSError as e:
        error("Cannot read '{}': {}".format(args.strip(), e))


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
        info("No files found.")
    else:
        for r in sorted(results):
            multi("  " + r)
        ok("{} result(s).".format(len(results)))


# ---------------------------------------------------------------------------
# sort  — print lines of a file sorted alphabetically
# ---------------------------------------------------------------------------

def sort(args):
    if not args:
        warn("Usage: sort <file>")
        return
    path = _resolve(args.strip())
    try:
        sz = uos.stat(path)[6]
    except OSError as e:
        error("Cannot access '{}': {}".format(args.strip(), e))
        return
    if sz > 8192:
        warn("Large file ({} KB) — loading into RAM. Run 'freeup' first if low on memory.".format(
            sz // 1024))
    try:
        with open(path, 'r') as f:
            lines = f.readlines()
        for line in sorted(lines):
            multi(line.rstrip('\n'))
    except OSError as e:
        error("Cannot read '{}': {}".format(args.strip(), e))
    except MemoryError:
        error("Not enough RAM to sort this file. Run 'freeup' and retry.")


# ---------------------------------------------------------------------------
# uniq  — remove consecutive duplicate lines
# ---------------------------------------------------------------------------

def uniq(args):
    if not args:
        warn("Usage: uniq <file>")
        return
    path = _resolve(args.strip())
    try:
        prev = None
        with open(path, 'r') as f:
            for line in f:
                s = line.rstrip('\n')
                if s != prev:
                    multi(s)
                    prev = s
    except OSError as e:
        error("Cannot read '{}': {}".format(args.strip(), e))


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
