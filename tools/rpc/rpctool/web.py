"""Serving the browser tool (tools/web) on localhost.

Web Serial is only offered to secure pages, and http://localhost counts as
one, so a plain local server is all the browser tool needs.
"""
import functools
import os
import sys
import threading
import webbrowser

try:
    from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
except ImportError:  # pragma: no cover
    raise

from .errors import RpcError

# Set explicitly: Python takes these from the system, and on Windows the
# registry can map .js to text/plain, which a browser refuses to run as a
# module.
TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".md": "text/plain; charset=utf-8",
    "": "application/octet-stream",
}


def find_web_dir(explicit=None):
    """--dir, then RPC_WEB_DIR, then tools/web beside this package in a checkout."""
    for cand in (explicit, os.environ.get("RPC_WEB_DIR")):
        if cand:
            if os.path.isfile(os.path.join(cand, "index.html")):
                return os.path.abspath(cand)
            raise RpcError("{} has no index.html".format(cand))
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.normpath(os.path.join(here, "..", "..", "web"))
    if os.path.isfile(os.path.join(cand, "index.html")):
        return cand
    raise RpcError("The browser tool's files were not found. Run from a checkout of the "
                   "repository (pip install -e tools/rpc), or pass --dir <path to tools/web>.")


class _Handler(SimpleHTTPRequestHandler):
    extensions_map = TYPES

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        SimpleHTTPRequestHandler.end_headers(self)

    def log_message(self, fmt, *args):
        pass


def make_server(directory, port=8765, host="127.0.0.1"):
    handler = functools.partial(_Handler, directory=directory)
    try:
        return ThreadingHTTPServer((host, port), handler)
    except OSError as e:
        raise RpcError("cannot listen on {}:{}: {}  (try --port)".format(host, port, e))


def serve(directory, port=8765, open_browser=True, stream=None, ready=None):
    stream = stream or sys.stderr
    srv = make_server(directory, port)
    url = "http://localhost:{}/".format(srv.server_address[1])
    stream.write("Serving {} at {}\nOpen it in Chrome or Edge. Ctrl+C to stop.\n"
                 .format(directory, url))
    stream.flush()
    if open_browser:
        threading.Timer(0.3, webbrowser.open, args=(url,)).start()
    if ready:
        ready(srv, url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0
