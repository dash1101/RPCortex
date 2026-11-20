"""`rpc web`: the browser tool is served from localhost with types a browser
will run as modules."""
import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402,F401
from rpctool import web  # noqa: E402
from rpctool.errors import RpcError  # noqa: E402

try:
    from urllib.request import urlopen
except ImportError:  # pragma: no cover
    raise


class TestWeb(unittest.TestCase):
    def test_serves_modules_as_javascript(self):
        d = web.find_web_dir()
        srv = web.make_server(d, 0)
        th = threading.Thread(target=srv.serve_forever)
        th.daemon = True
        th.start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        base = "http://127.0.0.1:{}/".format(srv.server_address[1])
        r = urlopen(base)
        self.assertIn(b"<html", r.read().lower())
        r = urlopen(base + "js/link.js")
        self.assertTrue(r.headers["Content-Type"].startswith("text/javascript"))
        self.assertEqual(srv.server_address[0], "127.0.0.1")

    def test_explicit_dir_must_hold_the_tool(self):
        with self.assertRaises(RpcError):
            web.find_web_dir(tempfile.gettempdir())


if __name__ == "__main__":
    unittest.main()
