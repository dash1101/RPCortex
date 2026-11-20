"""The serial port: finding it, opening it the way the device needs, and reading
what is waiting rather than a fixed count.

pyserial is imported only when a real port is opened or listed, so the rest of
the library (and its tests) runs without it.
"""
import os
import re
import sys
import time

from .errors import DeviceGone, RpcError

# Raspberry Pi's vendor ID; RPCortex uses its own product ID in that space
# (os/usb_descriptors.c) and names itself "RPCortex" by "Nova Labs".
RPI_VID = 0x2E8A
RPC_PID = 0x0011
BOOTSEL_PIDS = {0x0003: "RP2040", 0x000F: "RP2350"}

ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][A-Za-z0-9]|\x1b[=>78]")

# How long one read on the port blocks. Short, because the reader asks for what
# is waiting and loops: a long timeout here is added to every chunk's round trip.
READ_SLICE = 0.05


def strip_ansi(data):
    return ANSI.sub(b"", data)


def _pyserial():
    try:
        import serial  # noqa: F401
        import serial.tools.list_ports  # noqa: F401
    except ImportError:
        raise RpcError("pyserial is needed for a real port:  pip install pyserial")
    return serial


def _gone_errors():
    errs = [OSError]
    try:
        import serial
        errs.append(serial.SerialException)
    except ImportError:
        pass
    return tuple(errs)


class Transport(object):
    """A byte stream to the device over anything shaped like pyserial's Serial:
    read(n), write(b), flush(), in_waiting, reset_input_buffer(), close()."""

    def __init__(self, ser, name=None):
        self.ser = ser
        self.name = name or getattr(ser, "port", None) or "device"
        self._gone = _gone_errors()
        self.closed = False

    def read(self, timeout):
        """Whatever is waiting, after up to `timeout` seconds for the first byte.
        Returns b"" when nothing came."""
        deadline = time.monotonic() + max(0.0, timeout)
        while True:
            try:
                waiting = self.ser.in_waiting
                data = self.ser.read(max(1, waiting))
            except self._gone as e:
                raise DeviceGone("the port went away ({})".format(e))
            if data:
                return bytes(data)
            if time.monotonic() >= deadline:
                return b""

    def write(self, data):
        try:
            self.ser.write(data)
            self.ser.flush()
        except self._gone as e:
            raise DeviceGone("the port went away ({})".format(e))

    def discard_input(self):
        try:
            self.ser.reset_input_buffer()
        except self._gone:
            pass

    def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            self.ser.close()
        except Exception:
            pass


def open_serial(path, baud=115200):
    """Open a real port with DTR asserted.

    The Pico SDK's USB console counts the host as connected only while DTR is
    asserted and drops output otherwise, so it is set before the port opens
    (pyserial applies it as part of opening) and again after, for drivers that
    ignore the first.
    """
    serial = _pyserial()
    s = serial.Serial()
    s.port = path
    s.baudrate = baud
    s.timeout = READ_SLICE
    s.write_timeout = 10
    s.dtr = True
    s.rts = True
    try:
        s.open()
    except serial.SerialException as e:
        raise RpcError(_open_hint(path, e))
    try:
        s.dtr = True
    except (OSError, serial.SerialException):
        pass
    return s


def _open_hint(path, exc):
    msg = "cannot open {}: {}".format(path, exc)
    text = str(exc).lower()
    if sys.platform.startswith("linux") and ("permission" in text or "errno 13" in text):
        msg += ("\n  Add yourself to the group that owns the port (usually 'dialout'):"
                "\n    sudo usermod -aG dialout $USER   then log out and back in.")
    elif "busy" in text or "access is denied" in text or "errno 16" in text:
        msg += "\n  Another program has the port open (a terminal, Thonny, the browser tool). Close it first."
    return msg


def touch_1200(path):
    """Open the port at 1200 baud and close it: the Pico SDK reboots the board
    into BOOTSEL when the host sets that rate."""
    serial = _pyserial()
    try:
        s = serial.Serial()
        s.port = path
        s.baudrate = 1200
        s.dtr = True
        s.open()
        time.sleep(0.1)
        s.close()
    except (OSError, serial.SerialException):
        # The board resetting under the open is the expected outcome.
        pass


class PortInfo(object):
    def __init__(self, device, description="", vid=None, pid=None,
                 manufacturer="", product="", serial_number=""):
        self.device = device
        self.description = description or ""
        self.vid = vid
        self.pid = pid
        self.manufacturer = manufacturer or ""
        self.product = product or ""
        self.serial_number = serial_number or ""

    @property
    def rpcortex(self):
        return (self.pid == RPC_PID or "rpcortex" in self.product.lower()
                or "nova labs" in self.manufacturer.lower())

    def label(self):
        ids = "{:04X}:{:04X}".format(self.vid or 0, self.pid or 0)
        what = "RPCortex" if self.rpcortex else (self.product or self.description or "Pico")
        sn = "  serial {}".format(self.serial_number) if self.serial_number else ""
        return "{:<16} {}  {}{}".format(self.device, ids, what, sn)


def list_candidates():
    """Serial ports that belong to a Raspberry Pi silicon board, RPCortex first."""
    _pyserial()
    from serial.tools import list_ports
    found = []
    for p in list_ports.comports():
        if p.vid != RPI_VID:
            continue
        found.append(PortInfo(p.device, p.description, p.vid, p.pid,
                              p.manufacturer, p.product, p.serial_number))
    found.sort(key=lambda p: (not p.rpcortex, p.device))
    return found


def choose_port(port=None, ask=True, stream=None):
    """The port to use: --port, then RPC_PORT, then the one RPCortex device
    plugged in. With several, list them and ask (or fail when not interactive)."""
    if port:
        return port
    env = os.environ.get("RPC_PORT", "").strip()
    if env:
        return env
    stream = stream or sys.stderr
    cands = list_candidates()
    rpc = [p for p in cands if p.rpcortex]
    pool = rpc or cands
    if not pool:
        raise RpcError("No RPCortex device found. Plug one in, or name the port with "
                       "--port (or RPC_PORT).  'rpc ports' lists what is connected.")
    if len(pool) == 1:
        return pool[0].device
    if not (ask and sys.stdin.isatty()):
        raise RpcError("Several devices are connected; choose one with --port:\n  " +
                       "\n  ".join(p.label() for p in pool))
    stream.write("Several devices are connected:\n")
    for i, p in enumerate(pool, 1):
        stream.write("  {}. {}\n".format(i, p.label()))
    while True:
        stream.write("Which one? [1-{}] ".format(len(pool)))
        stream.flush()
        ans = sys.stdin.readline().strip()
        if not ans:
            raise RpcError("No port chosen.")
        if ans.isdigit() and 1 <= int(ans) <= len(pool):
            return pool[int(ans) - 1].device
