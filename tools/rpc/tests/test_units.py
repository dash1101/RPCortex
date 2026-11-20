"""The pieces with no device in them: the repository rules, UF2 parsing,
quoting and the terminal's key handling; and the real serial library over a
pseudo-terminal."""
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402
from mockdev import MockDevice  # noqa: E402
from rpctool import flash, repo, term, transport  # noqa: E402
from rpctool.errors import DigestMismatch, RpcError  # noqa: E402
from rpctool.util import quote, rnorm  # noqa: E402

class TestRepoRules(unittest.TestCase):
    def test_arch_runs_here_matches_the_device(self):
        # arch_runs_here() in os/shell/pkgrepo.cpp
        cases = [
            ("", "armv6m", True), ("", "armv8m", True),
            ("armv6m", "armv6m", True), ("armv6m", "armv8m", True),
            ("armv8m", "armv6m", False), ("armv8m", "armv8m", True),
            ("armv8m, xtensa", "armv6m", False), ("xtensa,armv6m", "armv6m", True),
            (" armv8m ,riscv32", "armv8m", True), ("xtensa", "armv8m", False),
        ]
        for arch, mine, want in cases:
            self.assertEqual(repo.arch_runs_here(arch, mine), want, (arch, mine))

    def test_board_arch(self):
        self.assertEqual(repo.board_arch("pico2_w"), "armv8m")
        self.assertEqual(repo.board_arch("pico2"), "armv8m")
        self.assertEqual(repo.board_arch("pico_w"), "armv6m")
        self.assertEqual(repo.board_arch("pico"), "armv6m")
        self.assertIsNone(repo.board_arch("esp32s3"))
        self.assertIsNone(repo.board_arch(None))

    def test_choose(self):
        P = lambda name, arch: repo.Package({"name": name, "url": "u", "arch": arch}, None)  # noqa: E731
        pkgs = [P("Thing", "armv6m"), P("thing", "armv8m"), P("only8", "armv8m")]
        self.assertEqual(repo.choose(pkgs, "THING", "armv8m").arch, "armv8m")
        self.assertEqual(repo.choose(pkgs, "thing", "armv6m").arch, "armv6m")
        with self.assertRaises(RpcError):
            repo.choose(pkgs, "only8", "armv6m")
        with self.assertRaises(RpcError):
            repo.choose(pkgs, "absent", "armv6m")

    def test_real_index_fixtures_parse(self):
        for name in ("index.json", "index-v2.json"):
            path = os.path.join(support.REPO, "os", "host", "fixtures", name)
            pkgs = repo.load_index(path)
            self.assertTrue(pkgs, name)
            self.assertTrue(all(p.name and p.url for p in pkgs))

    def test_download_verifies(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        blob = os.path.join(tmp, "x.app")
        with open(blob, "wb") as f:
            f.write(b"package bytes")
        good = hashlib.sha256(b"package bytes").hexdigest()
        idx = os.path.join(tmp, "index.json")
        with open(idx, "w") as f:
            json.dump({"packages": [{"name": "x", "url": "x.app", "sha256": good},
                                    {"name": "y", "url": "x.app", "sha256": "1" * 64},
                                    {"name": "z", "url": "x.app"}]}, f)
        x, y, z = repo.load_index(idx)
        self.assertEqual(repo.download(x), (b"package bytes", True))
        with self.assertRaises(DigestMismatch):
            repo.download(y)
        self.assertEqual(repo.download(z), (b"package bytes", False))

    def test_url_basename(self):
        self.assertEqual(repo.url_basename("https://h/a/b%20c.txt?x=1"), "b c.txt")
        self.assertEqual(repo.url_basename("https://h/"), "download")

    def test_default_index_matches_the_firmware(self):
        with open(os.path.join(support.REPO, "os", "shell", "pkgrepo.cpp")) as f:
            self.assertIn('"{}"'.format(repo.REPO_DEFAULT), f.read())


def uf2(families, extra_flags=0):
    out = b""
    for i, fam in enumerate(families):
        flags = 0x2000 if fam else 0
        out += struct.pack("<8I", 0x0A324655, 0x9E5D5157, flags | extra_flags, 0x10000000 + 256 * i,
                           256, i, len(families), fam or 0)
        out += b"\0" * 476 + struct.pack("<I", 0x0AB16F30)
    return out


class TestFlash(unittest.TestCase):
    def test_uf2_chip(self):
        self.assertEqual(flash.uf2_chip(uf2([0xE48BFF56] * 3)), "RP2040")
        # an RP2350 image opens with one absolute-family block the ROM ignores
        self.assertEqual(flash.uf2_chip(uf2([0xE48BFF57, 0xE48BFF59, 0xE48BFF59])), "RP2350")
        self.assertIsNone(flash.uf2_chip(uf2([0, 0])))
        with self.assertRaises(RpcError):
            flash.uf2_chip(uf2([0xE48BFF56, 0xE48BFF59]))
        with self.assertRaises(RpcError):
            flash.uf2_chip(b"\0" * 512)
        with self.assertRaises(RpcError):
            flash.uf2_chip(b"short")

    def test_real_images_if_built(self):
        out = os.path.join(support.REPO, "out")
        for board, chip in (("pico2_w", "RP2350"), ("pico", "RP2040")):
            p = os.path.join(out, "rpcortex-v2-{}.uf2".format(board))
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    self.assertEqual(flash.uf2_chip(f.read()), chip)

    def test_drive_chip_and_copy(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        with open(os.path.join(tmp, "INFO_UF2.TXT"), "w") as f:
            f.write("UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RPI-RP2\n")
        self.assertEqual(flash.drive_chip(tmp), "RP2040")
        self.assertIsNone(flash.drive_chip(os.path.join(tmp, "nope")))
        flash.copy_uf2(b"x" * 1000, tmp, "a.uf2")
        with open(os.path.join(tmp, "a.uf2"), "rb") as f:
            self.assertEqual(f.read(), b"x" * 1000)
        self.assertEqual(flash.wait_for_drive(1, finder=lambda: [tmp]), tmp)
        self.assertIsNone(flash.wait_for_drive(0.1, poll=0.05, finder=lambda: []))

    def test_picotool_absent(self):
        self.assertFalse(flash.picotool_load("x.uf2", which=lambda n: None))


class TestQuote(unittest.TestCase):
    def test_quote(self):
        self.assertEqual(quote("/home/root/a.txt"), "/home/root/a.txt")
        self.assertEqual(quote("a b"), '"a b"')
        self.assertEqual(quote("x;rm -r /"), '"x;rm -r /"')
        self.assertEqual(quote('say "hi"'), "'say \"hi\"'")
        self.assertEqual(quote(""), '""')
        self.assertEqual(quote("café"), '"café"')
        with self.assertRaises(RpcError):
            quote("it's \"both\"")

    def test_quoted_words_split_back(self):
        for word in ("a b", "x;y", "p|q", "a&&b", "o>f", "it's", 'say "hi"', "café ☃"):
            line = "cmd " + quote(word)
            segs = MockDevice.split_connectors(line)
            self.assertEqual(len(segs), 1, line)
            self.assertEqual(MockDevice.split_args(segs[0][1]), ["cmd", word])

    def test_rnorm(self):
        self.assertEqual(rnorm("a/../b", "/home"), "/home/b")
        self.assertEqual(rnorm("/x//y/", "/"), "/x/y")
        self.assertEqual(rnorm("", "/tmp"), "/tmp")


@unittest.skipUnless(hasattr(os, "openpty") and os.name == "posix", "needs a pseudo-terminal")
class TestTerminal(unittest.TestCase):
    def test_keys_through_and_ctrl_bracket_quits(self):
        import io
        dev = MockDevice().start()
        self.addCleanup(dev.shutdown)
        master, slave = os.openpty()
        self.addCleanup(os.close, master)
        self.addCleanup(os.close, slave)
        out = io.BytesIO()
        status = io.StringIO()
        t = transport.Transport(dev.open(), "mock")

        def typist():
            time.sleep(0.2)
            os.write(master, b"echo typed\r")
            time.sleep(0.4)
            os.write(master, b"\x1d")
        threading.Thread(target=typist).start()
        term.run(t, stdin_fd=slave, out=out, status=status)
        self.assertIn("echo typed", dev.typed_lines)
        self.assertIn(b"typed\r\n", out.getvalue())
        self.assertIn("Ctrl+] to quit", status.getvalue())

    def test_windows_keys(self):
        self.assertEqual(term.windows_key("\xe0", "H"), b"\x1b[A")
        self.assertEqual(term.windows_key("\x00", "S"), b"\x1b[3~")
        self.assertEqual(term.windows_key("\xe0", "t"), b"\x1b[1;5C")
        self.assertEqual(term.windows_key("\xe0", "?"), b"")


@unittest.skipUnless(hasattr(os, "openpty") and os.name == "posix", "needs a pseudo-terminal")
class TestRealSerialOverPty(unittest.TestCase):
    """The real transport, through real pyserial, against the mock on a pty."""

    def setUp(self):
        try:
            import serial  # noqa: F401
        except ImportError:
            self.skipTest("pyserial is not installed")
        from mockdev import PtyMock
        self.dev = MockDevice(noise=0.2).start()
        self.addCleanup(self.dev.shutdown)
        self.pty = PtyMock(self.dev).start()
        self.addCleanup(self.pty.close)

    def test_link_over_a_real_port(self):
        from rpctool import Device
        s = transport.open_serial(self.pty.path)
        d = Device(transport.Transport(s, self.pty.path), timing=support.fast()).connect()
        try:
            self.assertEqual(d.mode, "link")
            data = os.urandom(20000)
            d.put(data, "/tmp/over pty ☃")
            self.assertEqual(d.get("/tmp/over pty ☃"), data)
            self.assertEqual(d.exec("echo via pty").text, "via pty\n")
        finally:
            d.close()
        time.sleep(0.1)
        self.assertFalse(self.dev.in_link)


if __name__ == "__main__":
    unittest.main()
