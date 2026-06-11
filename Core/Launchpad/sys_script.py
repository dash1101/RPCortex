# Desc: RPCortex shell scripting (.rps) - Pulsar OS
# File: /Core/Launchpad/sys_script.py
# Last Updated: 6/10/2026
# Lang: MicroPython, English
# Version: v0.9.1
#
# A small, line-oriented scripting language for automating the shell.  Scripts
# run through the live Launchpad engine, so every shell command (and pipes /
# && / ||) is available.  Pair with startup tasks for autonomous behaviour.
#
#   script <file.rps>
#
# Language (first cut):
#   # comment                      blank lines and #-lines are ignored
#   set NAME VALUE                 define a variable (VALUE may contain $vars)
#   $NAME                          expands to the variable's value anywhere
#   <any shell command>           run it (echo, ls, wifi, pipes, && / || ...)
#   if COND                        run the block if COND is true
#     ...
#   else                           (optional)
#     ...
#   end
#   while COND                     repeat the block while COND is true
#     ...
#   end
#
# COND is either a builtin test or a shell command (true when it succeeds):
#   eq A B        true if A == B           ne A B      true if A != B
#   exists PATH   true if the path exists   empty A     true if A is empty
#   <command>     true if the command exits without error

import sys
import uos

if '/Core' not in sys.path:
    sys.path.append('/Core')

from RPCortex import warn, error, info, ok, multi

_MAX_LOOP = 100000   # while-loop guard against runaway scripts


class _ScriptStop(Exception):
    """Raised to unwind the interpreter when the shell logs out mid-script."""


def _abspath(p):
    return p if p.startswith('/') else uos.getcwd().rstrip('/') + '/' + p


def _ident_start(c):
    # MicroPython str has no isalnum(); use explicit ranges so it works anywhere.
    return c == '_' or ('a' <= c <= 'z') or ('A' <= c <= 'Z')


def _ident_char(c):
    return _ident_start(c) or ('0' <= c <= '9')


def _engine():
    return sys.modules.get('Core.launchpad') or sys.modules.get('launchpad')


def _running():
    lp = _engine()
    if lp is None:
        return True
    try:
        return lp._shell_state.get('running', True)
    except Exception:
        return True


def _run(line):
    """Run a command line through the live shell engine; return its exit status."""
    lp = _engine()
    if lp is None:
        error("script: shell engine unavailable.")
        return False
    return lp._run_line(line)


# ---------------------------------------------------------------------------
# Parser — build a nested block structure from the source lines
# ---------------------------------------------------------------------------

def _parse_block(lines, i, stop):
    """Parse statements until a line whose first token is in `stop`.

    Returns (block, index_of_stop_line).  Raises ValueError on a missing end.
    Statement tuples: ('set', rest) | ('cmd', text) |
                      ('if', cond, then_block, else_block) | ('while', cond, body)
    """
    block = []
    n = len(lines)
    while i < n:
        raw = lines[i].strip()
        if not raw or raw.startswith('#'):
            i += 1
            continue
        sp   = raw.split(None, 1)
        head = sp[0]
        rest = sp[1].strip() if len(sp) > 1 else ''
        if head in stop:
            return block, i
        if head == 'if':
            then_blk, j = _parse_block(lines, i + 1, ('else', 'end'))
            if j >= n:
                raise ValueError("'if' without matching 'end'")
            else_blk = []
            if lines[j].strip().split(None, 1)[0] == 'else':
                else_blk, j = _parse_block(lines, j + 1, ('end',))
                if j >= n:
                    raise ValueError("'else' without matching 'end'")
            block.append(('if', rest, then_blk, else_blk))
            i = j + 1
        elif head == 'while':
            body, j = _parse_block(lines, i + 1, ('end',))
            if j >= n:
                raise ValueError("'while' without matching 'end'")
            block.append(('while', rest, body))
            i = j + 1
        elif head in ('else', 'end'):
            raise ValueError("unexpected '{}'".format(head))
        elif head == 'set':
            block.append(('set', rest))
            i += 1
        else:
            block.append(('cmd', raw))
            i += 1
    return block, i


# ---------------------------------------------------------------------------
# Interpreter
# ---------------------------------------------------------------------------

class _Interp:
    def __init__(self, lines):
        self.vars = {}
        self.block, _ = _parse_block(lines, 0, ())

    def _expand(self, s):
        if '$' not in s:
            return s
        out = []
        i, n = 0, len(s)
        while i < n:
            c = s[i]
            if c == '$' and i + 1 < n and _ident_start(s[i + 1]):
                j = i + 1
                while j < n and _ident_char(s[j]):
                    j += 1
                out.append(str(self.vars.get(s[i + 1:j], '')))
                i = j
            else:
                out.append(c)
                i += 1
        return ''.join(out)

    def _do_set(self, rest):
        p = rest.split(None, 1)
        if not p or not p[0]:
            error("script: 'set' needs a name.")
            return
        self.vars[p[0]] = self._expand(p[1]) if len(p) > 1 else ''

    def _cond(self, cond):
        cond = self._expand(cond).strip()
        if not cond:
            return False
        parts = cond.split()
        op = parts[0]
        if op == 'eq':
            return len(parts) >= 3 and parts[1] == parts[2]
        if op == 'ne':
            return not (len(parts) >= 3 and parts[1] == parts[2])
        if op == 'empty':
            return len(parts) < 2 or parts[1] == ''
        if op == 'exists':
            if len(parts) < 2:
                return False
            try:
                uos.stat(_abspath(parts[1]))
                return True
            except OSError:
                return False
        # Otherwise: run as a shell command, true if it exits without error
        return bool(_run(cond))

    def _exec(self, block):
        for st in block:
            if not _running():
                raise _ScriptStop
            kind = st[0]
            if kind == 'cmd':
                _run(self._expand(st[1]))
            elif kind == 'set':
                self._do_set(st[1])
            elif kind == 'if':
                self._exec(st[2] if self._cond(st[1]) else st[3])
            elif kind == 'while':
                guard = 0
                while self._cond(st[1]):
                    self._exec(st[2])
                    guard += 1
                    if guard >= _MAX_LOOP:
                        error("script: while exceeded {} iterations — aborting.".format(_MAX_LOOP))
                        return

    def run(self):
        try:
            self._exec(self.block)
        except _ScriptStop:
            pass


def script(args):
    if not args or not args.strip():
        warn("Usage: script <file.rps>")
        return
    path = _abspath(args.strip().split()[0])
    try:
        with open(path, 'r') as f:
            lines = [ln.rstrip('\n') for ln in f]
    except OSError as e:
        error("Cannot read script '{}': {}".format(path, e))
        return
    try:
        interp = _Interp(lines)
    except ValueError as e:
        error("Script parse error: {}".format(e))
        return
    interp.run()
