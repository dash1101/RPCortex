"""An interactive terminal on the device's console: every key goes to the
device as typed, everything the device prints comes back as it is. Ctrl+]
quits. Works in a Windows console as well as a POSIX terminal."""
import os
import sys
import threading
import time

from .errors import DeviceGone

ESCAPE = 0x1D        # Ctrl+]

# msvcrt reports a special key as a prefix (\x00 or \xe0) and a code. These
# are the codes, as the ANSI sequences a terminal would send for them.
WINDOWS_KEYS = {
    "H": b"\x1b[A", "P": b"\x1b[B", "M": b"\x1b[C", "K": b"\x1b[D",
    "G": b"\x1b[H", "O": b"\x1b[F", "S": b"\x1b[3~", "R": b"\x1b[2~",
    "I": b"\x1b[5~", "Q": b"\x1b[6~",
    "s": b"\x1b[1;5D", "t": b"\x1b[1;5C",      # Ctrl+Left, Ctrl+Right
    "\x8d": b"\x1b[1;5A", "\x91": b"\x1b[1;5B",  # Ctrl+Up, Ctrl+Down
    ";": b"\x1bOP", "<": b"\x1bOQ", "=": b"\x1bOR", ">": b"\x1bOS",
}


def windows_key(prefix, code):
    """The bytes to send for one special key, or b"" for one with no meaning."""
    return WINDOWS_KEYS.get(code, b"")


def _pump_device(transport, out, stop, notes):
    try:
        while not stop.is_set():
            data = transport.read(0.05)
            if data:
                out.write(data)
                out.flush()
    except DeviceGone as e:
        notes.append("\r\n[the device went away: {}]\r\n".format(e))
        stop.set()
    except Exception as e:  # keep the terminal usable whatever happens
        notes.append("\r\n[stopped reading: {}]\r\n".format(e))
        stop.set()


def run(transport, stdin_fd=None, out=None, status=None):
    """Pass keys and output through until Ctrl+] (or the device goes away)."""
    out = out or getattr(sys.stdout, "buffer", sys.stdout)
    status = status or sys.stderr
    status.write("Connected to {}. Ctrl+] to quit.\r\n".format(transport.name))
    status.flush()
    stop = threading.Event()
    notes = []
    reader = threading.Thread(target=_pump_device, args=(transport, out, stop, notes))
    reader.daemon = True
    reader.start()
    try:
        if os.name == "nt" and stdin_fd is None:
            _keys_windows(transport, stop)
        else:
            _keys_posix(transport, stop, sys.stdin.fileno() if stdin_fd is None else stdin_fd)
    finally:
        stop.set()
        reader.join(1.0)
        for n in notes:
            status.write(n)
        status.write("\r\n[left the terminal]\r\n")
        status.flush()


def _keys_posix(transport, stop, fd):
    import select
    import termios
    import tty
    old = None
    try:
        old = termios.tcgetattr(fd)
        tty.setraw(fd)
    except termios.error:
        old = None          # not a terminal (a pipe): pass bytes through as they come
    try:
        while not stop.is_set():
            ready, _, _ = select.select([fd], [], [], 0.1)
            if not ready:
                continue
            data = os.read(fd, 1024)
            if not data:
                break
            cut = data.find(bytes([ESCAPE]))
            if cut >= 0:
                if cut:
                    transport.write(data[:cut])
                break
            transport.write(data)
    finally:
        if old is not None:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)


def _enable_vt_output():
    """Windows 10 and later draw ANSI colour once this is switched on."""
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if k.GetConsoleMode(h, ctypes.byref(mode)):
            k.SetConsoleMode(h, mode.value | 0x0004)
        k.SetConsoleOutputCP(65001)
    except Exception:
        pass


def _keys_windows(transport, stop):
    import msvcrt
    _enable_vt_output()
    while not stop.is_set():
        try:
            if not msvcrt.kbhit():
                time.sleep(0.01)
                continue
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):
                transport.write(windows_key(ch, msvcrt.getwch()))
            elif ord(ch) == ESCAPE:
                break
            else:
                transport.write(ch.encode("utf-8"))
        except KeyboardInterrupt:
            # The console turns Ctrl+C into an interrupt before getwch sees
            # it; the device should get it as the byte it is.
            transport.write(b"\x03")
