"""rpc tui: a full-screen program for an RPCortex v2 device.

It talks to the device through the link protocol with the rpctool library
(Device, the same one `rpc` uses), from one worker thread, and draws with
curses. Everything under this package is the TUI's own and nothing reads a
file by a path in the repository, so it can be shipped on its own with
rpctool beside it.

What it needs from rpctool: Device and Transport, the errors, repo (the
index, fetch, download), flash, backup and pkg. Besides Device's methods,
the worker reads a few of its attributes: dev.t.name, dev.gone, dev.mode,
dev.hello, and on dev.link: bye(), ping(), dropped and the frame reader's
buffer (r.buf, what arrived after BYE). backup.restart_unsaved uses
link.send and link.r.next_frame, as Device.reboot does.

    rpc tui              python -m rpctool.tui
"""
import argparse
import os
import sys

__version__ = "0.1.0"


DESCRIPTION = ("A full-screen program for an RPCortex v2 device: its state, files, packages, "
               "firmware, log and console, over the link protocol.")


def add_arguments(ap, connection=True):
    """The TUI's options, on its own parser or on `rpc tui`'s (which has
    --port and --user already, from rpc's common options)."""
    ap.add_argument("--version", action="store_true", help="print the version and exit")
    if connection:
        ap.add_argument("-p", "--port", default=None,
                        help="serial port (default: $RPC_PORT, else the RPCortex device found)")
        ap.add_argument("-u", "--user", default=None,
                        help="the account to log in with if the device is at its login prompt")
    ap.add_argument("--theme", default=None, help="tui, phosphor, amber, ember, frost, lilac, "
                    "mono, ocean or paper (t cycles them; the choice is remembered)")
    ap.add_argument("--colours", "--colors", dest="colours", default=None,
                    choices=("true", "256", "8", "mono"),
                    help="colour depth (default: what the terminal says it has)")
    ap.add_argument("--ascii", action="store_true", help="draw boxes with ASCII only")
    ap.add_argument("--index", default=None, help="package index URL or file")
    ap.add_argument("--releases", default=None,
                    help="where latest.json and releases.json are (a URL or a folder)")
    return ap


def build_parser():
    return add_arguments(argparse.ArgumentParser(prog="rpc-tui", description=DESCRIPTION))


def _releases(base):
    if not base:
        return None, None
    sep = "" if base.endswith(("/", os.sep)) else ("/" if "://" in base else os.sep)
    return base + sep + "latest.json", base + sep + "releases.json"


def real_opener(worker):
    """The serial port: the one named, else the RPCortex device plugged in.
    Never asks on stdin (curses owns the terminal); several devices are a
    question for the screen."""
    from rpctool import transport
    from rpctool.errors import RpcError
    from .worker import ChoosePort
    path = worker.port or os.environ.get("RPC_PORT", "").strip()
    if not path:
        cands = transport.list_candidates()
        pool = [p for p in cands if p.rpcortex] or cands
        if not pool:
            raise RpcError("no RPCortex device found; plug one in, or start with --port")
        if len(pool) == 1:
            path = pool[0].device
        elif worker.last_port in [p.device for p in pool]:
            path = worker.last_port
        else:
            raise ChoosePort(pool)
    ser = transport.open_serial(path)
    return transport.Transport(ser, path)


def main(argv=None):
    return run(build_parser().parse_args(argv))


def run(a):
    """Run with parsed options (from main, or from `rpc tui`)."""
    if a.version:
        sys.stdout.write(__version__ + "\n")
        return 0
    from . import cursesui, themes
    cursesui.import_curses()            # the windows-curses hint, before anything else
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        sys.stderr.write("rpc tui needs a terminal.\n")
        return 2
    if a.theme and a.theme not in themes.THEMES:
        sys.stderr.write("no theme called {}; there are {}\n".format(
            a.theme, ", ".join(themes.ORDER)))
        return 2
    from .app import App
    from .worker import Worker
    manifest, history = _releases(a.releases)
    worker = Worker(real_opener, user=getattr(a, "user", None), port=getattr(a, "port", None))
    app = App(worker, theme=a.theme, index_url=a.index, manifest_url=manifest,
              history_url=history)
    if a.theme:
        app.config["theme"] = a.theme
        themes.save_config(app.config)
    worker.start()
    try:
        cursesui.run(app, theme_depth=a.colours, ascii_only=a.ascii)
    except KeyboardInterrupt:
        pass
    finally:
        worker.stop()
        worker.join(5.0)
    return 0
