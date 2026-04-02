# Desc: Launchpad CLI shell engine for RPCortex - Nebula OS
# File: /Core/launchpad.py
# Last Updated: 4/1/2026
# Lang: MicroPython, English
# Version: v0.8.1
# Author: dash1101

import sys
import uos

if "/Core" not in sys.path:
    sys.path.append("/Core")

import regedit
from usrmgmt import decode
from RPCortex import (
    fatal, error, info, warn, ok, multi,
    init_session_log, close_session_log
)

# ---------------------------------------------------------------------------
# Command registry  (populated from .lp files at shell start)
# ---------------------------------------------------------------------------

commands = {}

def load_commands():
    """Load all .lp command definition files from /Core/Launchpad/."""
    base = "/Core/Launchpad/"
    _load_lp(base + "system.lp")      # always first
    try:
        for name in uos.listdir(base):
            if name.endswith('.lp') and name != 'system.lp':
                _load_lp(base + name)
    except OSError:
        pass

def _load_lp(path):
    try:
        with open(path, 'r') as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith('#'):
                    continue
                parts = line.split(':', 1)
                if len(parts) != 2:
                    warn("Malformed .lp entry (skipping): {}".format(line))
                    continue
                cmd, func_path = parts
                commands[cmd.strip()] = func_path.strip()
    except OSError:
        pass   # optional files (e.g. programs.lp) may not exist yet

# ---------------------------------------------------------------------------
# Module cache
#
# /Core/Launchpad/*.py files are loaded via __import__() into sys.modules.
# MicroPython's C-level import machinery is more heap-efficient than exec(),
# and once a module is in sys.modules it NEVER needs re-reading or
# re-compiling — _cmd_cache.clear() makes the next call a free dict lookup.
# This eliminates MemoryError-on-retry entirely for built-in commands.
#
# Files outside /Core/Launchpad/ (e.g. installed packages) still use exec()
# so they don't pollute sys.modules permanently.
#
# Shell state injected into every loaded scope:
#   _cmd_history  — the shell history list (for the `history` command)
#   _shell_state  — {'running': bool, 'home': str}
#   _cmd_cache    — the cache dict itself (for freeup)
#   _load_commands — reloads the commands dict (for pkg install/remove)
#   _commands     — the live commands dict (for pkg remove to purge entries)
# ---------------------------------------------------------------------------

_cmd_cache   = {}               # file_path -> module or scope dict
_history     = []               # command history (most recent last)
_HIST_MAX    = 50
_shell_state = {'running': False, 'home': '/'}  # mutable; cached scopes hold a reference
_aliases     = {}               # name -> expanded command string (session-local)

# ---------------------------------------------------------------------------
# Critical built-in commands — inline handlers that NEVER go through exec().
#
# The exec()-based loader reads a command file (~10 KB) into a contiguous
# string, then compiles it to bytecode — both require a contiguous heap
# block.  With a fragmented heap (lots of small free regions totalling 90+
# KB) those allocations can still raise MemoryError even though gc.mem_free()
# looks fine.  Commands below bypass that path entirely so that reboot/freeup
# remain reachable no matter how bad heap fragmentation gets.
# ---------------------------------------------------------------------------

def _crit_reboot(args=None):
    import machine as _m
    info("Rebooting system...")
    close_session_log()
    try:
        regedit.save("Settings.Startup", "0")  # mark as clean shutdown
    except Exception:
        pass   # best-effort — reboot must always work even if registry write fails
    _m.reset()

def _crit_sreboot(args=None):
    import machine as _m
    info("Performing soft reboot...")
    close_session_log()
    try:
        regedit.save("Settings.Startup", "0")  # mark as clean shutdown
    except Exception:
        pass
    _m.soft_reset()

def _crit_alias(args=None):
    if not args:
        if not _aliases:
            info("No aliases defined. Usage: alias name=command")
        else:
            for name, val in sorted(_aliases.items()):
                multi("  {} = {}".format(name, val))
        return
    if '=' not in args:
        warn("Usage: alias name=command   (or bare 'alias' to list)")
        return
    idx  = args.index('=')
    name = args[:idx].strip()
    val  = args[idx + 1:].strip()
    if not name:
        warn("Alias name cannot be empty.")
        return
    if name in _CRITICAL:
        warn("Cannot shadow a critical built-in: {}".format(name))
        return
    _aliases[name] = val
    ok("alias {} = {}".format(name, val))


def _crit_unalias(args=None):
    if not args:
        warn("Usage: unalias <name>")
        return
    name = args.strip()
    if name in _aliases:
        del _aliases[name]
        ok("Alias '{}' removed.".format(name))
    else:
        warn("No alias named '{}'.".format(name))


def _crit_rawrepl(args=None):
    """Exit the entire OS and return to MicroPython REPL.

    Use this when you want to flash a fresh install via the Web Installer
    without doing a full reinstall. After this command:
      1. MicroPython REPL becomes active  (>>> prompt)
      2. Open install.html in Chrome/Edge
      3. Click Connect Device and flash normally

    SystemExit (BaseException) propagates through all except-Exception
    handlers and is not caught by the shell loop, initialization.py, or
    main.py — MicroPython drops to REPL after main.py returns.
    """
    info("Exiting to MicroPython REPL...")
    multi("  Connect with the Web Installer: rpc.novalabs.app/install.html")
    close_session_log()
    try:
        regedit.save("Settings.Startup", "0")
    except Exception:
        pass
    raise SystemExit(0)


def _crit_freeup(args=None):
    import gc as _gc
    before = _gc.mem_free()
    _cmd_cache.clear()
    _gc.collect()
    after = _gc.mem_free()
    ok("Memory freed: {} KB -> {} KB free  (+{} KB)".format(
        before // 1024, after // 1024, (after - before) // 1024))

def _crit_xfer(args=None):
    """Receive a file over serial via base64 protocol.

    Used by the web package installer to push files to the device
    without leaving the shell or entering raw REPL.

    Protocol:
      1. Caller sends:  _xfer /dest/path
      2. Device prints:  XFER_READY\\r\\n
      3. Caller sends base64-encoded lines (one chunk per line)
      4. Caller sends:  XFER_END\\n
      5. Device prints:  XFER_OK:<bytes>\\r\\n
      6. If .pkg: auto-installs, prints XFER_INSTALLED or XFER_INSTALL_ERR
      7. Device prints:  XFER_COMPLETE\\r\\n
    """
    if not args:
        error("Usage: _xfer <destination_path>")
        return

    dest = args.strip().split(None, 1)[0]

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

    # Auto-install if it is a .pkg file
    if dest.endswith('.pkg'):
        sys.stdout.write("XFER_INSTALLING\r\n")
        try:
            import gc as _gc2
            _gc2.collect()
            import pkgmgr
            result = pkgmgr.install(dest)
            if result:
                # Reload commands so newly installed command is live
                try:
                    load_commands()
                except Exception:
                    pass
                sys.stdout.write("XFER_INSTALLED\r\n")
            else:
                sys.stdout.write("XFER_INSTALL_FAILED\r\n")
        except Exception as e:
            sys.stdout.write("XFER_INSTALL_ERR:{}\r\n".format(e))
        # Clean up temp .pkg file
        try:
            uos.remove(dest)
        except OSError:
            pass

    sys.stdout.write("XFER_COMPLETE\r\n")


def _crit_recovery(args=None):
    """Enter recovery mode manually from a running shell session."""
    info("Entering recovery mode...")
    close_session_log()
    _shell_state['running'] = False
    recovery_init("Manual entry via 'recovery' command.")


_CRITICAL = {
    'reboot':    _crit_reboot,
    'sreboot':   _crit_sreboot,
    'softreset': _crit_sreboot,
    'freeup':    _crit_freeup,
    'gc':        _crit_freeup,
    '_xfer':     _crit_xfer,
    'alias':     _crit_alias,
    'unalias':   _crit_unalias,
    'rawrepl':   _crit_rawrepl,
    'recovery':  _crit_recovery,
}


def _inject(target, is_module):
    """Inject shell state references into a module or scope dict."""
    if is_module:
        target._cmd_history   = _history
        target._shell_state   = _shell_state
        target._cmd_cache     = _cmd_cache
        target._load_commands = load_commands
        target._commands      = commands
        target._aliases       = _aliases
    else:
        target['_cmd_history']   = _history
        target['_shell_state']   = _shell_state
        target['_cmd_cache']     = _cmd_cache
        target['_load_commands'] = load_commands
        target['_commands']      = commands
        target['_aliases']       = _aliases


_LP_DIR = '/Core/Launchpad/'

def _lp_import(file_path):
    """Load a /Core/Launchpad/*.py file via __import__ (heap-efficient)."""
    slash = file_path.rfind('/')
    name  = file_path[slash + 1:]
    if name.endswith('.py'):
        name = name[:-3]
    dir_path = file_path[:slash]
    if dir_path not in sys.path:
        sys.path.append(dir_path)
    if name in sys.modules:
        mod = sys.modules[name]
    else:
        try:
            mod = __import__(name)
        except Exception as e:
            error("Failed to import '{}': {}".format(file_path, e))
            return None
    _inject(mod, True)
    return mod


def _exec_scope(file_path):
    """Load an arbitrary .py file via exec() into a fresh scope dict."""
    try:
        with open(file_path, 'r') as f:
            code = f.read()
    except OSError:
        error("Command file not found: '{}'".format(file_path))
        return None
    scope = {'__name__': '__lp_cmd__'}
    _inject(scope, False)
    try:
        exec(code, scope)
    except Exception as e:
        error("Failed to load '{}': {}".format(file_path, e))
        return None
    return scope


def _get_scope(file_path):
    """Return (and if necessary build) the scope/module for a command file."""
    if file_path not in _cmd_cache:
        if file_path.startswith(_LP_DIR) and file_path.endswith('.py'):
            scope = _lp_import(file_path)
        else:
            scope = _exec_scope(file_path)
        if scope is None:
            return None
        _cmd_cache[file_path] = scope
    return _cmd_cache[file_path]

# ---------------------------------------------------------------------------
# Command execution
# ---------------------------------------------------------------------------

def execute_command(command, args):
    if command not in commands:
        error("'{}' is not a recognized command.  Type 'help'.".format(command))
        return

    mapping = commands[command]
    parts   = mapping.split(':', 1)
    if len(parts) != 2:
        error("Bad command mapping for '{}': '{}'".format(command, mapping))
        return

    file_path, func_name = parts[0].strip(), parts[1].strip()
    scope = _get_scope(file_path)
    if scope is None:
        return

    if isinstance(scope, dict):
        func = scope.get(func_name)
    else:
        func = getattr(scope, func_name, None)
    if func is None:
        error("Function '{}' not found in '{}'".format(func_name, file_path))
        return

    try:
        func(args)
    except Exception as e:
        error("Command '{}' raised an error: {}".format(command, e))


def execute_file(name, args):
    """Fall-through handler: try to run a .py script by name."""
    try:
        if regedit.read('Features.Program_Execution') == 'false':
            error("Program execution is disabled.")
            info("Enable it: reg set Features.Program_Execution true  (or via 'settings')")
            return
    except Exception:
        pass
    cwd = uos.getcwd().rstrip('/')
    candidates = [
        name,
        name + '.py',
        cwd + '/' + name,
        cwd + '/' + name + '.py',
    ]
    path = None
    for c in candidates:
        try:
            uos.stat(c)
            path = c
            break
        except OSError:
            pass

    if path is None:
        error("'{}' is not a command or executable file.".format(name))
        return

    try:
        with open(path, 'r') as f:
            code = f.read()
        scope = {}
        exec(code, scope)
        if 'main' in scope:
            scope['main'](args)
    except OSError as e:
        error("Cannot open '{}': {}".format(path, e))
    except Exception as e:
        error("Error running '{}': {}".format(path, e))

# ---------------------------------------------------------------------------
# Multi-command splitting  (';' separator, quote-aware)
# ---------------------------------------------------------------------------

def _tilde_expand(s, home):
    """Expand ~ tokens in an argument string.

    Rules (matches sh behaviour):
      ~/...   → home/...
      ~        (alone, or followed by space)  → home
      ~word   → left untouched  (no user-dir expansion)
    """
    if not s or '~' not in s:
        return s
    home = (home or '/').rstrip('/')
    result = []
    i = 0
    n = len(s)
    while i < n:
        if s[i] == '~':
            nxt = i + 1
            if nxt == n or s[nxt] in (' ', '\t'):
                result.append(home)   # bare ~
            elif s[nxt] == '/':
                result.append(home)   # ~/path  — the / stays as next char
            else:
                result.append('~')    # ~word — leave alone
        else:
            result.append(s[i])
        i += 1
    return ''.join(result)


def _split_cmds(raw):
    """Split a command line on ';', respecting single and double quotes."""
    cmds = []
    cur  = []
    in_q = False
    qchar = None
    for ch in raw:
        if ch in ('"', "'"):
            if not in_q:
                in_q  = True
                qchar = ch
            elif ch == qchar:
                in_q  = False
                qchar = None
            cur.append(ch)
        elif ch == ';' and not in_q:
            s = ''.join(cur).strip()
            if s:
                cmds.append(s)
            cur = []
        else:
            cur.append(ch)
    s = ''.join(cur).strip()
    if s:
        cmds.append(s)
    return cmds

# ---------------------------------------------------------------------------
# Shell input  — interactive line reader with history navigation
# ---------------------------------------------------------------------------

def _complete_path(partial):
    """Return the unique completion suffix for a partial filesystem path."""
    try:
        if '/' in partial:
            sep = partial.rfind('/')
            dir_part  = partial[:sep + 1]   # includes trailing /
            file_part = partial[sep + 1:]
            if dir_part.startswith('/'):
                sd = dir_part.rstrip('/') or '/'
            else:
                sd = uos.getcwd().rstrip('/') + '/' + dir_part.rstrip('/')
        else:
            dir_part  = ''
            file_part = partial
            sd        = uos.getcwd()
        entries = uos.listdir(sd)
        matches = [e for e in entries if e.startswith(file_part)]
        if len(matches) != 1:
            return ''
        suffix = matches[0][len(file_part):]
        fp = ('/' + matches[0]) if sd == '/' else (sd.rstrip('/') + '/' + matches[0])
        try:
            if uos.stat(fp)[0] & 0x4000:
                suffix += '/'
        except OSError:
            pass
        return suffix
    except Exception:
        return ''


def _tab_complete(buf_str):
    """Complete command name (single word) or filesystem path (after first word)."""
    if not buf_str:
        return ''
    has_space = ' ' in buf_str
    words = buf_str.split()
    if not has_space:
        # Command completion — only when no space typed yet
        partial = words[0] if words else ''
        all_names = list(commands.keys()) + list(_CRITICAL.keys()) + list(_aliases.keys())
        matches = [c for c in all_names if c.startswith(partial) and len(c) > len(partial)]
        return matches[0][len(partial):] if len(matches) == 1 else ''
    # Path completion — complete the last word as a filesystem path
    last = '' if buf_str.endswith(' ') else words[-1]
    return _complete_path(last)


_skip_lf = False   # CRLF pairing: skip a \n that follows a \r


def _shell_input(prompt):
    """
    Interactive line reader with full cursor navigation.
      - Up/Down   : history navigation
      - Left/Right: cursor movement within the line
      - Home/End  : jump to start/end (xterm and VT sequences)
      - Delete    : delete character under cursor
      - Ctrl+A/E  : beginning / end of line
      - Backspace : delete character before cursor
      - Ctrl+C    : cancel input, return empty string
      - Tab       : accept ghost-text completion (command name)

    Ghost text: when the cursor is at the end of a single-word partial
    that has exactly one completion, the suffix is shown in dim gray.
    Pressing Tab accepts it; any other key clears it first.
    """
    global _skip_lf
    sys.stdout.write(prompt)
    buf      = []
    hist_pos = len(_history)   # past the end = 'new input'
    cursor   = 0               # insert position within buf (0..len(buf))
    ghost    = ''              # completion suffix currently shown on screen

    # ── helpers ──────────────────────────────────────────────────────────────
    def _ghost_clear():
        # Erase ghost text from screen.  Ghost is always right of the cursor,
        # so \x1b[K (erase to EOL) is sufficient when cursor == len(buf).
        # When cursor != len(buf) ghost is never shown, so nothing to clear.
        if ghost:
            sys.stdout.write('\x1b[K')

    def _ghost_update():
        # Compute and display new ghost: command completion on the first word,
        # path completion on subsequent words (cursor must be at end of line).
        nonlocal ghost
        new_ghost = ''
        if cursor == len(buf):
            new_ghost = _tab_complete(''.join(buf))
        ghost = new_ghost
        if ghost:
            sys.stdout.write('\033[2m\033[90m' + ghost + '\033[0m')
            sys.stdout.write('\x1b[{}D'.format(len(ghost)))

    # ─────────────────────────────────────────────────────────────────────────
    while True:
        try:
            ch = sys.stdin.read(1)
        except Exception:
            return ''

        # CRLF handling: if the last line ended with \r, skip a paired \n
        if _skip_lf:
            _skip_lf = False
            if ch == '\n':
                continue

        # --- Tab — accept completion ---
        if ch == '\t':
            if ghost and cursor == len(buf):
                _ghost_clear()
                for c in ghost:
                    buf.append(c)
                    cursor += 1
                    sys.stdout.write(c)
                ghost = ''
                # Recompute in case there's a further unique suffix
                _ghost_update()
            # Else: no completion available — silently ignore
            continue

        # --- Confirm input ---
        if ch in ('\r', '\n'):
            if ch == '\r':
                _skip_lf = True   # consume paired \n from CRLF (Thonny etc.)
            _ghost_clear()
            # Move terminal cursor to end of line before newline
            if cursor < len(buf):
                sys.stdout.write('\x1b[{}C'.format(len(buf) - cursor))
            sys.stdout.write('\r\n')
            line = ''.join(buf)
            if line.strip():
                _history.append(line)
                if len(_history) > _HIST_MAX:
                    _history.pop(0)
            return line

        # --- Backspace ---
        elif ch in ('\x7f', '\x08'):
            _ghost_clear()
            if cursor > 0:
                del buf[cursor - 1]
                cursor -= 1
                tail = ''.join(buf[cursor:])
                sys.stdout.write('\x08\x1b[K' + tail)
                if tail:
                    sys.stdout.write('\x1b[{}D'.format(len(tail)))
            ghost = ''
            _ghost_update()

        # --- Ctrl+C ---
        elif ch == '\x03':
            _ghost_clear()
            sys.stdout.write('^C\r\n')
            return ''

        # --- Ctrl+A  (beginning of line) ---
        elif ch == '\x01':
            _ghost_clear()
            ghost = ''
            if cursor > 0:
                sys.stdout.write('\x1b[{}D'.format(cursor))
                cursor = 0

        # --- Ctrl+E  (end of line) ---
        elif ch == '\x05':
            _ghost_clear()
            ghost = ''
            if cursor < len(buf):
                sys.stdout.write('\x1b[{}C'.format(len(buf) - cursor))
                cursor = len(buf)
            _ghost_update()

        # --- Escape sequences ---
        elif ch == '\x1b':
            _ghost_clear()
            ghost = ''
            try:
                n1 = sys.stdin.read(1)
                if n1 != '[':
                    continue
                n2 = sys.stdin.read(1)

                if n2 == 'A':          # Up arrow — older history
                    if _history and hist_pos > 0:
                        hist_pos -= 1
                    new = _history[hist_pos] if 0 <= hist_pos < len(_history) else ''
                    if cursor > 0:
                        sys.stdout.write('\x1b[{}D'.format(cursor))
                    sys.stdout.write('\x1b[K' + new)
                    buf    = list(new)
                    cursor = len(buf)
                    _ghost_update()

                elif n2 == 'B':        # Down arrow — newer history
                    if hist_pos < len(_history) - 1:
                        hist_pos += 1
                        new = _history[hist_pos]
                    else:
                        hist_pos = len(_history)
                        new = ''
                    if cursor > 0:
                        sys.stdout.write('\x1b[{}D'.format(cursor))
                    sys.stdout.write('\x1b[K' + new)
                    buf    = list(new)
                    cursor = len(buf)
                    _ghost_update()

                elif n2 == 'D':        # Left arrow
                    if cursor > 0:
                        cursor -= 1
                        sys.stdout.write('\x1b[D')

                elif n2 == 'C':        # Right arrow
                    if cursor < len(buf):
                        cursor += 1
                        sys.stdout.write('\x1b[C')
                    _ghost_update()

                elif n2 == 'H':        # Home (xterm)
                    if cursor > 0:
                        sys.stdout.write('\x1b[{}D'.format(cursor))
                        cursor = 0

                elif n2 == 'F':        # End (xterm)
                    if cursor < len(buf):
                        sys.stdout.write('\x1b[{}C'.format(len(buf) - cursor))
                        cursor = len(buf)
                    _ghost_update()

                elif n2 in ('1', '3', '4', '7', '8'):   # VT extended sequences
                    try:
                        tilde = sys.stdin.read(1)
                    except Exception:
                        tilde = ''
                    if tilde == '~':
                        if n2 in ('1', '7'):        # Home
                            if cursor > 0:
                                sys.stdout.write('\x1b[{}D'.format(cursor))
                                cursor = 0
                        elif n2 in ('4', '8'):      # End
                            if cursor < len(buf):
                                sys.stdout.write('\x1b[{}C'.format(len(buf) - cursor))
                                cursor = len(buf)
                            _ghost_update()
                        elif n2 == '3':             # Delete forward
                            if cursor < len(buf):
                                del buf[cursor]
                                tail = ''.join(buf[cursor:])
                                sys.stdout.write('\x1b[K' + tail)
                                if tail:
                                    sys.stdout.write('\x1b[{}D'.format(len(tail)))
                                _ghost_update()
            except Exception:
                pass

        # --- Printable character (inserted at cursor) ---
        elif ord(ch) >= 32:
            _ghost_clear()
            ghost = ''
            buf.insert(cursor, ch)
            cursor += 1
            if cursor == len(buf):
                sys.stdout.write(ch)
            else:
                tail = ''.join(buf[cursor:])
                sys.stdout.write(ch + tail)
                sys.stdout.write('\x1b[{}D'.format(len(tail)))
            _ghost_update()

# ---------------------------------------------------------------------------
# Prompt formatting
# ---------------------------------------------------------------------------

_RST  = '\033[0m'
_CYAN = '\033[96m'
_GRAY = '\033[90m'
_BLUE = '\033[94m'


def _prompt(username):
    cwd  = uos.getcwd()
    home = _shell_state.get('home', '').rstrip('/')
    if home and (cwd == home or cwd.startswith(home + '/')):
        display = '~' + cwd[len(home):]
    else:
        display = cwd
    return "{}{}{}{}{}{} ".format(
        _CYAN,  username,
        _GRAY + '@nebula' + _RST + ':',
        _BLUE,  display,
        _RST + _CYAN + '>' + _RST,
    )

# ---------------------------------------------------------------------------
# Shell main loop
# ---------------------------------------------------------------------------

def launchpad_init(username, password):
    """Start the authenticated Launchpad shell for the given user."""
    if not decode(username, password, silent=True):
        warn("Authentication failed for '{}'. Cannot start shell.".format(username))
        return

    load_commands()
    init_session_log()

    # Set home directory and start there
    home = '/Users/{}'.format(username)
    _shell_state['home'] = home
    try:
        uos.chdir(home)
    except OSError:
        uos.chdir('/')

    ok("Welcome back, {}!  Shell ready.".format(username), p="Launchpad")
    info("Type 'help' for a command list.  Up/Down arrows navigate history.", p="Launchpad")

    _shell_state['running'] = True

    while _shell_state['running']:
        try:
            raw = _shell_input(_prompt(username))
            raw = raw.strip()
            # Skip empty lines (common when using Thonny or other serial emulators)
            if not raw:
                continue

            for sub_raw in _split_cmds(raw):
                if not _shell_state['running']:
                    break
                _parts  = sub_raw.split(None, 1)
                command = _parts[0]
                args    = _parts[1] if len(_parts) > 1 else None
                # Alias expansion — re-parse if the command matches an alias
                if command in _aliases:
                    expanded = _aliases[command]
                    if args:
                        expanded = expanded + ' ' + args
                    _eparts = expanded.split(None, 1)
                    command = _eparts[0]
                    args    = _eparts[1] if len(_eparts) > 1 else None
                # Tilde expansion in args
                if args and '~' in args:
                    args = _tilde_expand(args, _shell_state.get('home', '/'))
                # --help / -h flag: redirect to 'help <command>'
                if args in ('--help', '-h') and command not in ('help',):
                    execute_command('help', command)
                    continue
                try:
                    if command in _CRITICAL:
                        _CRITICAL[command](args)
                    elif command in commands:
                        execute_command(command, args)
                    else:
                        execute_file(command, args)
                except MemoryError:
                    import gc as _gc
                    _cmd_cache.clear()
                    _gc.collect()
                    # Heap-consolidation nudge: allocating then freeing a large
                    # block forces MicroPython to compact small free regions into
                    # one contiguous span before the retry.
                    try:
                        _nudge = bytearray(4096)
                        del _nudge
                        _gc.collect()
                    except MemoryError:
                        pass
                    warn("Heap fragmented — cache cleared ({} KB free). Retrying...".format(
                        _gc.mem_free() // 1024))
                    try:
                        if command in _CRITICAL:
                            _CRITICAL[command](args)
                        elif command in commands:
                            execute_command(command, args)
                        else:
                            execute_file(command, args)
                    except MemoryError:
                        error("Allocation failed ({} KB free) — heap is fragmented.".format(
                            _gc.mem_free() // 1024))
                        info("Run 'freeup' to consolidate heap, or 'reboot'.")
                    except Exception as e2:
                        error("Command error after cleanup: {}".format(e2))

        except KeyboardInterrupt:
            warn("Use 'exit' or 'logout' to leave the shell.")
        except MemoryError:
            import gc as _gc
            _cmd_cache.clear()
            _gc.collect()
            try:
                _nudge = bytearray(4096)
                del _nudge
                _gc.collect()
            except MemoryError:
                pass
            try:
                warn("Heap fragmented — auto-cleanup ran ({} KB free).".format(
                    _gc.mem_free() // 1024))
                info("Run 'freeup' or 'reboot' if issues persist.")
            except Exception:
                pass
        except Exception as e:
            error("Unexpected shell error: {}".format(e))

        # Low RAM check — warn after each command cycle, but only once per threshold
        # crossing so it doesn't spam on every prompt.
        try:
            import gc as _gc
            _free = _gc.mem_free()
            if _free < 71680:   # 70 KB threshold
                warn("Low memory: {} KB free. Run 'freeup' to reclaim RAM.".format(
                    _free // 1024))
        except Exception:
            pass

    # Shell exited cleanly (logout / exit command fired)
    ok("Goodbye, {}.".format(username), p="Launchpad")
    close_session_log()


def recovery_init(errStr):
    """Start the unauthenticated recovery shell."""
    load_commands()

    warn("=== RECOVERY MODE ===", p="Recovery")
    if errStr:
        warn("Reason: {}".format(errStr), p="Recovery")
    if errStr == "Missing critical system files.":
        warn("Re-imaging RPCortex is strongly recommended.", p="Recovery")
    info("A limited shell is now available.  Type 'help' for commands.", p="Recovery")

    _shell_state['running'] = True

    while _shell_state['running']:
        try:
            raw = _shell_input(_prompt("recovery"))
            raw = raw.strip()
            if not raw:
                continue
            for sub_raw in _split_cmds(raw):
                if not _shell_state['running']:
                    break
                _parts  = sub_raw.split(None, 1)
                command = _parts[0]
                args    = _parts[1] if len(_parts) > 1 else None
                if command in _aliases:
                    expanded = _aliases[command]
                    if args:
                        expanded = expanded + ' ' + args
                    _eparts = expanded.split(None, 1)
                    command = _eparts[0]
                    args    = _eparts[1] if len(_eparts) > 1 else None
                if args and '~' in args:
                    args = _tilde_expand(args, _shell_state.get('home', '/'))
                # --help / -h flag: redirect to 'help <command>'
                if args in ('--help', '-h') and command not in ('help',):
                    execute_command('help', command)
                    continue
                if command in _CRITICAL:
                    _CRITICAL[command](args)
                elif command in commands:
                    execute_command(command, args)
                else:
                    execute_file(command, args)
        except KeyboardInterrupt:
            warn("Use 'reboot' to restart the system.")
        except MemoryError:
            import gc as _gc
            _cmd_cache.clear()
            _gc.collect()
            try:
                _nudge = bytearray(4096)
                del _nudge
                _gc.collect()
            except MemoryError:
                pass
            try:
                warn("Heap fragmented in recovery — auto-cleanup ran ({} KB free).".format(
                    _gc.mem_free() // 1024))
            except Exception:
                pass
        except Exception as e:
            error("Recovery shell error: {}".format(e))
