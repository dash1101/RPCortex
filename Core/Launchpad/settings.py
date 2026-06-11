# Desc: CLI settings panel for RPCortex - Pulsar OS
# File: /Core/Launchpad/settings.py
# Last Updated: 6/11/2026
# Lang: MicroPython, English
# Version: v0.9.1
# Author: dash1101
#
# TUI settings manager with box border, section grouping, and system widgets.
# Toggles boolean registry keys and edits text/numeric personalization keys
# (owner, timezone, device ID, idle-logout) without touching the CLI.
# Loaded by the shell via the settings command in system.lp.

import sys
import machine

if '/Core' not in sys.path:
    sys.path.append('/Core')

import regedit
from RPCortex import ok, warn, info, multi, inpt

# ---------------------------------------------------------------------------
# Box drawing and ANSI constants
# ---------------------------------------------------------------------------

_H  = '─'   # ─
_V  = '│'   # │
_TL = '┌'   # ┌
_TR = '┐'   # ┐
_BL = '└'   # └
_BR = '┘'   # ┘
_ML = '├'   # ├
_MR = '┤'   # ┤

_CYN = '\x1b[96m'
_GRN = '\x1b[92m'
_GRY = '\x1b[90m'
_YLW = '\x1b[93m'
_BLD = '\x1b[1m'
_RST = '\x1b[0m'
_WHT = '\x1b[97m'

W = 62   # total panel width (characters)
I = W - 2  # inner content width (60)

# ---------------------------------------------------------------------------
# Box helpers
# ---------------------------------------------------------------------------

def _row(vis, ansi=None):
    """Return a │-bordered line. vis = plain text for width calc."""
    if ansi is None:
        ansi = vis
    pad = max(0, I - len(vis))
    return _V + ansi + ' ' * pad + _V + '\r\n'


def _divider():
    return _ML + _H * I + _MR + '\r\n'


def _blank():
    return _row(' ')


def _section_label(title):
    """Dim section header row — uppercase, no border embellishment."""
    vis  = '  ' + title
    ansi = '  ' + _GRY + _BLD + title + _RST
    return _row(vis, ansi)


def _draw_setting(num, label, val, note=''):
    """Render one boolean toggle row inside the panel."""
    prefix_vis  = '  [{}] {:<22} : '.format(num, label)
    status_vis  = 'ON ' if val == 'true' else 'OFF'
    note_vis    = '  ' + note if note else ''
    full_vis    = prefix_vis + status_vis + note_vis

    status_ansi = (_GRN + _BLD + 'ON ' + _RST) if val == 'true' else (_GRY + 'OFF' + _RST)
    note_ansi   = ('  ' + _GRY + note + _RST) if note else ''
    full_ansi   = ('  ' + _WHT + '[{}]'.format(num) + _RST +
                   ' {:<22} : '.format(label) + status_ansi + note_ansi)

    sys.stdout.write(_row(full_vis, full_ansi))


def _draw_value(key_char, label, val, note=''):
    """Render one editable text/numeric row inside the panel."""
    shown      = val if val else '(unset)'
    prefix_vis = '  [{}] {:<22} : '.format(key_char, label)
    note_vis   = '  ' + note if note else ''
    full_vis   = prefix_vis + shown + note_vis

    val_ansi   = (_YLW + shown + _RST) if val else (_GRY + shown + _RST)
    note_ansi  = ('  ' + _GRY + note + _RST) if note else ''
    full_ansi  = ('  ' + _WHT + '[{}]'.format(key_char) + _RST +
                  ' {:<22} : '.format(label) + val_ansi + note_ansi)

    sys.stdout.write(_row(full_vis, full_ansi))

# ---------------------------------------------------------------------------
# Hardware helpers
# ---------------------------------------------------------------------------

def _get_temp():
    try:
        from machine import ADC
        sensor  = ADC(4)
        raw     = sensor.read_u16()
        voltage = raw * 3.3 / 65535
        temp_c  = 27.0 - (voltage - 0.706) / 0.001721
        return '{:.1f}C'.format(temp_c)
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


def _rget(key, default='false'):
    return regedit.read(key) or default


def _rtoggle(key):
    current = _rget(key, 'false')
    new_val = 'false' if current == 'true' else 'true'
    regedit.save(key, new_val)
    return new_val


def _redit(key, label, numeric=False, allow_blank=True):
    """Full-screen prompt to edit a text/numeric registry value."""
    sys.stdout.write('\x1b[2J\x1b[H')
    cur = _rget(key, '')
    info('Edit {}'.format(label))
    multi('  Current value: ' + (cur if cur else '(unset)'))
    if allow_blank:
        multi('  Enter a new value, or leave blank to keep / clear.')
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
    ok('{} set to: {}'.format(label, val))

# ---------------------------------------------------------------------------
# Panel renderer
# ---------------------------------------------------------------------------

def _draw():
    sys.stdout.write('\x1b[2J\x1b[H')   # clear + cursor home

    freq  = _get_freq()
    temp  = _get_temp()
    mem   = _get_mem()

    # --- Title bar ---
    title_text = '  RPCortex Settings'
    right_text = '{}  {}  {}  '.format(freq, temp, mem)
    gap        = max(0, I - len(title_text) - len(right_text))
    title_vis  = title_text + ' ' * gap + right_text
    title_ansi = (_CYN + _BLD + title_text + _RST +
                  ' ' * gap +
                  _GRY + right_text + _RST)

    sys.stdout.write(_TL + _H * I + _TR + '\r\n')
    sys.stdout.write(_row(title_vis, title_ansi))
    sys.stdout.write(_divider())
    sys.stdout.write(_blank())

    # --- Read settings ---
    verbose = _rget('Settings.Verbose_Boot',        'false')
    prog_ex = _rget('Features.Program_Execution',   'true')
    oc_on     = _rget('Settings.OC_On_Boot',          'false')
    _boot_clk = _rget('Hardware.Boot_Clock',          '')
    oc_freq   = _boot_clk if _boot_clk else _rget('Hardware.Max_Clock', '')
    beeper  = _rget('Features.beeper',              'false')
    sd_sup  = _rget('Features.SD_Support',          'false')
    wifi_ac = _rget('Settings.Network_Autoconnect', 'false')

    owner   = _rget('System.Owner',     '')
    tz      = _rget('System.TZ_Offset', '0')
    devid   = _rget('System.Device_ID', 'pulsar')
    idle    = _rget('Settings.Idle_Logout', '0')

    # --- SYSTEM ---
    sys.stdout.write(_section_label('SYSTEM'))
    _draw_setting('1', 'Verbose Boot',     verbose)
    _draw_setting('2', 'Program Execution',prog_ex)
    sys.stdout.write(_blank())

    # --- HARDWARE ---
    sys.stdout.write(_section_label('HARDWARE'))
    _draw_setting('3', 'Boot Overclock',   oc_on,  oc_freq if oc_on == 'true' else '')
    _draw_setting('4', 'Beeper',           beeper)
    _draw_setting('5', 'SD Card Support',  sd_sup, 'not yet implemented' if sd_sup == 'true' else '')
    sys.stdout.write(_blank())

    # --- NETWORK ---
    sys.stdout.write(_section_label('NETWORK'))
    _draw_setting('6', 'WiFi Autoconnect', wifi_ac)
    sys.stdout.write(_blank())

    # --- PERSONALIZATION ---
    sys.stdout.write(_section_label('PERSONALIZATION'))
    _draw_value('o', 'Owner',              owner)
    _draw_value('t', 'Timezone Offset',    tz,    'hours from UTC')
    _draw_value('d', 'Device ID',          devid, 'shell hostname')
    idle_note = 'minutes (0 = off)' if idle == '0' else 'minutes'
    _draw_value('i', 'Idle Logout',        idle,  idle_note)
    sys.stdout.write(_blank())

    # --- Footer ---
    footer_vis  = '  [1-6] toggle   [o/t/d/i] edit   [r] refresh   [q] quit'
    footer_ansi = ('  ' + _GRY + '[1-6]' + _RST + ' toggle   ' +
                   _GRY + '[o/t/d/i]' + _RST + ' edit   ' +
                   _GRY + '[r]' + _RST + ' refresh   ' +
                   _GRY + '[q]' + _RST + ' quit')
    sys.stdout.write(_row(footer_vis, footer_ansi))
    sys.stdout.write(_blank())
    sys.stdout.write(_BL + _H * I + _BR + '\r\n')
    sys.stdout.write('\r\nChoice: ')

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def settings(args=None):
    """Interactive CLI settings panel."""
    while True:
        _draw()
        try:
            ch = sys.stdin.read(1)
        except Exception:
            break

        if   ch == '1': _rtoggle('Settings.Verbose_Boot')
        elif ch == '2': _rtoggle('Features.Program_Execution')
        elif ch == '3': _rtoggle('Settings.OC_On_Boot')
        elif ch == '4': _rtoggle('Features.beeper')
        elif ch == '5': _rtoggle('Features.SD_Support')
        elif ch == '6': _rtoggle('Settings.Network_Autoconnect')
        elif ch in ('o', 'O'): _redit('System.Owner',         'Owner')
        elif ch in ('t', 'T'): _redit('System.TZ_Offset',     'Timezone Offset', numeric=True)
        elif ch in ('d', 'D'): _redit('System.Device_ID',     'Device ID')
        elif ch in ('i', 'I'): _redit('Settings.Idle_Logout', 'Idle Logout', numeric=True)
        elif ch in ('r', 'R'):
            pass   # redraw
        elif ch in ('q', 'Q', '\x03', '\r', '\n'):
            sys.stdout.write('\x1b[2J\x1b[H')
            ok("Settings saved.")
            return
