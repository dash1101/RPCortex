# Desc: Core output utilities and session logging for RPCortex - Nebula OS
# File: /Core/RPCortex.py
# Last Updated: 3/24/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta3
# Author: dash1101

import os
import sys
import time

post_check = True

# ---------------------------------------------------------------------------
# ANSI color constants
# ---------------------------------------------------------------------------

HEADER    = '\033[95m'
OKBLUE    = '\033[94m'
OKCYAN    = '\033[96m'
WARNING   = '\033[93m'
GRAY      = '\033[90m'
GREEN     = '\033[32m'
WHITE     = '\033[0m'
FAIL      = '\033[91m'
BOLD      = '\033[1m'
UNDERLINE = '\033[4m'
WHITE_AT  = '\033[97m'

# ---------------------------------------------------------------------------
# Session log
# ---------------------------------------------------------------------------

LOG_DIR    = '/Nebula/Logs'
LATEST_LOG = LOG_DIR + '/latest.log'
MAX_LOGS   = 10

_log_file = None   # open file handle during a session; None otherwise


def init_session_log():
    """Open a new session log file. Call once after successful login."""
    global _log_file
    try:
        try:
            os.mkdir(LOG_DIR)
        except OSError:
            pass   # already exists
        rename_logs()
        _log_file = open(LATEST_LOG, 'w')
        t = time.localtime()
        _log_file.write(
            "=== RPCortex Nebula - Session Log ===\n"
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
    """Internal: append one line to the open session log."""
    if not _log_file:
        return
    try:
        t = time.localtime()
        _log_file.write(
            "{}-{:02d}-{:02d} {:02d}:{:02d}:{:02d} [{:<5}] {}\n".format(
                t[0], t[1], t[2], t[3], t[4], t[5], level, msg
            )
        )
        _log_file.flush()
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
    print(out, end='')


def error(msg, nL=True, p=None):
    if post_check:
        _fmt(FAIL, '!', msg, p, nL)
        _log_write('ERROR', ('[{}] '.format(p) if p else '') + str(msg))


def fatal(msg, nL=True, p=None):
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
    if post_check:
        out = msg + ('\n' if nL else '')
        print(out, end='')
        _log_write('PRINT', str(msg))


def inpt(msg):
    if post_check:
        return input("{}{} {}••>  {}".format(WHITE, msg, OKCYAN, WHITE))


def masked_inpt(msg):
    """Like inpt() but echoes a bullet (•) for each character.
    Falls back to regular inpt() on platforms where raw stdin isn't available."""
    if not post_check:
        return ''
    prompt_str = "{}{} {}••>  {}".format(WHITE, msg, OKCYAN, WHITE)
    sys.stdout.write(prompt_str)
    buf = []
    try:
        while True:
            ch = sys.stdin.read(1)
            if ch in ('\r', '\n'):
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
