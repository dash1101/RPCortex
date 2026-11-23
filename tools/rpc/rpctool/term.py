"""An interactive terminal on the device's console: every key goes to the
device as typed, everything the device prints comes back as it is. Ctrl+]
quits. Works in a Windows console as well as a POSIX terminal.

With a bridge (rpctool.bridge.Bridge), the terminal also offers the device this
computer's network (LINK.md, "The network bridge"): it sends the two bytes
0x1E 0x1F, which every firmware without the bridge ignores, and from then on
answers the device's fetch requests. Their frames are kept off the screen;
everything else, other frames included, is shown as it comes. While a fetch is
running the keys typed are held back and sent once it ends, except Ctrl+C,
which goes straight through and stops it."""
import os
import queue
import sys
import threading
import time

from .bridge import SHOWN
from .errors import DeviceGone
from .link import Frame

ESCAPE = 0x1D        # Ctrl+]
OFFER = b"\x1e\x1f"
FRAME_MAX = 8192     # a "frame" longer than this was not one: show it

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


class _Wire(object):
    """Writes to the device from two threads (keys and the bridge), one whole
    write at a time, holding keys back while a fetch is answered."""

    def __init__(self, transport):
        self.t = transport
        self.lock = threading.Lock()
        self.fetching = threading.Event()
        self.held = bytearray()

    def write(self, data):
        with self.lock:
            self.t.write(data)

    def key(self, data):
        """Keys from the person at the terminal."""
        if self.fetching.is_set():
            if b"\x03" in data:
                self.write(b"\x03")        # Ctrl+C stops the fetch
            self.held.extend(data.replace(b"\x03", b""))
            return
        if self.held:
            data, self.held = bytes(self.held) + data, bytearray()
        self.write(data)

    def release(self):
        """A fetch ended: send what was typed meanwhile."""
        self.fetching.clear()
        if self.held:
            data, self.held = bytes(self.held), bytearray()
            self.write(data)


class TermBridge(object):
    """Splits what the device prints into the screen's bytes and the bridge's
    frames, and answers the frames on a thread of its own."""

    def __init__(self, bridge, wire, status=None):
        self.bridge = bridge
        self.wire = wire
        self.status = status
        self.q = queue.Queue()
        self.frame = None          # bytes of a 0x1E line still arriving, or None
        self.offered = False       # the device answered BRIDGE on
        self.worker = threading.Thread(target=self._work)
        self.worker.daemon = True
        self.worker.start()

    def note(self, text):
        if self.status:
            self.status.write("\r\n[rpc] {}\r\n".format(text))
            self.status.flush()

    def feed(self, data):
        """Bytes from the device in; the bytes to show out."""
        show = bytearray()
        i = 0
        while i < len(data):
            if self.frame is None:
                j = data.find(b"\x1e", i)
                if j < 0:
                    show.extend(data[i:])
                    break
                show.extend(data[i:j])
                self.frame = bytearray()
                i = j + 1
                continue
            j = data.find(b"\n", i)
            if j < 0:
                self.frame.extend(data[i:])
                if len(self.frame) > FRAME_MAX:
                    show.extend(b"\x1e" + bytes(self.frame))
                    self.frame = None
                break
            self.frame.extend(data[i:j])
            i = j + 1
            raw = bytes(self.frame)
            self.frame = None
            body = raw[:-1] if raw.endswith(b"\r") else raw
            f = Frame(body)
            if f.type not in SHOWN:
                show.extend(b"\x1e" + raw + b"\n")      # not the bridge's: as it came
            elif f.type == "BRIDGE":
                if not self.offered:
                    self.offered = True
                    self.note("network bridge on: commands on the device can use this "
                              "computer's connection")
            else:
                if f.type == "FETCH":
                    self.wire.fetching.set()
                self.q.put(f)
        return bytes(show)

    def _next(self, timeout):
        try:
            return self.q.get(timeout=timeout if timeout is not None else 3600)
        except queue.Empty:
            return None

    def _work(self):
        while True:
            f = self.q.get()
            if f is None:
                return
            if f.type != "FETCH":
                continue            # the end of a fetch already given up on
            self.wire.fetching.set()
            again = []
            try:
                left = self.bridge.serve(f, self._next, self.wire.write)
                again = [x for x in left if x.type == "FETCH"]
            except Exception as e:  # never take the terminal down with it
                self.note("bridge: {}".format(e))
            finally:
                self.wire.release()
            for x in again:
                self.q.put(x)

    def stop(self):
        self.q.put(None)


def _pump_device(transport, out, stop, notes, tb=None):
    try:
        while not stop.is_set():
            data = transport.read(0.05)
            if data:
                if tb is not None:
                    data = tb.feed(data)
                if data:
                    out.write(data)
                    out.flush()
    except DeviceGone as e:
        notes.append("\r\n[the device went away: {}]\r\n".format(e))
        stop.set()
    except Exception as e:  # keep the terminal usable whatever happens
        notes.append("\r\n[stopped reading: {}]\r\n".format(e))
        stop.set()


def run(transport, stdin_fd=None, out=None, status=None, bridge=None):
    """Pass keys and output through until Ctrl+] (or the device goes away).
    With `bridge`, offer the device this computer's network and answer it."""
    out = out or getattr(sys.stdout, "buffer", sys.stdout)
    status = status or sys.stderr
    status.write("Connected to {}. Ctrl+] to quit.\r\n".format(transport.name))
    status.flush()
    wire = _Wire(transport)
    tb = TermBridge(bridge, wire, status) if bridge is not None else None
    stop = threading.Event()
    notes = []
    reader = threading.Thread(target=_pump_device, args=(transport, out, stop, notes, tb))
    reader.daemon = True
    reader.start()
    if tb is not None:
        try:
            wire.write(OFFER)
        except DeviceGone:
            pass
    try:
        if os.name == "nt" and stdin_fd is None:
            _keys_windows(wire, stop)
        else:
            _keys_posix(wire, stop, sys.stdin.fileno() if stdin_fd is None else stdin_fd)
    finally:
        stop.set()
        if tb is not None:
            tb.stop()
        reader.join(1.0)
        for n in notes:
            status.write(n)
        status.write("\r\n[left the terminal]\r\n")
        status.flush()


def _keys_posix(wire, stop, fd):
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
                if wire.held and not wire.fetching.is_set():
                    wire.release()
                continue
            data = os.read(fd, 1024)
            if not data:
                break
            cut = data.find(bytes([ESCAPE]))
            if cut >= 0:
                if cut:
                    wire.key(data[:cut])
                break
            wire.key(data)
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


def _keys_windows(wire, stop):
    import msvcrt
    _enable_vt_output()
    while not stop.is_set():
        try:
            if not msvcrt.kbhit():
                if wire.held and not wire.fetching.is_set():
                    wire.release()
                time.sleep(0.01)
                continue
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):
                wire.key(windows_key(ch, msvcrt.getwch()))
            elif ord(ch) == ESCAPE:
                break
            else:
                wire.key(ch.encode("utf-8"))
        except KeyboardInterrupt:
            # The console turns Ctrl+C into an interrupt before getwch sees
            # it; the device should get it as the byte it is.
            wire.key(b"\x03")
