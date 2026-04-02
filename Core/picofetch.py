# Desc: System information display for RPCortex - Nebula OS  (neofetch style)
# File: /Core/picofetch.py
# Last Updated: 4/1/2026
# Lang: MicroPython, English
# Version: v0.8.1-beta4
# Author: dash1101

import sys
import os
import gc
import machine
import utime

# ---------------------------------------------------------------------------
# ANSI helpers
# ---------------------------------------------------------------------------

_COLORS = {
    'reset':          '\x1b[0m',
    'bold':           '\x1b[1m',
    'dim':            '\x1b[2m',
    'black':          '\x1b[30m',
    'red':            '\x1b[31m',
    'green':          '\x1b[32m',
    'yellow':         '\x1b[33m',
    'blue':           '\x1b[34m',
    'magenta':        '\x1b[35m',
    'cyan':           '\x1b[36m',
    'white':          '\x1b[37m',
    'bright_black':   '\x1b[90m',
    'bright_red':     '\x1b[91m',
    'bright_green':   '\x1b[92m',
    'bright_yellow':  '\x1b[93m',
    'bright_blue':    '\x1b[94m',
    'bright_magenta': '\x1b[95m',
    'bright_cyan':    '\x1b[96m',
    'bright_white':   '\x1b[97m',
}

def _c(name):
    return _COLORS.get(name, '')

def _w(s):
    sys.stdout.write(s)

def _ln(s=''):
    _w(s + '\r\n')

# ---------------------------------------------------------------------------
# RPCortex logo  (7 lines, gradient cyan → blue → magenta)
# ---------------------------------------------------------------------------

_LOGO = [
    ('bright_cyan',    '      :::::::::  :::::::::   ::::::::  '),
    ('cyan',           '     :+:    :+: :+:    :+: :+:    :+: '),
    ('cyan',           '    +:+    +:+ +:+    +:+ +:+         '),
    ('bright_blue',    '   +#++:++#:  +#++:++#+  +#+          '),
    ('blue',           '  +#+    +#+ +#+        +#+           '),
    ('bright_magenta', ' #+#    #+# #+#        #+#    #+#     '),
    ('magenta',        '###    ### ###         ########       '),
]

# ---------------------------------------------------------------------------
# Registry helper  (non-fatal — returns None on any failure)
# ---------------------------------------------------------------------------

def _reg(key):
    try:
        if '/Core' not in sys.path:
            sys.path.append('/Core')
        import regedit
        return regedit.read(key)
    except Exception:
        return None

# ---------------------------------------------------------------------------
# Hardware info gathering
# ---------------------------------------------------------------------------

def _get_board():
    try:
        m = getattr(sys.implementation, '_machine', None)
        if m:
            return m
    except Exception:
        pass
    return 'Raspberry Pi Pico'


def _get_cpu():
    """Detect CPU model from sys.implementation._machine."""
    try:
        m = getattr(sys.implementation, '_machine', '') or ''
        m_up = m.upper()
        if 'RP2350' in m_up:
            return 'RP2350  Dual-Core ARM Cortex-M33'
        if 'RP2040' in m_up:
            return 'RP2040  Dual-Core ARM Cortex-M0+'
        if sys.platform == 'rp2':
            return 'RP2040  Dual-Core ARM Cortex-M0+'
        return m if m else sys.platform
    except Exception:
        return sys.platform


def _get_mpy():
    try:
        v = sys.implementation.version
        return 'MicroPython {}.{}.{}'.format(v[0], v[1], v[2])
    except Exception:
        return 'MicroPython'


def _get_freq():
    try:
        return '{} MHz'.format(machine.freq() // 1_000_000)
    except Exception:
        return 'Unknown'


def _get_ram():
    try:
        gc.collect()
        free  = gc.mem_free()
        alloc = gc.mem_alloc()
        total = free + alloc
        pct   = alloc * 100 // total if total else 0
        return '{} KB used / {} KB total  ({}%)'.format(
            alloc // 1024, total // 1024, pct)
    except Exception:
        return 'Unknown'


def _get_flash():
    try:
        sv    = os.statvfs('/')
        total = sv[0] * sv[2]
        free  = sv[0] * sv[3]
        used  = total - free
        return '{} KB used / {} KB total'.format(used // 1024, total // 1024)
    except Exception:
        return 'Unknown'


def _get_uptime():
    try:
        ms = utime.ticks_ms()
        s  = ms // 1000
        m  = s  // 60
        h  = m  // 60
        s  = s  % 60
        m  = m  % 60
        if h:
            return '{}h {}m {}s'.format(h, m, s)
        if m:
            return '{}m {}s'.format(m, s)
        return '{}s'.format(s)
    except Exception:
        return 'Unknown'


def _get_temp():
    """Read the RP2040 / RP2350 onboard temperature sensor."""
    try:
        m = getattr(sys.implementation, '_machine', '') or ''
        if 'RP2350' in m.upper():
            # RP2350 — ADC channel 4 same formula (approximate)
            from machine import ADC
            sensor  = ADC(4)
            raw     = sensor.read_u16()
            voltage = raw * 3.3 / 65535
            temp_c  = 27.0 - (voltage - 0.706) / 0.001721
            return '{:.1f} °C  (onboard)'.format(temp_c)
        else:
            # RP2040
            from machine import ADC
            sensor  = ADC(4)
            raw     = sensor.read_u16()
            voltage = raw * 3.3 / 65535
            temp_c  = 27.0 - (voltage - 0.706) / 0.001721
            return '{:.1f} °C  (onboard)'.format(temp_c)
    except Exception:
        return 'Not available'


def _get_uid():
    try:
        uid = machine.unique_id()
        return ':'.join('{:02x}'.format(b) for b in uid)
    except Exception:
        return 'Unknown'


def _get_wifi():
    """Return WiFi status string."""
    try:
        import network
        wlan = network.WLAN(network.STA_IF)
        if wlan.isconnected():
            ip = wlan.ifconfig()[0]
            return 'Connected  ({})'.format(ip)
        elif wlan.active():
            return 'Active  (not connected)'
        else:
            return 'Inactive'
    except Exception:
        autoconn = _reg('Settings.Network_Autoconnect')
        if autoconn == 'true':
            return 'Configured (autoconnect on)'
        return 'Not connected'


def _color_swatches():
    names = ['black','red','green','yellow','blue','magenta','cyan','white']
    row1 = ''.join(_c(n) + '\u2588\u2588\u2588' + _c('reset') for n in names)
    row2 = ''.join(_c('bright_'+n) + '\u2588\u2588\u2588' + _c('reset') for n in names)
    return row1, row2

# ---------------------------------------------------------------------------
# Main display function
# ---------------------------------------------------------------------------

def fetch(color='bright_cyan', show_ascii=True):
    """
    Print RPCortex system info in neofetch style.
    Reads OS metadata from the registry when available.

    Args:
        color      : accent color name
        show_ascii : show the RPCortex ASCII logo
    """
    accent = _c(color)
    bold   = _c('bold')
    dim    = _c('dim')
    reset  = _c('reset')
    white  = _c('bright_white')
    gray   = _c('bright_black')

    # Gather info (some calls take a moment on first run)
    os_ver  = _reg('Settings.Version') or 'Unknown'
    user    = _reg('Settings.Active_User') or 'Unknown'
    board   = _get_board()
    cpu     = _get_cpu()
    mpy     = _get_mpy()
    freq    = _get_freq()
    ram     = _get_ram()
    flash   = _get_flash()
    uptime  = _get_uptime()
    temp    = _get_temp()
    uid     = _get_uid()
    wifi    = _get_wifi()
    swatch1, swatch2 = _color_swatches()

    # Header line:  user@nebula
    host_line = (
        accent + bold + user   + reset +
        white  + '@'           + reset +
        accent + bold + 'nebula' + reset
    )
    separator = accent + ('\u2500' * 28) + reset

    info_rows = [
        ('OS',      'RPCortex {}  ({})'.format(os_ver, sys.platform)),
        ('Board',   board),
        ('CPU',     cpu),
        ('Freq',    freq),
        ('Runtime', mpy),
        ('RAM',     ram),
        ('Flash',   flash),
        ('Temp',    temp),
        ('Uptime',  uptime),
        ('WiFi',    wifi),
        ('UID',     uid),
        ('Shell',   'Launchpad  (RPCortex Nebula)'),
    ]

    right = []
    right.append('')
    right.append(host_line)
    right.append(separator)
    for label, value in info_rows:
        right.append(
            accent + bold + '{:>8s}'.format(label) + reset +
            dim    + ' : ' + reset +
            white  + value + reset
        )
    right.append('')
    right.append('  ' + swatch1)
    right.append('  ' + swatch2)
    right.append('')

    if show_ascii:
        logo = list(_LOGO)
        logo_width = max(len(lt) for _, lt in logo)
        n = max(len(logo), len(right))
        while len(logo)  < n: logo.append(('reset', ''))
        while len(right) < n: right.append('')

        _w('\r\n')
        for i in range(n):
            lc, lt = logo[i]
            padded = lt + ' ' * max(0, logo_width - len(lt))
            _w(_c(lc) + padded + reset + '  ' + right[i] + '\r\n')
        _w('\r\n')
    else:
        _w('\r\n')
        _ln(host_line)
        _ln(separator)
        for label, value in info_rows:
            _ln(
                accent + bold + '{:>8s}'.format(label) + reset +
                dim    + ' : ' + reset +
                white  + value + reset
            )
        _ln()
        _ln('  ' + swatch1)
        _ln('  ' + swatch2)
        _ln()


# Run directly if called as a script
if __name__ == '__main__':
    fetch()
