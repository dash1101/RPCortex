"""Getting into link mode from whatever the console is doing, logging in,
falling back on old firmware, and always leaving a normal prompt behind."""
import time
import unittest

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support
from mockdev import MockDevice
from rpctool.device import classify
from rpctool.errors import (LoginFailed, LoginRequired, NotSupported, RpcError)


class DeviceCase(unittest.TestCase):
    def mock(self, **kw):
        dev = MockDevice(**kw).start()
        self.addCleanup(dev.shutdown)
        return dev

    def wait_prompt(self, dev):
        time.sleep(0.15)
        self.assertFalse(dev.in_link, "the device was left in link mode")


class TestEntering(DeviceCase):
    def test_from_the_prompt_and_back(self):
        dev = self.mock()
        d = support.connect(dev)
        self.assertEqual((d.mode, dev.in_link, dev.link_entries), ("link", True, 1))
        d.close()
        self.wait_prompt(dev)
        self.assertEqual(dev.byes, 1)
        self.assertEqual(dev.typed_lines, ["link"])

    def test_half_typed_line_is_abandoned(self):
        dev = self.mock()
        s = dev.open()
        s.write(b"rm -r /home")              # somebody typed this and walked away
        time.sleep(0.1)
        s.close()
        d = support.connect(dev)
        d.close()
        self.assertEqual(dev.typed_lines, ["link"])
        self.assertIn("/home/root/notes.txt", dev.fs.files)

    def test_left_in_link_mode_by_an_earlier_client(self):
        dev = self.mock()
        first = support.connect(dev)
        first.t.close()                     # crashed: no BYE
        d = support.connect(dev)
        self.assertEqual(d.exec("echo again").text, "again\n")
        d.close()
        self.wait_prompt(dev)

    def test_left_in_the_middle_of_a_transfer(self):
        dev = self.mock()
        dev.fs.files["/tmp/x"] = b"x" * 5000
        first = support.connect(dev)
        first.link.send("GET /tmp/x")       # and then it died, waiting for nobody's A
        time.sleep(0.1)
        first.t.close()
        d = support.connect(dev)
        self.assertEqual(d.get("/tmp/x"), b"x" * 5000)
        d.close()
        self.wait_prompt(dev)

    def test_left_in_the_middle_of_a_command(self):
        dev = self.mock()
        first = support.connect(dev)
        first.link.send("EXEC sleep 30")
        time.sleep(0.1)
        first.t.close()
        d = support.connect(dev)
        self.assertEqual(d.exec("echo fine").text, "fine\n")
        d.close()

    def test_old_firmware(self):
        dev = self.mock(link=False)
        d = support.connect(dev)
        self.assertEqual(d.mode, "legacy")
        r = d.exec("echo old")
        self.assertEqual((r.rc, r.text), (0, "old\n"))
        self.assertEqual(d.exec("fail").rc, 1)
        self.assertEqual(d.exec("nonesuch").rc, 1)
        d.put(b"legacy bytes " * 100, "/home/root/with space.txt")
        self.assertEqual(dev.fs.files["/home/root/with space.txt"], b"legacy bytes " * 100)
        for call in (lambda: d.get("/x"), lambda: d.ls("/"), lambda: d.stat("/")):
            with self.assertRaises(NotSupported):
                call()
        with self.assertRaises(NotSupported):
            d.put(b"", "/tmp/empty")
        with self.assertRaises(NotSupported):
            d.put(b"x", "/tmp/café")
        # The prompt holds 127 characters; past that the marker would be lost.
        with self.assertRaises(NotSupported):
            d.exec("echo " + "x" * 80)
        self.assertEqual(d.exec("echo " + "y" * 60).text, "y" * 60 + "\n")
        d.close()

    def test_old_firmware_digest_refused(self):
        dev = self.mock(link=False)
        dev.corrupt_put = True
        d = support.connect(dev)
        with self.assertRaises(RpcError):
            d.put(b"abc" * 300, "/tmp/bad")
        self.assertNotIn("/tmp/bad", dev.fs.files)
        d.close()


class TestLogin(DeviceCase):
    def test_nothing_typed_without_an_account(self):
        dev = self.mock(login=True)
        with self.assertRaises(LoginRequired):
            support.connect(dev)
        self.assertEqual(dev.login_typed, [])

    def test_login_with_password(self):
        dev = self.mock(login=True)
        asked = []
        d = support.connect(dev, user="root", password=lambda: asked.append(1) or "rootpw")
        self.assertEqual((d.mode, d.hello["user"], asked), ("link", "root", [1]))
        d.close()

    def test_account_without_password_is_not_asked(self):
        dev = self.mock(login=True)
        d = support.connect(dev, user="guest", password=lambda: self.fail("asked for a password"))
        self.assertEqual(d.hello["user"], "guest")
        d.close()

    def test_wrong_password(self):
        dev = self.mock(login=True)
        with self.assertRaises(LoginFailed):
            support.connect(dev, user="root", password="nope")
        self.assertEqual(dev.login_typed, ["root", "<password>"])

    def test_unknown_user(self):
        dev = self.mock(login=True)
        with self.assertRaises(LoginFailed):
            support.connect(dev, user="mallory", password="x")

    def test_already_logged_in_ignores_user(self):
        dev = self.mock()
        d = support.connect(dev, user="root", password="unused")
        self.assertEqual(dev.login_typed, [])
        d.close()


class TestHighLevel(DeviceCase):
    def setUp(self):
        self.dev = self.mock()
        self.d = support.connect(self.dev)
        self.addCleanup(self.d.close)

    def test_mkdir_parents(self):
        self.d.mkdir("/home/root/a/b c/d", parents=True)
        for p in ("/home/root/a", "/home/root/a/b c", "/home/root/a/b c/d"):
            self.assertTrue(self.dev.fs.isdir(p), p)
        self.d.mkdir("/home/root/a/b c/d", parents=True)          # existing is fine
        with self.assertRaises(RpcError):
            self.d.mkdir("/home/root/a")                          # without -p it is not
        with self.assertRaises(RpcError):
            self.d.mkdir("/home/root/notes.txt/x", parents=True)

    def test_rm_and_mv(self):
        self.d.put(b"1", "/tmp/one")
        self.d.mv("/tmp/one", "/tmp/two words")
        self.assertIn("/tmp/two words", self.dev.fs.files)
        self.d.rm("/tmp/two words")
        self.assertNotIn("/tmp/two words", self.dev.fs.files)
        with self.assertRaises(RpcError):
            self.d.rm("/tmp/nothing")
        with self.assertRaises(RpcError):
            self.d.rm("/home/root/*.txt")                         # refused before sending
        self.assertIn("/home/root/notes.txt", self.dev.fs.files)
        self.d.mkdir("/tmp/tree/sub", parents=True)
        self.d.put(b"x", "/tmp/tree/sub/f")
        self.d.rm("/tmp/tree", recursive=True)
        self.assertFalse(self.dev.fs.exists("/tmp/tree"))

    def test_quote_both_kinds_refused(self):
        with self.assertRaises(RpcError):
            self.d.rm("/tmp/it's \"quoted\"")

    def test_walk(self):
        self.d.mkdir("/tmp/w/x", parents=True)
        self.d.put(b"1", "/tmp/w/a")
        self.d.put(b"2", "/tmp/w/x/b")
        seen = {d: sorted(e.name for e in ents) for d, ents in self.d.walk("/tmp/w")}
        self.assertEqual(seen, {"/tmp/w": ["a", "x"], "/tmp/w/x": ["b"]})

    def test_reboot(self):
        gen = self.dev.gen
        self.assertIsNone(self.d.reboot())
        self.assertEqual(self.dev.gen, gen + 1)
        self.assertTrue(self.d.gone)


class TestRebootRefused(DeviceCase):
    def test_guest_cannot_reboot(self):
        dev = self.mock(user="guest")
        d = support.connect(dev)
        r = d.reboot()
        self.assertEqual(r.rc, 1)
        self.assertIn("admin", r.text)
        d.close()
        self.wait_prompt(dev)


class TestClassify(unittest.TestCase):
    def test_states(self):
        self.assertEqual(classify(b"^C\r\n\x1b[36mroot\x1b[90m@vela:/> "), "shell")
        self.assertEqual(classify(b"\x1b[0mUsername \x1b[36m\xe2\x80\xa2\xe2\x80\xa2>  "), "login")
        self.assertEqual(classify(b"Password \xe2\x80\xa2\xe2\x80\xa2>  "), "password")
        self.assertEqual(classify(b"\x1eERR cancelled cancelled by the host\r\n"), "link")
        self.assertEqual(classify(b"\x1eOK\r\nLeft link mode.\r\nroot@vela:/> "), "shell")
        self.assertIsNone(classify(b""))


if __name__ == "__main__":
    unittest.main()
