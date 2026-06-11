# Desc: CLI settings panel for RPCortex - Pulsar OS
# File: /Core/Launchpad/settings.py
# Last Updated: 6/11/2026
# Lang: MicroPython, English
# Version: v0.9.1
# Author: dash1101
#
# SysMon-styled settings manager. Toggles boolean registry keys and edits
# text/numeric personalization keys (owner, timezone, device ID, idle-logout)
# without touching the CLI.
#
# Efficient redraw: the panel is drawn once, then a toggle rewrites ONLY its own
# row in place (relative cursor moves) instead of clearing and repainting the
# whole screen on every keystroke. Edits and refresh do a full redraw; unknown
# keys redraw nothing.

import sys
import machine

if '/Core' not in sys.path:
    sys.path.append('/Core')

import regedit
from RPCortex import ok, warn, info, multi, inpt

# ---------------------------------------------------------------------------
# ANSI styling (mirrors SysMon: borderless, ═ rules, cyan section heads)
# ---------------------------------------------------------------------------

_CY = '\x1b[96m'   # cyan   — section heads
_GR = '\x1b[92m'   # green  — ON
_YL = '\x1b[93m'   # yellow — values
_DG = '\x1b[90m'   # gray   — rules / OFF / notes
_WH = '\x1b[97m'   # white  — keys / title
_BD = '\x1b[1m'    # bold
_R  = '\x1b[0m'    # reset

_W      = 78           # display width
_PROMPT = 'Choice: '   # input prompt (no trailing newline)

# Module state for in-place updates, set by _full_draw().
_idx    = {}   # setting-key -> line index within the drawn panel
_nlines = 0    # number of content lines drawn (prompt sits on line _nlines)

# ---------------------------------------------------------------------------
# Registry helpers
# ---------------------------------------------------------------------------

def _rget(key, default='false'):
    return regedit.read(key) or default


def _rtoggle(key):
    new_val = 'false' if _rget(key, 'false') == 'true' else 'true'
    regedit.save(key, new_val)
    return new_val

# ---------------------------------------------------------------------------
# Hardware widgets (title bar)
# ---------------------------------------------------------------------------

def _get_temp():
    try:
        from machine import ADC
        v = ADC(4).read_u16() * 3.3 / 65535
        return '{:.1f}C'.format(27.0 - (v - 0.706) / 0.001721)
    except Exception:
        return '?'


def _get_freq():
    try:
        return '{} MHz'.format(machine.freq() // 1_000_000)
    except Exception:
        return '?'


def _get_mem():
    try:
        import gc as _gc
        return '{} KB'.format(_gc.mem_free() // 1024)
    except Exception:
        return '?'

# ---------------------------------------------------------------------------
# Line builders
# ---------------------------------------------------------------------------

def _div():
    return _DG + '═' * _W + _R


def _sec(title):
    prefix = '══ {} '.format(title)
    rest   = '═' * max(0, _W - len(prefix))
    return _CY + prefix + _DG + rest + _R


def _title_line():
    left  = '  RPCortex Settings'
    right = '{}  {}  {}'.format(_get_freq(), _get_temp(), _get_mem())
    pad   = max(1, _W - len(left) - len(right))
    return ('  ' + _WH + _BD + 'RPCortex Settings' + _R +
            ' ' * pad + _DG + right + _R)


def _toggle_row(num, label, val, note=''):
    status = (_GR + _BD + 'ON ' + _R) if val == 'true' else (_DG + 'OFF' + _R)
    ntxt   = ('   ' + _DG + note + _R) if note else ''
    return ('  ' + _WH + '[' + num + ']' + _R + ' ' +
            '{:<22}'.format(label) + ' : ' + status + ntxt)


def _value_row(key, label, val, note=''):
    shown = val if val else '(unset)'
    vcol  = (_YL + shown + _R) if val else (_DG + shown + _R)
    ntxt  = ('   ' + _DG + note + _R) if note else ''
    return ('  ' + _WH + '[' + key + ']' + _R + ' ' +
            '{:<22}'.format(label) + ' : ' + vcol + ntxt)


def _footer():
    return ('  ' + _DG + '[1-6]' + _R + ' toggle   ' +
            _DG + '[o/t/d/i]' + _R + ' edit   ' +
            _DG + '[r]' + _R + ' refresh   ' +
            _DG + '[q]' + _R + ' quit')


def _idle_note():
    return 'minutes (0 = off)' if _rget('Settings.Idle_Logout', '0') == '0' else 'minutes'


def _row_for(key):
    """Rebuild a single setting row from the current registry value."""
    if key == '1':
        return _toggle_row('1', 'Verbose Boot',      _rget('Settings.Verbose_Boot', 'false'))
    if key == '2':
        return _toggle_row('2', 'Program Execution', _rget('Features.Program_Execution', 'true'))
    if key == '3':
        oc   = _rget('Settings.OC_On_Boot', 'false')
        bc   = _rget('Hardware.Boot_Clock', '')
        freq = bc if bc else _rget('Hardware.Max_Clock', '')
        return _toggle_row('3', 'Boot Overclock', oc, freq if oc == 'true' else '')
    if key == '4':
        return _toggle_row('4', 'Beeper', _rget('Features.beeper', 'false'))
    if key == '5':
        sd = _rget('Features.SD_Support', 'false')
        return _toggle_row('5', 'SD Card Support', sd, 'not yet implemented' if sd == 'true' else '')
    if key == '6':
        return _toggle_row('6', 'WiFi Autoconnect', _rget('Settings.Network_Autoconnect', 'false'))
    if key == 'o':
        return _value_row('o', 'Owner',           _rget('System.Owner', ''))
    if key == 't':
        return _value_row('t', 'Timezone Offset',  _rget('System.TZ_Offset', '0'), 'hrs from UTC')
    if key == 'd':
        return _value_row('d', 'Device ID',        _rget('System.Device_ID', 'pulsar'), 'hostname')
    if key == 'i':
        return _value_row('i', 'Idle Logout',      _rget('Settings.Idle_Logout', '0'), _idle_note())
    return ''

# ---------------------------------------------------------------------------
# Drawing
# ---------------------------------------------------------------------------

def _build_lines():
    """Return (lines, idx). idx maps a setting key to its content line index."""
    lines = []
    idx   = {}

    lines.append(_title_line())
    lines.append(_div())
    lines.append('')
    lines.append(_sec('SYSTEM'))
    idx['1'] = len(lines); lines.append(_row_for('1'))
    idx['2'] = len(lines); lines.append(_row_for('2'))
    lines.append('')
    lines.append(_sec('HARDWARE'))
    idx['3'] = len(lines); lines.append(_row_for('3'))
    idx['4'] = len(lines); lines.append(_row_for('4'))
    idx['5'] = len(lines); lines.append(_row_for('5'))
    lines.append('')
    lines.append(_sec('NETWORK'))
    idx['6'] = len(lines); lines.append(_row_for('6'))
    lines.append('')
    lines.append(_sec('PERSONALIZATION'))
    idx['o'] = len(lines); lines.append(_row_for('o'))
    idx['t'] = len(lines); lines.append(_row_for('t'))
    idx['d'] = len(lines); lines.append(_row_for('d'))
    idx['i'] = len(lines); lines.append(_row_for('i'))
    lines.append('')
    lines.append(_div())
    lines.append(_footer())
    return lines, idx


def _full_draw():
    """Clear and paint the whole panel; leave the cursor at the prompt."""
    global _idx, _nlines
    lines, _idx = _build_lines()
    _nlines = len(lines)
    out = ['\x1b[2J\x1b[H\x1b[?25h']
    for ln in lines:
        out.append(ln)
        out.append('\r\n')
    out.append(_PROMPT)
    sys.stdout.write(''.join(out))


def _update(key):
    """Rewrite just one setting's row in place, then return to the prompt."""
    i = _idx.get(key)
    if i is None:
        return
    up = _nlines - i
    # Up to the row, col 1, rewrite + clear to EOL, back down to the prompt.
    sys.stdout.write('\x1b[{}A\r'.format(up))
    sys.stdout.write(_row_for(key) + '\x1b[K')
    sys.stdout.write('\x1b[{}B\r'.format(up))
    sys.stdout.write(_PROMPT)

# ---------------------------------------------------------------------------
# Value editing (full-screen prompt)
# ---------------------------------------------------------------------------

def _redit(key, label, numeric=False):
    """Full-screen prompt to edit a text/numeric registry value."""
    sys.stdout.write('\x1b[2J\x1b[H')
    cur = _rget(key, '')
    info('Edit {}'.format(label))
    multi('  Current value: ' + (cur if cur else '(unset)'))
    multi('  Enter a new value, or leave blank to keep it.')
    val = inpt('New {}'.format(label)).strip()
    if val == '':
        return
    if numeric:
        probe = val[1:] if val[:1] in ('-', '+') else val
        if not probe.isdigit():
            warn('{} must be a whole number.'.format(label))
            sys.stdin.read(1)
            return
    regedit.save(key, val)

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def settings(args=None):
    """Interactive CLI settings panel."""
    _full_draw()
    while True:
        try:
            ch = sys.stdin.read(1)
        except Exception:
            break

        if ch in ('q', 'Q', '\x03', '\r', '\n'):
            sys.stdout.write('\x1b[2J\x1b[H')
            ok("Settings saved.")
            return

        if   ch == '1': _rtoggle('Settings.Verbose_Boot');        _update('1')
        elif ch == '2': _rtoggle('Features.Program_Execution');   _update('2')
        elif ch == '3': _rtoggle('Settings.OC_On_Boot');          _update('3')
        elif ch == '4': _rtoggle('Features.beeper');              _update('4')
        elif ch == '5': _rtoggle('Features.SD_Support');          _update('5')
        elif ch == '6': _rtoggle('Settings.Network_Autoconnect'); _update('6')
        elif ch in ('o', 'O'): _redit('System.Owner',         'Owner');                       _full_draw()
        elif ch in ('t', 'T'): _redit('System.TZ_Offset',     'Timezone Offset', numeric=True); _full_draw()
        elif ch in ('d', 'D'): _redit('System.Device_ID',     'Device ID');                    _full_draw()
        elif ch in ('i', 'I'): _redit('Settings.Idle_Logout', 'Idle Logout', numeric=True);    _full_draw()
        elif ch in ('r', 'R'): _full_draw()
        # any other key: ignore, no redraw
