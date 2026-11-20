"""The protocol client against the mock device: every verb, framing noise,
flow control, digests, cancelling and timeouts."""
import hashlib
import time
import unittest

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: F401  (sets the import path)
from mockdev import MockDevice
from rpctool.errors import DeviceTimeout, DigestMismatch, LinkError, RpcError
from rpctool.link import FrameReader

UNICODE = "/home/root/naïve file ☃.txt"


class LinkBase(unittest.TestCase):
    mock_args = {}

    def setUp(self):
        self.dev = MockDevice(**self.mock_args).start()
        self.noise = []
        self.d = support.connect(self.dev, on_noise=self.noise.append)
        self.link = self.d.link

    def tearDown(self):
        self.d.close()
        self.dev.shutdown()


class TestHelloAndExec(LinkBase):
    def test_hello_on_entry(self):
        h = self.d.hello
        self.assertEqual(self.d.mode, "link")
        self.assertEqual(h["proto"], "1")
        self.assertEqual(h["board"], "pico2_w")
        self.assertEqual(self.link.chunk, 768)
        self.assertEqual(self.link.exec_max, 32768)
        # entry needs no request, so none was sent before the first one here
        self.assertEqual(self.dev.requests, [])

    def test_hello_request_decodes_cwd(self):
        self.dev.fs.dirs.add("/home/root/my 100% files")
        self.d.exec('cd "/home/root/my 100% files"')
        info = self.link.hello_request()
        self.assertEqual(info["cwd"], "/home/root/my 100% files")

    def test_ping(self):
        self.link.ping()
        self.assertEqual(self.dev.requests[-1], "PING")

    def test_exec_output_and_status(self):
        r = self.d.exec("echo hello")
        self.assertEqual((r.rc, r.text, r.truncated, r.timed_out), (0, "hello\n", False, False))
        r = self.d.exec("fail")
        self.assertEqual(r.rc, 1)
        self.assertIn("[!] it failed", r.text)

    def test_exec_chaining(self):
        r = self.d.exec("fail && echo no || echo yes ; echo last")
        self.assertEqual(r.text, "[!] it failed\nyes\nlast\n")
        self.assertEqual(r.rc, 0)

    def test_exec_unicode(self):
        r = self.d.exec("echo grüße ☃")
        self.assertEqual(r.text, "grüße ☃\n")

    def test_exec_truncated(self):
        r = self.d.exec("big 40000")
        self.assertTrue(r.truncated)
        self.assertEqual(len(r.output), 32768)

    def test_exec_timeout_sends_ctrl_c(self):
        t0 = time.monotonic()
        r = self.d.exec("sleep 10", timeout=0.3)
        self.assertTrue(r.timed_out)
        self.assertEqual(r.rc, 1)
        self.assertLess(time.monotonic() - t0, 3)
        self.assertEqual(self.d.exec("echo still here").text, "still here\n")

    def test_exec_question_given_up(self):
        r = self.d.exec("ask", timeout=0.3)
        self.assertTrue(r.timed_out)
        self.link.ping()

    def test_request_too_long_refused_before_sending(self):
        with self.assertRaises(RpcError):
            self.d.exec("echo " + "x" * 1100)
        with self.assertRaises(RpcError):
            self.d.exec("echo a\nb")
        self.link.ping()

    def test_unknown_verb(self):
        with self.assertRaises(LinkError) as cm:
            self.link.request("FROB x")
        self.assertEqual(cm.exception.code, "badreq")


class TestFiles(LinkBase):
    def roundtrip(self, data, path):
        seen = []
        digest = self.d.put(data, path, lambda a, b: seen.append((a, b)))
        self.assertEqual(digest, hashlib.sha256(data).hexdigest())
        self.assertEqual(self.dev.fs.files[path], data)
        self.assertEqual(seen[-1], (len(data), len(data)))
        self.assertEqual(self.d.get(path), data)

    def test_sizes_around_the_chunk(self):
        for n in (0, 1, 767, 768, 769, 1536, 5000):
            self.roundtrip(bytes(bytearray((i * 31) & 0xFF for i in range(n))), "/tmp/f{}".format(n))

    def test_put_replaces(self):
        self.roundtrip(b"one", "/tmp/r")
        self.roundtrip(b"two two", "/tmp/r")

    def test_unicode_and_spaces(self):
        self.roundtrip("snow ☃\n".encode("utf-8"), UNICODE)
        names = [e.name for e in self.d.ls("/home/root")]
        self.assertIn("naïve file ☃.txt", names)
        self.assertEqual(self.d.stat(UNICODE), ("f", len("snow ☃\n".encode("utf-8"))))

    def test_ls_and_stat(self):
        ents = {e.name: e for e in self.d.ls("/")}
        self.assertTrue(ents["home"].is_dir)
        self.assertEqual(ents["home"].size, 0)
        self.assertEqual(self.d.stat("/home/root/notes.txt"), ("f", 22))
        self.assertEqual(self.d.stat("/home"), ("d", 0))
        self.assertIsNone(self.d.stat("/no/such"))

    def test_errors(self):
        with self.assertRaises(LinkError) as cm:
            self.d.get("/no/such")
        self.assertEqual(cm.exception.code, "nofile")
        with self.assertRaises(LinkError) as cm:
            self.d.get("/home")
        self.assertEqual(cm.exception.code, "nofile")
        with self.assertRaises(LinkError) as cm:
            self.d.ls("/home/root/notes.txt")
        self.assertEqual(cm.exception.code, "nofile")
        with self.assertRaises(LinkError) as cm:
            self.d.put(b"x", "/home")
        self.assertEqual(cm.exception.code, "nofile")
        with self.assertRaises(LinkError) as cm:
            self.d.put(b"x" * 2000000, "/tmp/huge")
        self.assertEqual(cm.exception.code, "nospace")
        with self.assertRaises(LinkError) as cm:
            self.d.put(b"x", "/" + "a" * 300)
        self.assertEqual(cm.exception.code, "toolong")
        self.link.ping()

    def test_device_digest_error_keeps_nothing(self):
        self.dev.corrupt_put = True
        with self.assertRaises(LinkError) as cm:
            self.d.put(b"payload" * 300, "/tmp/bad")
        self.assertEqual(cm.exception.code, "digest")
        self.assertNotIn("/tmp/bad", self.dev.fs.files)
        self.dev.corrupt_put = False
        self.link.ping()

    def test_host_detects_damaged_get(self):
        self.dev.fs.files["/tmp/g"] = b"abc" * 1000
        self.dev.corrupt_get = True
        with self.assertRaises(DigestMismatch):
            self.d.get("/tmp/g")
        self.dev.corrupt_get = False
        self.assertEqual(self.d.get("/tmp/g"), b"abc" * 1000)

    def test_cancel_put_with_ctrl_c(self):
        def interrupt(done, total):
            if done >= 768 * 2:
                raise KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            self.d.put(b"z" * 10000, "/tmp/partial", interrupt)
        self.assertNotIn("/tmp/partial", self.dev.fs.files)
        self.assertEqual(self.dev.cancels, 1)
        self.assertEqual(self.d.exec("echo ok").text, "ok\n")

    def test_cancel_get_with_ctrl_c(self):
        self.dev.fs.files["/tmp/long"] = b"q" * 10000

        def interrupt(done, total):
            if done >= 768:
                raise KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            self.d.get("/tmp/long", interrupt)
        self.assertEqual(self.dev.cancels, 1)
        self.assertEqual(self.d.get("/tmp/long"), b"q" * 10000)

    def test_device_stops_answering(self):
        self.dev.fs.files["/tmp/slow"] = b"s" * 3000
        self.dev.stall_get = 1.0
        self.d.timing.transfer = 0.3
        with self.assertRaises(DeviceTimeout):
            self.d.get("/tmp/slow")
        self.dev.stall_get = 0
        self.d.timing.transfer = 2.0
        self.assertEqual(self.d.exec("echo back").text, "back\n")

    def test_device_side_transfer_timeout(self):
        self.dev.xfer_timeout = 0.3
        data = b"t" * 100
        self.link.send("PUT {} {} /tmp/t".format(len(data), hashlib.sha256(data).hexdigest()))
        self.assertEqual(self.link.frame(2).type, "READY")
        f = self.link.final(3)
        self.assertEqual((f.type, f.rest.split(" ")[0]), ("ERR", "timeout"))
        self.assertNotIn("/tmp/t", self.dev.fs.files)


class TestNoise(LinkBase):
    mock_args = {"noise": 0.5, "partial_noise": True, "seed": 7}

    def test_background_lines_never_reach_the_parser(self):
        data = bytes(bytearray(range(256))) * 30
        self.d.put(data, "/tmp/n")
        self.assertEqual(self.d.get("/tmp/n"), data)
        r = self.d.exec("echo clean")
        self.assertEqual(r.text, "clean\n")
        self.assertTrue(any("background tick" in n for n in self.noise))
        self.assertTrue(any("half a line" in n for n in self.noise))
        self.assertFalse(any("\x1e" in n for n in self.noise))


class TestPlainLF(TestNoise):
    mock_args = {"noise": 0.4, "partial_noise": True, "crlf": False, "seed": 3}


class TestGuest(LinkBase):
    mock_args = {"user": "guest"}

    def test_permissions(self):
        with self.assertRaises(LinkError) as cm:
            self.d.get("/home/root/secret.txt")
        self.assertEqual(cm.exception.code, "perm")
        with self.assertRaises(LinkError) as cm:
            self.d.put(b"x", "/etc/nope")
        self.assertEqual(cm.exception.code, "perm")
        self.d.put(b"mine", "/home/guest/mine.txt")


class TestFrameReader(unittest.TestCase):
    def test_frame_after_partial_line_and_crlf(self):
        class T(object):
            name = "t"

            def __init__(self, chunks):
                self.chunks = list(chunks)

            def read(self, timeout):
                return self.chunks.pop(0) if self.chunks else b""

        noise = []
        r = FrameReader(T([b"[:] svc says hi\r\n", b"half", b" a line\x1eOK n=2\r", b"\n\x1eERR nofile no"]),
                        noise.append)
        f = r.next_frame(0.2)
        self.assertEqual((f.type, f.kv()), ("OK", {"n": "2"}))
        self.assertIsNone(r.next_frame(0.05))      # the ERR line has no newline yet
        r.feed(b"\n")
        f = r.next_frame(0.05)
        self.assertEqual((f.type, f.error().code), ("ERR", "nofile"))
        self.assertEqual(noise, ["[:] svc says hi", "half a line"])


class TestLinkClientAlone(unittest.TestCase):
    def test_link_dropped_then_bye(self):
        dev = MockDevice().start()
        try:
            d = support.connect(dev)
            dev.drop_link = True
            d.exec("echo x")
            self.assertFalse(dev.in_link)
            d.close()          # BYE goes unanswered; a Ctrl+C at the prompt is harmless
            time.sleep(0.1)
            self.assertFalse(dev.in_link)
        finally:
            dev.shutdown()

    def test_resync_drops_stale_frames(self):
        dev = MockDevice().start()
        try:
            d = support.connect(dev)
            c = d.link
            c.send("PING")          # its OK is left unread on purpose
            time.sleep(0.1)
            self.assertTrue(c.resync())
            self.assertEqual(c.exec("echo synced").text, "synced\n")
            d.close()
        finally:
            dev.shutdown()


if __name__ == "__main__":
    unittest.main()
