# Desc: Core output utilities and session logging for RPCortex - Pulsar OS
# File: /Core/RPCortex.py
# Last Updated: 6/10/2026
# Lang: MicroPython, English
# Version: v0.9.1
# Author: dash1101

import os
import sys
import time

# Single source of truth for the running code's version and codename.
# initialization.start() syncs Settings.Version and System.Codename in the
# registry to these values on every boot, so the registry can never drift
# after an OS update.
OS_VERSION  = "v0.9.1"
OS_CODENAME = "RPCortex B9 - Pulsar"

# OS_BUILD is a date/time build id stamped by build.py into a generated
# Core/buildinfo.py at release-build time. A from-source/dev tree has no
# buildinfo, so it reports "source"/"dev". The build id lets the updater tell
# two builds of the SAME version apart (re-publishing v0.9.1 bumps the build).
# OS_STAGE is the release channel (Stable/Beta/Alpha/RC/Release) from build.cfg.
try:
    from buildinfo import BUILD as OS_BUILD
except Exception:
    OS_BUILD = "source"
try:
    from buildinfo import STAGE as OS_STAGE
except Exception:
    OS_STAGE = "dev"

post_check = True

# ---------------------------------------------------------------------------
# Output capture + command exit-status tracking  (pipes, && / ||, scripting)
#
# multi() is the data channel (stdout-like): when a capture buffer is active it
# is collected instead of printed, so the shell can feed it to the next stage
# of a pipeline.  The status helpers (ok/info/warn/error/fatal) always print —
# they are stderr-like and never become piped data.
#
# error()/fatal() additionally set _had_error.  The shell clears it before each
# command and reads it after, deriving a pass/fail exit status for && / || and
# script conditionals WITHOUT every command needing to return one.
# ---------------------------------------------------------------------------

_capture   = None     # list buffer while capturing multi() output, else None
_had_error = False    # set by error()/fatal(); cleared per command by the shell


def begin_capture():
    """Start buffering multi() output. Returns the previous buffer (nesting-safe)."""
    global _capture
    prev = _capture
    _capture = []
    return prev


def end_capture(prev=None):
    """Stop buffering; return captured text and restore the previous buffer."""
    global _capture
    text = ''.join(_capture) if _capture is not None else ''
    _capture = prev
    return text


def is_capturing():
    """True while multi() output is being captured (i.e. piped onward)."""
    return _capture is not None


def clear_error():
    """Reset the per-command error flag (call before dispatching a command)."""
    global _had_error
    _had_error = False


def had_error():
    """True if error()/fatal() was called since the last clear_error()."""
    return _had_error

# ---------------------------------------------------------------------------
# ANSI color constants
# ---------------------------------------------------------------------------

HEADER    = '\033[95m'
OKBLUE    = '\033[94m'
OKCYAN    = '\033[96m'
WARNING   = '\033[93m'
GRAY      = '\033[90m'
GREEN     = '\033[32m'
WHITE     = '\033[0m'   # NB: this is ANSI reset/default, not white — used to reset color.
FAIL      = '\033[91m'  #     Bright white is WHITE_AT ('\033[97m').
BOLD      = '\033[1m'
UNDERLINE = '\033[4m'
WHITE_AT  = '\033[97m'

# ---------------------------------------------------------------------------
# Session log
# ---------------------------------------------------------------------------

LOG_DIR    = '/Pulsar/Logs'
LATEST_LOG = LOG_DIR + '/latest.log'
MAX_LOGS   = 10

_log_file    = None   # open file handle during a session; None otherwise
_log_pending = 0      # lines written since last flush (batched to cut flash latency)


def init_session_log():
    """Open a new session log file. Call once after successful login."""
    global _log_file, _log_pending
    _log_pending = 0
    try:
        try:
            os.mkdir(LOG_DIR)
        except OSError:
            pass   # already exists
        rename_logs()
        _log_file = open(LATEST_LOG, 'w')
        t = time.localtime()
        _log_file.write(
            "=== RPCortex Pulsar - Session Log ===\n"
            "Started : {}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}\n"
            "=====================================\n\n".format(
                t[0], t[1], t[2], t[3], t[4], t[5]
            )
        )
        _log_file.flush()
    except Exception:
        _log_file = None   # logging unavailable; non-fatal


def close_session_log():
    """Flush and close the session log. Call on logout or shutdown."""
    global _log_file
    if _log_file:
        try:
            t = time.localtime()
            _log_file.write(
                "\n=== Session ended {}-{:02d}-{:02d} {:02d}:{:02d}:{:02d} ===\n".format(
                    t[0], t[1], t[2], t[3], t[4], t[5]
                )
            )
            _log_file.flush()
            _log_file.close()
        except Exception:
            pass
        _log_file = None


def _log_write(level, msg):
    """Internal: append one line to the open session log.

    Flushes are batched — flushing flash on every line caused visible lag
    between rapid output calls. Errors and warnings flush immediately so
    the crash log stays useful; routine lines flush every 8 writes.
    """
    global _log_pending
    if not _log_file:
        return
    try:
        t = time.localtime()
        _log_file.write(
            "{}-{:02d}-{:02d} {:02d}:{:02d}:{:02d} [{:<5}] {}\n".format(
                t[0], t[1], t[2], t[3], t[4], t[5], level, msg
            )
        )
        _log_pending += 1
        if _log_pending >= 8 or level in ('ERROR', 'FATAL', 'WARN'):
            _log_file.flush()
            _log_pending = 0
    except Exception:
        pass


def rename_logs():
    """Rotate logs: latest.log -> log_1, log_1 -> log_2, ..., up to MAX_LOGS."""
    for i in range(MAX_LOGS - 1, 0, -1):
        src = LOG_DIR + '/log_{}.log'.format(i)
        dst = LOG_DIR + '/log_{}.log'.format(i + 1)
        try:
            os.rename(src, dst)
        except OSError:
            pass
    try:
        os.rename(LATEST_LOG, LOG_DIR + '/log_1.log')
    except OSError:
        pass


def log(msg):
    """Write a raw message directly to the session log."""
    _log_write('LOG', msg)


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def file_exists(filepath):
    try:
        with open(filepath, 'r'):
            return True
    except OSError:
        return False


def str_to_bool(value):
    v = value.lower()
    if v == "true":
        return True
    if v == "false":
        return False
    raise ValueError("Cannot convert '{}' to bool".format(value))


# ---------------------------------------------------------------------------
# Output functions
# All print to the terminal AND write to the session log if one is active.
# ---------------------------------------------------------------------------

def _fmt(color, symbol, msg, p, nL):
    """Build and print a tagged output line."""
    out = "{}[{}{}{}]".format(color, WHITE_AT, symbol, color)
    if p is not None:
        out += " {}[{}{}{}]".format(color, WHITE_AT, p, color)
    out += " {}{}".format(WHITE, msg)
    if nL:
        out += '\n'
    sys.stdout.write(out)   # faster than print() on MicroPython (no arg/sep/end work)


def error(msg, nL=True, p=None):
    global _had_error
    _had_error = True   # failure signal for && / || and script conditionals
    if post_check:
        _fmt(FAIL, '!', msg, p, nL)
        _log_write('ERROR', ('[{}] '.format(p) if p else '') + str(msg))


def fatal(msg, nL=True, p=None):
    global _had_error
    _had_error = True
    if post_check:
        _fmt(FAIL, '!!!', msg, p, nL)
        _log_write('FATAL', ('[{}] '.format(p) if p else '') + str(msg))


def info(msg, nL=True, p=None):
    if post_check:
        _fmt(HEADER, ':', msg, p, nL)
        _log_write('INFO', ('[{}] '.format(p) if p else '') + str(msg))


def warn(msg, nL=True, p=None):
    if post_check:
        _fmt(WARNING, '?', msg, p, nL)
        _log_write('WARN', ('[{}] '.format(p) if p else '') + str(msg))


def ok(msg, nL=True, p=None):
    if post_check:
        _fmt(OKCYAN, '@', msg, p, nL)
        _log_write('OK', ('[{}] '.format(p) if p else '') + str(msg))


def multi(msg, nL=True, p=None):
    # The high-volume display/data channel (cat/ls/grep/TUI output). Uses
    # sys.stdout.write (faster than print) and is NOT logged per line — logging
    # every display line to flash was the main drag on text-heavy output. The
    # diagnostic log still captures events (info/ok/warn/error/fatal).
    if post_check:
        out = msg + ('\n' if nL else '')
        if _capture is not None:
            _capture.append(out)   # piped onward instead of printed
        else:
            sys.stdout.write(out)


# ---------------------------------------------------------------------------
# In-place spinner — for any operation that makes the user wait (WiFi connect,
# downloads, scans). Renders "<label> \ (3s)" on one line, updating in place.
# ---------------------------------------------------------------------------
_SPIN_FRAMES = '-\\|/'

def spin(label, i, start_ms):
    """Render the spinner once: '<label> <frame> (<elapsed>s)'.
    Call repeatedly with an incrementing i and the start tick from utime.ticks_ms()."""
    try:
        import utime
        secs = utime.ticks_diff(utime.ticks_ms(), start_ms) // 1000
    except Exception:
        secs = 0
    ch = _SPIN_FRAMES[i % len(_SPIN_FRAMES)]
    sys.stdout.write('\r\x1b[K{} {} ({}s)'.format(label, ch, secs))

def spin_done(msg=None):
    """Clear the spinner line; print a final message on its own line if given."""
    sys.stdout.write('\r\x1b[K')
    if msg is not None:
        sys.stdout.write(msg + '\n')


def inpt(msg):
    # Always return a string — callers do inpt(...).strip(). Returning None when
    # post_check is off (as an earlier version did) was a latent AttributeError.
    if not post_check:
        return ''
    return input("{}{} {}••>  {}".format(WHITE, msg, OKCYAN, WHITE))


def masked_inpt(msg):
    """Like inpt() but echoes a bullet (•) for each character.
    Falls back to regular inpt() on platforms where raw stdin isn't available."""
    if not post_check:
        return ''
    prompt_str = "{}{} {}••>  {}".format(WHITE, msg, OKCYAN, WHITE)
    sys.stdout.write(prompt_str)
    buf = []
    skip_lf = False
    try:
        while True:
            ch = sys.stdin.read(1)
            if skip_lf:
                skip_lf = False
                if ch == '\n':
                    continue
            if ch in ('\r', '\n'):
                if ch == '\r':
                    skip_lf = True
                sys.stdout.write('\r\n')
                return ''.join(buf)
            elif ch in ('\x7f', '\x08'):   # backspace / DEL
                if buf:
                    buf.pop()
                    sys.stdout.write('\x08 \x08')
            elif ch == '\x03':             # Ctrl+C — treat as empty input
                sys.stdout.write('^C\r\n')
                return ''
            elif ord(ch) >= 32:
                buf.append(ch)
                sys.stdout.write('\u2022')  # bullet point
    except Exception:
        # stdin read failed (e.g. non-interactive context) — fall back
        sys.stdout.write('\r\n')
        return ''.join(buf)
