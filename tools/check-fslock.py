#!/usr/bin/env python3
"""Refuse a littlefs call made without the filesystem lock.

littlefs is not safe to enter from two tasks at once: it keeps a list of open
files and a read cache shared by all of them, and this OS runs tasks on both
cores. storage.cpp guards it with g_fs_lock, and the rule is simple to state and
easy to break: every lfs_*(&g_lfs, ...) call, and every call to a helper that
expects its caller to hold the lock, must sit inside a LockGuard on g_fs_lock.

It was broken twice at once (PAR-63): the read-a-file-in-pieces calls took no
lock at all, and the time stamp after a streamed write was taken after the
lock's scope had closed. Neither was visible in review, and neither fails a
test - a race that corrupts littlefs's open-file list shows up days later as a
disk that reads full. So it is checked from the source instead.

    check-fslock.py [path/to/storage.cpp]
"""
import os
import re
import sys

# Functions that run before the scheduler, single-threaded, at mount time.
BOOT_ONLY = {"storage_init"}

CALL = re.compile(r"\b(lfs_[a-z_]+)\s*\(\s*&g_lfs\b")
FUNC = re.compile(r"^(?:static\s+)?[\w\s\*]+?\b(\w+)\s*\([^;]*\)\s*\{\s*$")
GUARD = re.compile(r"LockGuard\s+\w+\s*\(\s*&g_fs_lock\s*\)")


def strip(line):
    line = re.sub(r"//.*", "", line)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', line)


def scan(path):
    src = open(path).read().splitlines()

    # Helpers whose comment says the caller already holds the lock are found by
    # looking for functions that call littlefs but never take the lock themselves,
    # and are static. Their CALL SITES are then held to the rule instead.
    funcs = []          # (name, start, end, static)
    depth, cur = 0, None
    for i, raw in enumerate(src):
        line = strip(raw)
        if depth == 0:
            m = FUNC.match(line)
            if m:
                cur = (m.group(1), i, line.lstrip().startswith("static"))
        depth += line.count("{") - line.count("}")
        if depth == 0 and cur:
            funcs.append((cur[0], cur[1], i, cur[2]))
            cur = None

    def body(f):
        return src[f[1]:f[2] + 1]

    # A helper is a static function that reaches littlefs (directly, or through
    # another helper) without taking the lock, and is only ever CALLED by name.
    # One whose address is taken - handed out as a callback, the way a file's
    # read function is - runs from whoever holds the pointer, so it gets no
    # excuse: it must take the lock itself.
    whole = "\n".join(strip(l) for l in src)
    texts = {f[0]: "\n".join(strip(l) for l in body(f)) for f in funcs}
    helpers = set()
    changed = True
    while changed:
        changed = False
        reach = CALL.pattern
        if helpers:
            reach = "(?:" + CALL.pattern + r"|\b(?:" + "|".join(sorted(helpers)) + r")\s*\()"
        rx = re.compile(reach)
        for f in funcs:
            name = f[0]
            if name in helpers or name in BOOT_ONLY or not f[3]:
                continue
            t = texts[name]
            if rx.search(t) and not GUARD.search(t):
                pointer_use = re.search(r"\b" + name + r"\b(?!\s*\()", whole.replace(src[f[1]], ""))
                if pointer_use:
                    continue
                helpers.add(name)
                changed = True
    helper_call = re.compile(r"\b(" + "|".join(sorted(helpers)) + r")\s*\(") if helpers else None

    bad = []
    for f in funcs:
        name = f[0]
        if name in BOOT_ONLY or name in helpers:
            continue
        depth, lock_depth = 0, None
        for off, raw in enumerate(body(f)):
            line = strip(raw)
            lineno = f[1] + off + 1
            # Closing braces first: a "}" that ends the locked scope ends it
            # before anything after it on the line.
            for ch in line:
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if lock_depth is not None and depth < lock_depth:
                        lock_depth = None
            if GUARD.search(line) and lock_depth is None:
                lock_depth = depth
            hits = [m.group(1) for m in CALL.finditer(line)]
            if helper_call and off > 0:
                hits += [m.group(1) + "()" for m in helper_call.finditer(line)]
            for h in hits:
                if lock_depth is None:
                    bad.append((lineno, name, h))
    return helpers, bad


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "loader-spike", "firmware", "storage.cpp")
    helpers, bad = scan(path)
    if bad:
        for lineno, fn, call in bad:
            print("  %s:%d  %s in %s() without g_fs_lock" % (os.path.basename(path), lineno, call, fn))
        print("  %d littlefs call(s) outside the filesystem lock" % len(bad))
        return 1
    print("  every littlefs call holds g_fs_lock (helpers held to their callers: %s)" % ", ".join(sorted(helpers)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
