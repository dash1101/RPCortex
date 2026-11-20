"""Shared by the tests: the import path, short timings, and running the CLI
against a mock device with nothing real reachable."""
import contextlib
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                     # tools/rpc
REPO = os.path.dirname(os.path.dirname(ROOT))    # the repository
for p in (ROOT, HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

from rpctool import cli, flash, transport  # noqa: E402
from rpctool.device import Device  # noqa: E402
from rpctool.link import Timing  # noqa: E402

try:
    from unittest import mock
except ImportError:  # pragma: no cover
    import mock  # type: ignore


def fast():
    t = Timing()
    t.settle = 0.05
    t.enter = 2.0
    t.request = 3.0
    t.transfer = 2.0
    t.abort = 3.0
    t.bye = 0.5
    t.type_delay = 0.0
    t.legacy_put_ack = 2.0
    return t


def connect(dev, **kw):
    """A Device on the mock, connected."""
    kw.setdefault("timing", fast())
    return Device(transport.Transport(dev.open(), "mock"), **kw).connect()


class _Out(object):
    """Stands in for sys.stdout/stderr: text and bytes both land in one buffer."""

    def __init__(self):
        self.buffer = io.BytesIO()

    def write(self, s):
        self.buffer.write(s.encode("utf-8"))
        return len(s)

    def flush(self):
        pass

    def isatty(self):
        return False

    def text(self):
        return self.buffer.getvalue().decode("utf-8", "replace")


@contextlib.contextmanager
def no_real_ports():
    """Nothing in a test may reach a real serial port, a real BOOTSEL reset,
    picotool or a browser."""
    def refuse(*a, **k):
        raise AssertionError("a test tried to open a real port")
    env = dict(os.environ)
    env.pop("RPC_PORT", None)
    env.pop("RPC_PASSWORD", None)
    with mock.patch.dict(os.environ, env, clear=True), \
            mock.patch.object(transport, "open_serial", refuse), \
            mock.patch.object(transport, "touch_1200", refuse), \
            mock.patch.object(transport, "list_candidates", lambda: []), \
            mock.patch.object(flash, "find_drives", lambda: []), \
            mock.patch.object(flash, "bootsel_on_usb", lambda: False), \
            mock.patch("shutil.which", lambda name: None), \
            mock.patch("webbrowser.open", lambda *a, **k: True):
        yield


def run_cli(dev, argv, env=None):
    """Run `rpc <argv>` against the mock. Returns (status, stdout, stderr)."""
    out, err = _Out(), _Out()
    opener = (lambda path, baud=115200: dev.open()) if dev is not None else None
    with no_real_ports():
        os.environ.update(env or {})
        patches = [mock.patch.object(sys, "stdout", out), mock.patch.object(sys, "stderr", err)]
        if opener:
            patches.append(mock.patch.object(transport, "open_serial", opener))
        with contextlib.ExitStack() as stack:
            for p in patches:
                stack.enter_context(p)
            argv = list(argv)
            if dev is not None and "--port" not in argv:
                argv = ["--port", "mock"] + argv
            rc = cli.main(argv, timing=fast())
    return rc, out.buffer.getvalue(), err.text()
