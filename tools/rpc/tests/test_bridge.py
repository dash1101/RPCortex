"""The network bridge (LINK.md protocol 3) from the computer's side.

The mock device has no network of its own (netless=True) and asks the host to
fetch, mid-EXEC and mid-terminal, the way the firmware does. The host answers
from a real http.server on 127.0.0.1, through rpctool.bridge with urllib, so the
redirects, the size cap and the allowlist are exercised for real. Nothing leaves
this computer.
"""
import gzip
import hashlib
import json
import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: F401  (sets the import path)
from mockdev import MockDevice, make_app
from rpctool import bridge as _bridge
from rpctool import term as _term
from rpctool.errors import LinkError
from rpctool.link import Frame

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BIG = bytes(bytearray((i * 37 + 11) & 0xFF for i in range(200 * 1024 + 5)))
APP = make_app("greet2", "2.1", 3000)


class Site(BaseHTTPRequestHandler):
    """What the bridge fetches from. Paths say what they do."""
    seen = []

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/octet-stream", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        data = self.rfile.read(n)
        Site.seen.append(("POST", self.path, dict(self.headers), data))
        self._send(200, b"posted " + data, "text/plain")

    def do_GET(self):
        Site.seen.append(("GET", self.path, dict(self.headers), b""))
        host = "http://127.0.0.1:{}".format(self.server.server_address[1])
        p = self.path
        if p == "/big.bin":
            self._send(200, BIG)
        elif p == "/small.txt":
            self._send(200, b"hello through the bridge\n", "text/plain")
        elif p == "/index.json":
            idx = {"packages": [{"name": "greet2", "ver": "2.1", "arch": "armv8m",
                                 "url": host + "/pkg/greet2.app",
                                 "sha256": hashlib.sha256(APP).hexdigest()}]}
            self._send(200, json.dumps(idx).encode(), "application/json")
        elif p == "/pkg/greet2.app":
            self._send(200, APP)
        elif p == "/moved":
            self._send(302, b"", extra=(("Location", host + "/small.txt"),))
        elif p == "/moved-away":
            # Same server, another name for it: an allowlist of 127.0.0.1 must
            # refuse the hop even though the first URL was allowed.
            self._send(302, b"", extra=(("Location", "http://localhost:{}/small.txt".format(
                self.server.server_address[1])),))
        elif p == "/to-file":
            self._send(302, b"", extra=(("Location", "file:///etc/passwd"),))
        elif p == "/gz":
            z = gzip.compress(b"unzipped on the host\n")
            self._send(200, z, "text/plain", (("Content-Encoding", "gzip"),))
        elif p == "/slow":
            time.sleep(3.0)
            self._send(200, b"worth the wait\n", "text/plain")
        else:
            self._send(404, b"not here\n", "text/plain")


class BridgeBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = ThreadingHTTPServer(("127.0.0.1", 0), Site)
        cls.url = "http://127.0.0.1:{}".format(cls.srv.server_address[1])
        cls.thread = threading.Thread(target=cls.srv.serve_forever)
        cls.thread.daemon = True
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def make(self, policy=None, **dev_args):
        dev_args.setdefault("proto", 3)
        dev_args.setdefault("netless", True)
        self.dev = MockDevice(**dev_args).start()
        self.dev.repo_index = self.url + "/index.json"
        self.log = []
        self.bridge = _bridge.Bridge(policy or _bridge.Policy(), log=self.log.append)
        self.d = support.connect(self.dev, bridge=self.bridge, sudo_password="rootpw")
        return self.d

    def tearDown(self):
        d = getattr(self, "d", None)
        if d:
            d.close()
        if getattr(self, "dev", None):
            self.dev.shutdown()


class TestOffer(BridgeBase):
    def test_offered_on_connect_and_withdrawn_on_close(self):
        d = self.make()
        self.assertEqual(d.hello.get("bridge"), "1")
        self.assertTrue(d.bridged)
        self.assertTrue(self.dev.bridge_offered)
        self.assertIn("BRIDGE on", self.dev.requests)
        d.close()
        self.d = None
        self.assertIn("BRIDGE off", self.dev.requests)
        self.assertFalse(self.dev.bridge_offered)

    def test_not_offered_to_a_board_with_its_own_network(self):
        d = self.make(netless=False)
        self.assertEqual(d.hello.get("bridge"), "0")
        self.assertFalse(d.bridged)
        self.assertNotIn("BRIDGE on", self.dev.requests)

    def test_not_offered_to_protocol_2(self):
        d = self.make(proto=2)
        self.assertFalse(d.bridged)
        self.assertNotIn("BRIDGE on", self.dev.requests)

    def test_nothing_is_sent_without_an_offer(self):
        self.dev = MockDevice(proto=3, netless=True).start()
        self.d = support.connect(self.dev)            # no bridge given
        r = self.d.exec("wget {}/small.txt /home/root/s.txt".format(self.url))
        self.assertEqual(r.rc, 1)
        self.assertIn("rpc or the browser tool attached", r.text)
        self.assertEqual(self.dev.bridge_frames, [])

    def test_port_closing_ends_the_offer(self):
        self.make()
        self.dev.host_closed(self.dev.host)
        self.assertFalse(self.dev.bridge_offered)


class TestExec(BridgeBase):
    def test_wget_mid_exec(self):
        d = self.make()
        r = d.exec("wget {}/big.bin /home/root/big.bin".format(self.url))
        self.assertEqual(r.rc, 0, r.text)
        self.assertEqual(self.dev.fs.files["/home/root/big.bin"], BIG)
        self.assertEqual(self.dev.bridge_fetches[-1][2:], (200, len(BIG)))
        # The device acknowledged every chunk, and the host waited for each.
        acks = [f for f in self.dev.bridge_frames if f.startswith("FACK ")]
        self.assertEqual(len(acks), (len(BIG) + 767) // 768)
        self.assertTrue(any("bridge: GET {}/big.bin".format(self.url) in x for x in self.log))
        self.assertTrue(any("-> 200" in x for x in self.log), self.log)
        # The link is still in step.
        self.assertEqual(d.exec("echo after").text, "after\n")

    def test_curl_prints_the_body(self):
        d = self.make()
        r = d.exec("curl {}/small.txt".format(self.url))
        self.assertEqual((r.rc, r.text), (0, "hello through the bridge\n"))

    def test_pkg_install_by_name_needs_sudo_and_works(self):
        d = self.make()
        r = d.exec("pkg install greet2")
        self.assertEqual(r.rc, 1)
        self.assertIn("Run it with sudo", r.text)
        self.assertEqual(self.dev.bridge_frames, [], "refused before anything was fetched")
        r = d.exec(d.sudo_line("pkg install greet2"))
        self.assertEqual(r.rc, 0, r.text)
        self.assertEqual(self.dev.installed.get("greet2"), "2.1")
        urls = [f[1] for f in self.dev.bridge_fetches]
        self.assertEqual(urls, [self.url + "/index.json", self.url + "/pkg/greet2.app"])

    def test_redirect_followed_on_the_host(self):
        d = self.make()
        r = d.exec("curl {}/moved".format(self.url))
        self.assertEqual((r.rc, r.text), (0, "hello through the bridge\n"))
        self.assertEqual(len(self.dev.bridge_fetches), 1, "one fetch; the host followed it")

    def test_status_passed_on(self):
        d = self.make()
        r = d.exec("curl {}/nothing-here".format(self.url))
        self.assertEqual(r.rc, 1)
        self.assertIn("HTTP 404", r.text)

    def test_gzip_undone_on_the_host(self):
        d = self.make()
        r = d.exec("curl {}/gz".format(self.url))
        self.assertEqual((r.rc, r.text), (0, "unzipped on the host\n"))

    def test_slow_server_kept_alive(self):
        d = self.make()
        self.dev.bridge_quiet = 2.5           # shorter than the server takes (3 s)
        r = d.exec("curl {}/slow".format(self.url))
        self.assertEqual((r.rc, r.text), (0, "worth the wait\n"))

    def test_request_headers_and_body_reach_the_server(self):
        d = self.make()
        Site.seen[:] = []
        st, body, why = None, None, None

        # Straight through the mock's own fetch, as curl -X POST -H -d would.
        def go():
            return self.dev.bridge_fetch("POST", self.url + "/echo",
                                         headers=["X-Test: yes", "Host: evil.example",
                                                  "Cookie: a=b"], body=b"k=v")
        # Run it as a command would: inside an EXEC, so the host is listening.
        self.dev.c_bridgepost = lambda a: (self.dev.out(repr(go()) + "\n") or 0)
        r = d.exec("bridgepost")
        self.assertIn("posted k=v", r.text)
        post = [s for s in Site.seen if s[0] == "POST"][-1]
        hdrs = {k.lower(): v for k, v in post[2].items()}
        self.assertEqual(hdrs.get("x-test"), "yes")
        self.assertNotIn("evil.example", hdrs.get("host", ""), "the device cannot set Host")
        self.assertNotIn("cookie", hdrs, "nor send cookies")
        self.assertIn("RPCortex network bridge", hdrs.get("user-agent", ""))


class TestPolicy(BridgeBase):
    def test_allowlist_refuses(self):
        d = self.make(_bridge.Policy(allow=["example.com"]))
        r = d.exec("wget {}/small.txt /home/root/s.txt".format(self.url))
        self.assertEqual(r.rc, 1)
        self.assertIn("allowlist", r.text)
        self.assertNotIn("/home/root/s.txt", self.dev.fs.files)
        self.assertTrue(any("refused" in x for x in self.log))

    def test_allowlist_checked_on_every_hop(self):
        d = self.make(_bridge.Policy(allow=["127.0.0.1"]))
        r = d.exec("curl {}/moved".format(self.url))
        self.assertEqual(r.rc, 0, "a redirect within the allowlist is followed")
        r = d.exec("curl {}/moved-away".format(self.url))
        self.assertEqual(r.rc, 1)
        self.assertIn("allowlist", r.text)

    def test_only_http_and_https(self):
        d = self.make()
        for url in ("file:///etc/passwd", "ftp://example.com/x", "data:text/plain,hi"):
            self.assertRaises(_bridge.Refused, self.bridge.policy.check, url)
        r = d.exec("curl {}/to-file".format(self.url))
        self.assertEqual(r.rc, 1, "a redirect to file:// is refused")
        self.assertNotIn("root:", r.text)
        # And the device asking for one outright.
        self.dev.c_bridgefile = lambda a: (self.dev.out(repr(self.dev.bridge_fetch(
            "GET", "file:///etc/passwd")) + "\n") or 0)
        r = d.exec("bridgefile")
        self.assertIn("only http and https", r.text)
        self.assertNotIn("root:", r.text)

    def test_size_cap(self):
        d = self.make(_bridge.Policy(max_bytes=4096))
        r = d.exec("wget {}/big.bin /home/root/big.bin".format(self.url))
        self.assertEqual(r.rc, 1)
        self.assertIn("bridge limit", r.text)
        self.assertEqual(d.exec("echo after").text, "after\n")


class TestStopping(BridgeBase):
    def test_device_stops_mid_body(self):
        d = self.make()
        self.dev.bridge_stop_after = 3
        r = d.exec("wget {}/big.bin /home/root/big.bin".format(self.url))
        self.assertEqual(r.rc, 1)
        self.assertTrue(any(f.startswith("FSTOP") for f in self.dev.bridge_frames))
        self.assertEqual(self.bridge.served[-1][2], "cancelled")
        self.assertEqual(d.exec("echo after").text, "after\n", "still in step afterwards")

    def test_ctrl_c_from_the_host_mid_body(self):
        d = self.make()
        self.dev.bridge_chunk_delay = 0.01
        r = d.exec("wget {}/big.bin /home/root/big.bin".format(self.url), timeout=0.5)
        self.assertTrue(r.timed_out)
        self.assertEqual(r.rc, 1)
        self.assertTrue(any(f.startswith("FSTOP") for f in self.dev.bridge_frames))
        self.assertEqual(d.exec("echo after").text, "after\n")

    def test_host_never_sends_ahead_of_an_ack(self):
        # Hold every FACK back a moment and look for anything already waiting.
        d = self.make()
        seen = {"early": 0}
        real = self.dev.bridge_frame

        def frame(text):
            if text.startswith("FACK "):
                time.sleep(0.01)
                if self.dev.from_host.available():
                    seen["early"] += 1
            real(text)
        self.dev.bridge_frame = frame
        r = d.exec("wget {}/small.txt /home/root/s.txt".format(self.url))
        self.assertEqual(r.rc, 0)
        r = d.exec("wget {}/pkg/greet2.app /home/root/g.app".format(self.url))
        self.assertEqual(r.rc, 0)
        self.assertEqual(seen["early"], 0, "the host sent before the device acknowledged")


class TestTerminal(BridgeBase):
    def _term(self, keys, wait_for, timeout=20.0, policy=None):
        """Run rpc term against the mock: type `keys`, wait until `wait_for`
        returns true, then Ctrl+]."""
        self.dev = MockDevice(proto=3, netless=True).start()
        self.dev.repo_index = self.url + "/index.json"
        self.log = []
        self.bridge = _bridge.Bridge(policy or _bridge.Policy(), log=self.log.append)
        t = support.transport.Transport(self.dev.open(), "mock")
        r, w = os.pipe()
        out = support._Out()
        status = support._Out()
        th = threading.Thread(target=_term.run, args=(t,), kwargs=dict(
            stdin_fd=r, out=out.buffer, status=status, bridge=self.bridge))
        th.daemon = True
        th.start()
        end = time.monotonic() + 5
        while not self.dev.bridge_offered and time.monotonic() < end:
            time.sleep(0.02)
        os.write(w, b"\r")
        time.sleep(0.2)
        for k in keys:
            os.write(w, k)
            time.sleep(0.05)
        end = time.monotonic() + timeout
        while not wait_for() and time.monotonic() < end:
            time.sleep(0.05)
        os.write(w, b"\x1d")
        th.join(5)
        os.close(w)
        os.close(r)
        t.close()
        return out.buffer.getvalue(), status.text()

    def test_wget_at_the_prompt(self):
        shown, notes = self._term(
            [b"wget " + self.url.encode() + b"/small.txt /home/root/s.txt\r"],
            lambda: "/home/root/s.txt" in self.dev.fs.files)
        self.assertEqual(self.dev.fs.files.get("/home/root/s.txt"), b"hello through the bridge\n")
        self.assertIn("network bridge on", notes)
        self.assertNotIn(b"\x1eFETCH", shown, "bridge frames are kept off the screen")
        self.assertNotIn(b"\x1eFACK", shown)
        self.assertNotIn(b"\x1eBRIDGE", shown)
        self.assertIn(b"Saved /home/root/s.txt", shown)

    def test_sudo_pkg_install_just_works(self):
        shown, notes = self._term(
            [b"sudo pkg install greet2\r", b"rootpw\r"],
            lambda: self.dev.installed.get("greet2") == "2.1")
        self.assertEqual(self.dev.installed.get("greet2"), "2.1")
        self.assertIn(b"Installed greet2 2.1", shown)
        self.assertNotIn(b"\x1eF", shown)

    def test_stray_answer_never_reaches_the_prompt(self):
        # A host line arriving with no fetch running is dropped by the door.
        shown, _ = self._term([b"\x1eFDATA 9 QUJD\nls /home\r"],
                              lambda: "ls /home" in self.dev.typed_lines)
        self.assertIn("ls /home", self.dev.typed_lines)
        self.assertFalse(any("FDATA" in l for l in self.dev.typed_lines))


class TestTermPieces(unittest.TestCase):
    def test_frames_split_across_reads(self):
        class W(object):
            fetching = threading.Event()
        tb = _term.TermBridge(_bridge.Bridge(), W(), status=None)
        tb.stop()
        tb.worker.join(1)               # so the frames stay in the queue to look at
        shown = b""
        stream = (b"prompt> \x1eBRIDGE on chunk=768\r\nmore\x1eOK rc=0\nend \x1eFAC",
                  b"K 1 768\ntail\x1eHEL", b"LO proto=3\n")
        for piece in stream:
            shown += tb.feed(piece)
        self.assertEqual(shown, b"prompt> more\x1eOK rc=0\nend tail\x1eHELLO proto=3\n")
        self.assertTrue(tb.offered)
        f = tb.q.get(timeout=1)
        self.assertEqual((f.type, f.rest), ("FACK", "1 768"))

    def test_keys_held_during_a_fetch(self):
        class T(object):
            def __init__(self):
                self.sent = b""

            def write(self, d):
                self.sent += d
        t = T()
        w = _term._Wire(t)
        w.key(b"ls")
        w.fetching.set()
        w.key(b"pw")
        w.key(b"d\x03")
        self.assertEqual(t.sent, b"ls\x03", "only Ctrl+C goes through during a fetch")
        w.release()
        self.assertEqual(t.sent, b"ls\x03pwd", "the rest follows, in order")


class TestCli(BridgeBase):
    def test_flags(self):
        self.dev = MockDevice(proto=3, netless=True).start()
        rc, out, err = support.run_cli(self.dev, ["exec", "curl", self.url + "/small.txt"])
        self.assertEqual(rc, 0, err)
        self.assertEqual(out, b"hello through the bridge\n")
        self.assertIn("[rpc] bridge: GET " + self.url + "/small.txt", err)
        rc, out, err = support.run_cli(self.dev, ["--no-bridge", "exec", "curl", self.url + "/small.txt"])
        self.assertEqual(rc, 1)
        rc, out, err = support.run_cli(self.dev, ["--bridge-allow", "example.com", "exec", "curl",
                                                  self.url + "/small.txt"])
        self.assertEqual(rc, 1)
        self.assertIn("allowlist", out.decode())
        rc, out, err = support.run_cli(self.dev, ["--bridge-max", "1K", "exec", "curl",
                                                  self.url + "/big.bin"])
        self.assertEqual(rc, 1)
        self.assertIn("bridge limit", out.decode())


if __name__ == "__main__":
    unittest.main()
