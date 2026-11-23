"""rpc tui, driven key by key against the mock device.

The app draws on a TextCanvas (an 80x24 grid of text) instead of curses, and
its worker thread talks to the mock through the same rpctool Device the
real thing uses. Nothing here opens a serial port: no_real_ports() refuses.
"""
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import time
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import support  # noqa: E402
from support import mock, run_cli  # noqa: E402
from mockdev import make_app  # noqa: E402
from tui_mock import TuiMock, wait_for  # noqa: E402

from rpctool import flash  # noqa: E402
from rpctool.tui import canvas as tcanvas  # noqa: E402
from rpctool.tui.app import App  # noqa: E402
from rpctool.tui.worker import Worker  # noqa: E402
from rpctool.tui import cursesui, firmware, parse, termemu, themes  # noqa: E402
from rpctool.tui.dialogs import Confirm, Form, Prompt, Viewer  # noqa: E402

PASSWORD = "rootpw"


def make_uf2(family=0xE48BFF59, blocks=4):
    out = bytearray()
    for i in range(blocks):
        blk = bytearray(512)
        struct.pack_into("<IIIIIIII", blk, 0, flash.UF2_MAGIC0, flash.UF2_MAGIC1,
                         flash.UF2_FLAG_FAMILY, 0x10000000 + 256 * i, 256, i, blocks, family)
        struct.pack_into("<I", blk, 508, flash.UF2_MAGIC_END)
        out += blk
    return bytes(out)


class FakeFlasher(firmware.Flasher):
    """The BOOTSEL steps with nothing real: the reset is recorded, a drive
    "appears" after it, and the copy is kept."""

    def __init__(self, drive, dev):
        self.touched = []
        self.copied = []
        self.port_open_at_reset = []

        def touch(path):
            # Only one program may have a COM port open on Windows, so the
            # link must have let go of it before the 1200 baud open.
            self.port_open_at_reset.append(bool(dev.host and dev.host.is_open))
            self.touched.append(path)
        firmware.Flasher.__init__(
            self, touch_1200=touch,
            find_drives=lambda: [drive] if self.touched else [],
            copy_uf2=lambda data, d, name: self.copied.append((d, name, data)),
            drive_chip=lambda d: "RP2350", picotool_load=lambda p: False,
            bootsel_on_usb=lambda: False, sleep=lambda s: time.sleep(0.01))


class TuiCase(unittest.TestCase):
    mock_args = {}
    env_password = PASSWORD
    rows, cols = 24, 80

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="rpctui-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        guard = support.no_real_ports()
        guard.__enter__()
        self.addCleanup(guard.__exit__, None, None, None)
        # The theme is remembered in the config directory: keep it in here.
        os.environ["XDG_CONFIG_HOME"] = os.path.join(self.tmp, "config")
        os.environ["APPDATA"] = os.path.join(self.tmp, "config")
        self.local = os.path.join(self.tmp, "here")
        os.makedirs(self.local)
        self._repo()
        self.dev = TuiMock(gone_after=0.2, **self.mock_args).start()
        self.addCleanup(self.dev.shutdown)
        self.flasher = FakeFlasher(os.path.join(self.tmp, "RP2350"), self.dev)
        self.worker = self.make_worker(self.dev)
        self.app = self.make_app(self.worker)
        self.canvas = tcanvas.TextCanvas(self.rows, self.cols)
        self.frames = []
        self.worker.start()
        self.addCleanup(self._stop)
        if not self.mock_args.get("login"):
            self.assertTrue(self.until(lambda: self.app.conn == "connected"), self.app.msg)
            self.until(lambda: self.worker.idle())

    def make_worker(self, dev):
        w = Worker(dev.opener, timing=support.fast(), env_password=self.env_password)
        w.HEARTBEAT, w.RETRY, w.AFTER_FLASH = 0.5, 0.2, 0.2
        return w

    def make_app(self, worker):
        return App(worker, config_path=os.path.join(self.tmp, "config", "rpc-tui", "config.json"),
                   index_url=os.path.join(self.tmp, "repo", "index.json"),
                   manifest_url=os.path.join(self.tmp, "rel", "latest.json"),
                   history_url=os.path.join(self.tmp, "rel", "releases.json"),
                   flasher=self.flasher, cwd=self.local)

    def _stop(self):
        self.worker.stop()
        self.worker.join(5.0)

    def _repo(self):
        repo = os.path.join(self.tmp, "repo")
        os.makedirs(repo)
        pkgs = []
        for name, ver in (("greet", "1.1"), ("calc", "2.0")):
            data = make_app(name, ver)
            with open(os.path.join(repo, name + ".app"), "wb") as f:
                f.write(data)
            pkgs.append({"name": name, "ver": ver, "desc": name + " things", "url": name + ".app",
                         "sha256": hashlib.sha256(data).hexdigest(), "arch": "armv6m"})
        pkgs.append({"name": "rp2040only", "ver": "1.0", "url": "x.app", "arch": "armv5"})
        with open(os.path.join(repo, "index.json"), "w") as f:
            json.dump({"packages": pkgs}, f)
        rel = os.path.join(self.tmp, "rel")
        os.makedirs(rel)
        self.uf2 = make_uf2()
        with open(os.path.join(rel, "rpcortex-v2-pico2_w.uf2"), "wb") as f:
            f.write(self.uf2)
        self.write_manifest("2.0.700")

    def write_manifest(self, ver, uf2_sha=None):
        rel = os.path.join(self.tmp, "rel")
        with open(os.path.join(rel, "latest.json"), "w") as f:
            json.dump({"format": 2, "packages": [
                {"name": "pico2_w", "ver": ver, "size": 868604, "sha256": "0" * 64,
                 "url": os.path.join(rel, "rpcortex-v2-pico2_w.bin")}]}, f)
        with open(os.path.join(rel, "releases.json"), "w") as f:
            json.dump({"releases": [{"build": int(ver.split(".")[2]), "assets": [
                {"board": "pico2_w", "kind": "uf2",
                 "sha256": uf2_sha or hashlib.sha256(self.uf2).hexdigest()}]}]}, f)

    # --- driving it ---------------------------------------------------------------

    def pump(self):
        self.app.pump()
        self.app.tick()
        self.render()

    def render(self):
        self.app.render(self.canvas)
        text = self.canvas.text()
        self.frames.append(text)
        return text

    def until(self, cond, timeout=8.0):
        return wait_for(cond, timeout, pump=self.pump)

    def idle(self, timeout=8.0):
        self.assertTrue(self.until(lambda: self.worker.idle() and self.app.busy is None
                                   and self.app.events.empty(), timeout))

    def press(self, *keys):
        for k in keys:
            self.app.key(k)
            self.pump()

    def type(self, text, enter=True):
        for ch in text:
            self.app.key(ch)
        if enter:
            self.app.key("enter")
        self.pump()

    def dialog(self, kind=None, timeout=8.0):
        ok = self.until(lambda: self.app.dialogs and (kind is None or
                                                      isinstance(self.app.dialogs[-1], kind)),
                        timeout)
        self.assertTrue(ok, "no {} dialog; message: {}".format(
            kind.__name__ if kind else "", self.app.msg))
        return self.app.dialogs[-1]

    def screen_text(self):
        return self.render()


# --- the frame and the Device screen --------------------------------------------------

class TestDeviceScreen(TuiCase):
    def test_shows_the_device(self):
        self.assertTrue(self.until(lambda: "280 of 480 KB free" in self.render()
                                   and "connected to home" in self.render()))
        text = self.render()
        for want in ("[rpc]", "pico2_w", "v2.0.640-beta", "Vela II", "wifi, bluetooth",
                     "1h 2m", "192.168.1.40", "nav", "1 device", "6 terminal", "status",
                     "largest 180 KB", "link protocol 2", "0% used"):
            self.assertIn(want, text)
        self.assertTrue(self.canvas.line(0).startswith("[rpc] \u25a0 vela \u00b7 pico2_w"),
                        "the device's name and its state lead the top bar")
        self.assertEqual(self.canvas.role_at(0, 6), "ok")
        # Orbit's frame: square single-line boxes, nav on the left.
        self.assertEqual(self.canvas.line(1)[0], "\u250c")
        self.assertIn(" nav ", self.canvas.line(1))

    def test_refresh_does_not_hog_the_link(self):
        self.until(lambda: "280 of 480 KB free" in self.render())
        n = len(self.dev.requests)
        time.sleep(1.0)
        self.pump()
        # Five seconds between refreshes; at most a heartbeat or two in one second.
        self.assertLessEqual(len(self.dev.requests) - n, 2, self.dev.requests[n:])
        # Many refreshes, one Wi-Fi look: `wifi status` takes the network lock.
        self.app.screen.every = 0.2
        time.sleep(0.1)
        self.until(lambda: len([r for r in self.dev.requests if "uptime" in r]) >= 4)
        wifi = [r for r in self.dev.requests if "wifi" in r]
        self.assertEqual(len(wifi), 1, "Wi-Fi is asked for every 30 s, not every refresh")

    def test_reboot_reconnects(self):
        self.press("b")
        self.dialog(Confirm)
        self.press("y")
        self.assertTrue(self.until(lambda: self.dev.reboots == ["reboot"]))
        self.assertTrue(self.until(lambda: self.dev.opens >= 2 and self.app.conn == "connected"))

    def test_flash_a_local_uf2(self):
        with open(os.path.join(self.local, "fw.uf2"), "wb") as f:
            f.write(self.uf2)
        self.press("f")
        d = self.dialog(Prompt)
        self.assertEqual(d.value, "fw.uf2", "the newest .uf2 here is offered")
        self.press("enter")
        self.dialog(Confirm)
        self.assertIn("RP2350", self.render())
        self.press("y")
        self.assertTrue(self.until(lambda: self.flasher.copied))
        self.assertEqual(self.flasher.touched, ["mock"], "reset into BOOTSEL through the port")
        self.assertEqual(self.flasher.port_open_at_reset, [False], "the link let go first")
        self.assertEqual(self.flasher.copied[0][2], self.uf2)
        self.assertTrue(self.until(lambda: self.app.conn == "connected" and self.dev.opens >= 2))

    def test_theme_key_is_remembered(self):
        self.assertEqual(self.app.theme, "tui")
        self.press("t")
        self.assertEqual(self.app.theme, "phosphor")
        self.assertIn("t phosphor", self.render())
        again = self.make_app(self.worker)
        self.assertEqual(again.theme, "phosphor")
        with open(os.path.join(self.tmp, "config", "rpc-tui", "config.json")) as f:
            self.assertEqual(json.load(f)["theme"], "phosphor")

    def test_sizes(self):
        for rows, cols in ((24, 80), (40, 132), (30, 100)):
            self.canvas.resize(rows, cols)
            for i in range(6):
                self.app.go(i)
                text = self.render()
                self.assertIn("status", text)
                self.assertEqual(len(self.canvas.chars), rows)
        self.canvas.resize(20, 60)
        self.assertIn("at least 80x24", self.render())

    def test_help_and_quit(self):
        self.press("?")
        v = self.dialog(Viewer)
        self.assertIn("next theme", "\n".join(v.lines))
        self.press("esc", "q")
        self.assertFalse(self.app.running)


class TestDisconnect(TuiCase):
    def test_an_unplug_is_noticed_with_nothing_to_do(self):
        # The Firmware screen asks the device nothing, so only the heartbeat
        # (a PING every few seconds) can find out.
        self.press("4")
        self.idle()
        self.dev.unplug()
        self.assertTrue(self.until(lambda: self.app.conn == "disconnected", timeout=3.0))
        self.dev.replug()
        self.assertTrue(self.until(lambda: self.app.conn == "connected"))

    def test_unplug_and_replug(self):
        self.dev.unplug()
        self.assertTrue(self.until(lambda: self.app.conn == "disconnected"))
        self.assertIn("disconnected", self.canvas.line(0))
        time.sleep(0.5)
        self.pump()
        self.assertEqual(self.app.conn, "disconnected", "nothing to connect to yet")
        self.dev.replug()
        self.assertTrue(self.until(lambda: self.app.conn == "connected"))
        self.assertIn("v2.0.640-beta", self.render())

    def test_jobs_fail_cleanly_while_away(self):
        self.dev.unplug()
        self.until(lambda: self.app.conn == "disconnected")
        self.press("2", "r")
        self.press("1", "b")
        self.assertIn("not connected", self.app.msg[0])


class TestLogin(TuiCase):
    mock_args = {"login": True}

    def test_logs_in_from_the_screen(self):
        d = self.dialog(Prompt)
        self.assertIn("login prompt", d.label)
        self.type("root")
        d = self.dialog(Prompt)
        self.assertTrue(d.masked)
        self.type(PASSWORD, enter=False)
        self.press("enter")
        self.assertTrue(self.until(lambda: self.app.conn == "connected"))
        self.assertEqual(self.dev.user, "root")
        for frame in self.frames:
            self.assertNotIn(PASSWORD, frame)


# --- Files --------------------------------------------------------------------------

class TestFiles(TuiCase):
    def setUp(self):
        TuiCase.setUp(self)
        self.press("2")
        self.until(lambda: "home/" in self.render())

    def select(self, name):
        pane = self.app.screen.pane
        names = [e[0] for e in pane.entries]
        self.assertIn(name, names)
        pane.sel = names.index(name)
        self.render()

    def into(self, *names):
        for n in names:
            self.select(n)
            self.press("enter")
            self.until(lambda: self.worker.idle() and self.app.screen.pane.entries
                       and self.app.screen.pane.path.endswith(n))
        self.idle()

    def test_copy_both_ways(self):
        self.into("home", "root")
        self.assertIn("device:/home/root", self.render())
        self.select("notes.txt")
        self.press("c")
        self.idle()
        with open(os.path.join(self.local, "notes.txt"), "rb") as f:
            self.assertEqual(f.read(), self.dev.fs.files["/home/root/notes.txt"])
        self.assertIn("sha256 verified", self.app.msg[0])
        with open(os.path.join(self.local, "up.txt"), "wb") as f:
            f.write(b"from the computer\n" * 100)
        self.press("right", "r")
        self.idle()
        self.select("up.txt")
        self.press("c")
        self.idle()
        self.assertEqual(self.dev.fs.files["/home/root/up.txt"], b"from the computer\n" * 100)

    def test_copy_a_folder(self):
        os.makedirs(os.path.join(self.local, "site", "css"))
        for rel, data in (("index.html", b"<p>hi</p>"), ("css/a.css", b"p{}")):
            with open(os.path.join(self.local, "site", *rel.split("/")), "wb") as f:
                f.write(data)
        self.into("home", "root")
        self.press("right", "r")
        self.idle()
        self.select("site")
        self.press("c")
        self.idle()
        self.assertEqual(self.dev.fs.files["/home/root/site/css/a.css"], b"p{}")

    def test_replace_asks_first(self):
        self.into("home", "root")
        with open(os.path.join(self.local, "notes.txt"), "wb") as f:
            f.write(b"mine")
        self.press("right", "r", "left")
        self.idle()
        self.select("notes.txt")
        self.press("c")
        self.dialog(Confirm)
        self.press("n")
        self.idle()
        with open(os.path.join(self.local, "notes.txt"), "rb") as f:
            self.assertEqual(f.read(), b"mine")

    def test_mkdir_rename_view_delete(self):
        self.into("home", "root")
        self.press("m")
        self.dialog(Prompt)
        self.type("made here")
        self.idle()
        self.assertTrue(self.dev.fs.isdir("/home/root/made here"))
        self.select("notes.txt")
        self.press("n")
        self.dialog(Prompt)
        self.app.dialogs[-1].value = ""
        self.type("renamed.txt")
        self.idle()
        self.assertIn("/home/root/renamed.txt", self.dev.fs.files)
        self.select("renamed.txt")
        self.press("v")
        v = self.dialog(Viewer)
        self.assertIn("hello from the device", "\n".join(v.lines))
        self.press("esc")
        self.press("d")
        c = self.dialog(Confirm)
        self.assertIn("cannot be undone", c.text)
        self.press("y")
        self.idle()
        self.assertNotIn("/home/root/renamed.txt", self.dev.fs.files)

    def test_local_side_actions(self):
        self.press("right")
        self.press("m")
        self.dialog(Prompt)
        self.type("box")
        self.assertTrue(os.path.isdir(os.path.join(self.local, "box")))
        self.select("box")
        self.press("d")
        self.dialog(Confirm)
        self.press("y")
        self.assertFalse(os.path.exists(os.path.join(self.local, "box")))

    def test_cancel_a_transfer(self):
        self.dev.fs.files["/home/root/big.bin"] = os.urandom(768 * 20)
        self.dev.stall_get = 1.5
        self.into("home", "root")
        self.select("big.bin")
        self.press("c")
        self.assertTrue(self.until(lambda: self.app.busy is not None))
        self.assertIn("Esc cancels", self.render())
        time.sleep(0.2)
        self.press("esc")
        self.idle()
        self.assertEqual(self.dev.cancels, 1, "CANCEL went to the device")
        self.assertIn("Cancelled", self.app.msg[0])
        self.assertFalse(os.path.exists(os.path.join(self.local, "big.bin")))
        # The link is still in step: the next request answers as it should.
        self.dev.stall_get = 0
        self.select("notes.txt")
        self.press("v")
        v = self.dialog(Viewer)
        self.assertIn("hello from the device", "\n".join(v.lines))


# --- sudo -----------------------------------------------------------------------------

class TestSudoPrompt(TuiCase):
    env_password = None

    def install_local(self):
        path = os.path.join(self.local, "mine.app")
        with open(path, "wb") as f:
            f.write(make_app("mine", "3.1"))
        self.press("3", "a")
        self.dialog(Prompt)
        self.type("mine.app")

    def test_asks_masked_sends_auth_and_retries(self):
        self.install_local()
        d = self.dialog(Prompt)
        self.assertTrue(d.masked)
        self.assertIn("sudo", d.title)
        self.type(PASSWORD, enter=False)
        text = self.render()
        self.assertIn("\u2022" * len(PASSWORD), text)
        self.press("enter")
        self.idle()
        self.assertEqual(self.dev.installed.get("mine"), "3.1")
        self.assertEqual(self.dev.auths, [PASSWORD])
        auth = [r for r in self.dev.requests if r.startswith("AUTH")]
        self.assertEqual(len(auth), 1)
        self.assertEqual(len([r for r in self.dev.requests if "pkg install" in r]), 2,
                         "the install is repeated after AUTH")
        # Never shown, never kept.
        for frame in self.frames:
            self.assertNotIn(PASSWORD, frame)
        self.assertNotIn(PASSWORD, repr(self.app.msg))
        self.assertNotIn(PASSWORD, repr(self.app.noise))
        self.assertNotIn(PASSWORD, repr(vars(self.app)))

    def test_wrong_then_right(self):
        self.install_local()
        self.dialog(Prompt)
        self.type("nope")
        d = self.dialog(Prompt)
        self.assertEqual(d.value, "", "the field is empty again")
        self.type(PASSWORD)
        self.idle()
        self.assertEqual(self.dev.auths, ["nope", PASSWORD])
        self.assertIn("mine", self.dev.installed)

    def test_cancel_the_prompt(self):
        self.install_local()
        self.dialog(Prompt)
        self.press("esc")
        self.idle()
        self.assertNotIn("mine", self.dev.installed)
        self.assertIn("sudo", self.app.msg[0])
        self.assertEqual(self.dev.auths, [])


class TestSudoFromEnvironment(TuiCase):
    def test_rpc_password_is_used_without_asking(self):
        path = os.path.join(self.local, "mine.app")
        with open(path, "wb") as f:
            f.write(make_app("mine", "3.1"))
        self.press("3", "a")
        self.dialog(Prompt)
        self.type("mine.app")
        self.idle()
        self.assertEqual(self.dev.auths, [PASSWORD])
        self.assertFalse(any(isinstance(d, Prompt) for d in self.app.dialogs))


# --- Packages -----------------------------------------------------------------------

class TestPackages(TuiCase):
    def setUp(self):
        TuiCase.setUp(self)
        self.press("3")
        self.assertTrue(self.until(lambda: "update" in self.render() and "calc" in self.render()))

    def row(self, name):
        rows = self.app.screen.rows()
        names = [r[0] for r in rows]
        self.app.screen.sel = names.index(name)
        return rows[names.index(name)]

    def test_lists_installed_beside_the_repository(self):
        text = self.render()
        self.assertIn("greet", text)
        self.assertEqual(self.row("greet")[1:4], ("1.0", "1.1", "update"))
        self.assertEqual(self.row("calc")[1:4], (None, "2.0", "available"))
        self.assertNotIn("rp2040only", text, "a package for another architecture is left out")
        self.assertEqual(self.app.screen.dot(), "warn")
        self.assertTrue(any("_pkgs" in r for r in self.dev.requests))

    def test_upgrade_install_remove(self):
        self.row("greet")
        self.press("u")
        self.idle()
        self.assertEqual(self.dev.installed["greet"], "1.1")
        self.row("calc")
        self.press("i")
        self.idle()
        self.assertEqual(self.dev.installed["calc"], "2.0")
        self.assertTrue(self.until(lambda: self.row("calc")[3] == "installed"))
        self.row("calc")
        self.press("d")
        self.dialog(Confirm)
        self.press("y")
        self.idle()
        self.assertNotIn("calc", self.dev.installed)
        self.assertTrue(any(r == "EXEC sudo pkg remove calc" for r in self.dev.requests))

    def test_upgrade_all(self):
        self.press("U")
        self.dialog(Confirm)
        self.press("y")
        self.idle()
        self.assertEqual(self.dev.installed["greet"], "1.1")


# --- Firmware -----------------------------------------------------------------------

class TestFirmware(TuiCase):
    def setUp(self):
        TuiCase.setUp(self)
        self.press("4")
        self.assertTrue(self.until(lambda: "v2.0.700" in self.render()))

    def test_update_online(self):
        text = self.render()
        self.assertIn("update available", text)
        self.assertNotIn("--force", text)
        self.dev.update_to = ("v2.0.700-beta", "700")
        self.press("u")
        c = self.dialog(Confirm)
        self.assertIn("v2.0.700", c.text)
        self.press("y")
        self.assertTrue(self.until(lambda: self.dev.reboots == ["update"]))
        self.assertIn("EXEC sudo update online --yes", self.dev.requests)
        self.assertTrue(self.until(lambda: self.app.conn == "connected"
                                   and self.app.hello.get("os") == "v2.0.700-beta"))
        self.assertTrue(self.until(lambda: "up to date" in self.render()))

    def test_download_and_flash(self):
        self.press("f")
        c = self.dialog(Confirm)
        self.assertIn("SHA-256 " + hashlib.sha256(self.uf2).hexdigest(), c.text)
        self.press("y")
        self.assertTrue(self.until(lambda: self.flasher.copied))
        self.assertEqual(self.flasher.copied[0][1], "rpcortex-v2-pico2_w.uf2")
        self.assertEqual(self.flasher.touched, ["mock"])
        self.assertEqual(self.flasher.port_open_at_reset, [False], "the link let go first")
        self.assertTrue(self.until(lambda: self.app.conn == "connected"))

    def test_a_bad_download_writes_nothing(self):
        self.write_manifest("2.0.700", uf2_sha="1" * 64)
        self.press("r")
        time.sleep(0.2)
        self.pump()
        self.press("f")
        self.dialog(Confirm)
        self.press("y")
        self.idle()
        self.assertIn("does not match", self.app.msg[0])
        self.assertEqual(self.flasher.copied, [])
        self.assertEqual(self.flasher.touched, [], "the board was not even reset")
        self.assertEqual(self.app.conn, "connected")


class TestFirmwareOldScheme(TuiCase):
    mock_args = {"os_version": "v2.05-beta", "build": "635"}

    def test_needs_force_once_and_says_why(self):
        self.press("4")
        self.assertTrue(self.until(lambda: "v2.0.700" in self.render()))
        text = self.render()
        self.assertIn("v2.05-beta (build 635)", text)
        self.assertIn("update available", text)
        self.assertIn("--force", text)
        self.assertIn("2026-11-21", text)
        self.press("u")
        c = self.dialog(Confirm)
        self.assertIn("--force", c.text)
        self.press("y")
        self.assertTrue(self.until(lambda: self.dev.reboots == ["update"]))
        self.assertIn("EXEC sudo update online --yes --force", self.dev.requests)


# --- Logs and tasks ----------------------------------------------------------------

class TestLogs(TuiCase):
    def test_tasks_log_and_kill(self):
        self.press("5")
        self.assertTrue(self.until(lambda: "blinker" in self.render()))
        text = self.render()
        for want in ("init", "shell", "2.8K/8.0K", "join failed", "3 tasks"):
            self.assertIn(want, text)
        self.assertEqual(self.app.screen.dot(), "warn", "an error in the log")
        self.press("down", "down", "x")
        c = self.dialog(Confirm)
        self.assertIn("blinker", c.text)
        self.press("y")
        self.idle()
        self.assertEqual(self.dev.killed, [7])
        self.assertIn("Asked 'blinker'", self.app.msg[0])

    def test_background_lines_are_shown(self):
        self.dev.noise = 1.0
        self.press("5")
        self.assertTrue(self.until(lambda: "[device]" in self.render()))


# --- Terminal -----------------------------------------------------------------------

class TestTerminal(TuiCase):
    def test_leave_and_return_to_link_mode(self):
        # No heartbeat to fall back on: Ctrl+] itself must bring link mode back.
        self.worker.HEARTBEAT = 60.0
        self.press("6", "enter")
        self.assertTrue(self.until(lambda: self.app.conn == "terminal"))
        self.assertTrue(self.until(lambda: not self.dev.in_link))
        self.assertEqual(self.dev.byes, 1, "left with BYE")
        self.type("echo hello there")
        self.assertTrue(self.until(lambda: "hello there" in "\n".join(
            self.app.screen.emu.text_lines()[1:])))
        shown = self.app.screen.emu.text()
        self.assertIn("Left link mode.", shown)
        self.assertNotIn("BYE", shown, "frames are not shown as console text")
        self.press("1")
        self.assertEqual(self.app.idx, 5, "keys go to the device while it is open")
        self.press("^]")
        self.assertTrue(self.until(lambda: self.app.conn == "connected"))
        self.assertTrue(self.dev.in_link)
        self.assertEqual(self.dev.link_entries, 2)
        self.press("1")
        self.assertEqual(self.app.idx, 0)
        self.press("r")
        self.idle()
        self.assertIn("280 of 480 KB free", self.render())

    def test_keys_as_bytes(self):
        self.press("6", "enter")
        self.until(lambda: self.app.conn == "terminal")
        self.press("^C")
        self.assertTrue(self.until(lambda: "^C" in self.app.screen.emu.text()))


# --- Backup and restore ---------------------------------------------------------------

class TestBackupInTui(TuiCase):
    def backup(self, system):
        self.press("B")
        f = self.dialog(Form)
        f.value = "bk.zip"
        if system:
            self.press("tab", "tab", " ")
        self.press("enter")
        self.idle()
        v = self.dialog(Viewer)
        return "\n".join(v.lines)

    def test_backup_with_system_files(self):
        report = self.backup(True)
        names = zipfile.ZipFile(os.path.join(self.local, "bk.zip")).namelist()
        for want in ("system/os/registry.cfg", "system/os/users.cfg", "system/os/device.key",
                     "system/os/ssh/host_ed25519", "files/home/root/notes.txt"):
            self.assertIn(want, names)
        self.assertFalse(any(n.startswith("files/os/") for n in names))
        self.assertEqual(self.dev.auths, [PASSWORD], "the keys are read with sudo")
        self.assertIn("Not included", report)
        self.assertIn("installed packages", report)
        self.assertIn("Keep it as safe", report)

    def test_backup_says_what_it_left_out(self):
        report = self.backup(False)
        self.assertIn("--with-system", report)
        names = zipfile.ZipFile(os.path.join(self.local, "bk.zip")).namelist()
        self.assertFalse(any(n.startswith("system/") for n in names))

    def test_restore_system_files(self):
        self.backup(True)
        self.press("esc")
        self.dev.fs.files["/os/users.cfg"] = b"changed since\n"
        self.dev.fs.files["/os/registry.cfg"] = b"changed since\n"
        self.press("R")
        self.dialog(Prompt)
        self.type("bk.zip")
        c = self.dialog(Confirm)
        for want in ("/os/users.cfg", "/os/device.key", "/os/ssh/host_ed25519",
                     "different device", "without saving"):
            self.assertIn(want, c.text)
        self.press("a")
        self.assertTrue(self.until(lambda: self.dev.reboots == ["sreboot"]))
        self.assertEqual(self.dev.fs.files["/os/users.cfg"], b"root:salted:hash\n")
        self.assertEqual(self.dev.fs.files["/os/users.bak"], b"root:salted:hash\n")
        self.assertEqual(self.dev.fs.files["/os/registry.bak"], b"System.Setup=true\n")
        self.assertTrue(self.until(lambda: self.app.conn == "connected" and self.dev.opens >= 2))

    def test_restore_files_only(self):
        self.backup(True)
        self.press("esc")
        self.dev.fs.files["/os/users.cfg"] = b"changed since\n"
        self.press("R")
        self.dialog(Prompt)
        self.type("bk.zip")
        self.dialog(Confirm)
        self.press("f")
        self.idle()
        self.assertEqual(self.dev.fs.files["/os/users.cfg"], b"changed since\n")
        self.assertEqual(self.dev.reboots, [])


# --- the parts on their own ------------------------------------------------------------

class TestParsing(unittest.TestCase):
    def test_ver(self):
        v = parse.parse_ver("RPCortex v2.0.640-beta  (Vela II)\nCommit: 299568f  (beta)  [C++ "
                            "native]\nGCC 14.2.1   Platform: pico2_w (armv8m)\nFeatures: wifi, "
                            "bluetooth  (custom image: nobt)\n")
        self.assertEqual(v["version"], "v2.0.640-beta")
        self.assertEqual(v["codename"], "Vela II")
        self.assertEqual((v["board"], v["arch"]), ("pico2_w", "armv8m"))
        self.assertEqual(v["features"], ["wifi", "bluetooth"])
        self.assertEqual(v["custom"], "nobt")
        old = parse.parse_ver("RPCortex v2.01-beta (build 554)\nGCC 14.2.1   Platform: pico2_w "
                              "(RP2350)\n")
        self.assertEqual((old["version"], old["build"]), ("v2.01-beta", "554"))

    def test_status(self):
        text = ("Uptime: 3m 4s\n--rpctui-mem--\n  Total : 480 KB\n  Used  : 512 B  (nothing "
                "allocated right now)\n  Free  : 479 KB\n  Largest block : 20 KB\n"
                "--rpctui-df--\n  Total : 1792 KB\n  Used  : 900 KB  (50%)\n  Free  : 892 KB\n")
        s = parse.parse_status(text)
        self.assertEqual(s["uptime"], 184)
        self.assertEqual(s["mem"], {"total": 480, "free": 479, "largest": 20})
        self.assertEqual(s["df"], {"total": 1792, "used": 900, "free": 892, "pct": 50})
        self.assertEqual(parse.parse_uptime("Uptime: 12s"), 12)
        self.assertEqual(parse.human_uptime(90061), "1d 1h 1m")

    def test_wifi(self):
        self.assertEqual(parse.parse_wifi("  Radio  : \x1b[90moff\x1b[0m\n")["state"], "off")
        self.assertEqual(parse.parse_wifi("This board has no wireless hardware.")["state"], "none")
        up = parse.parse_wifi("  Radio  : on\n  Link   : up\n  Network: home\n  Address: 10.0.0.2\n")
        self.assertEqual((up["state"], up["network"], up["address"]), ("up", "home", "10.0.0.2"))
        self.assertEqual(parse.parse_wifi("  Radio  : on\n  Link   : joining\n")["state"], "down")

    def test_ps_with_colour_codes(self):
        text = ("  PID  NAME         STATE  CORE STACK       CPU      \n  ----\n"
                "  \x1b[36m3   \x1b[0m nova         \x1b[33mblock \x1b[0m 1    2.0K/3.0K   5.1s\n"
                "  \x1b[90m1 task  \u00b7  CPU 3%  \u00b7  10 KB free\x1b[0m\n")
        tasks, summary = parse.parse_ps(text)
        self.assertEqual((tasks[0].pid, tasks[0].name, tasks[0].state, tasks[0].stack),
                         (3, "nova", "block", "2.0K/3.0K"))
        self.assertIn("CPU 3%", summary)

    def test_packages(self):
        self.assertEqual(parse.parse_pkgs("PKGS_BEGIN\nOS:v2.0.640-beta:pico2_w\nPKG:gps:1.2\n"
                                          "PKG:novad1:1.8.0\nPKGS_END\n"),
                         {"gps": "1.2", "novad1": "1.8.0"})
        self.assertIsNone(parse.parse_pkgs("'_pkgs' is not a command"))
        self.assertEqual(parse.parse_pkg_list("[:] Installed packages:\n  \x1b[36mgps             "
                                              "\x1b[0m 1.2     \n"), {"gps": "1.2"})

    def test_log(self):
        lines = parse.parse_log("     0.412  [:] boot ok\n     2.500  [!] bad\n  2 lines\n")
        self.assertEqual(lines, [(":", "0.412  [:] boot ok"), ("!", "2.500  [!] bad")])

    def test_version_cmp_is_repo_version_cmp(self):
        cases = [("2.0.640", "2.0.639", 1), ("2.0.640", "2.05.635", -1), ("2.1.0", "2.0.999", 1),
                 ("1.0", "1.0", 0), ("2.1", "2.1.0", 0), ("1.0-beta", "1.0", 1),
                 ("1.0rc1", "1.0rc2", -1), ("1.10", "1.9", 1), ("", "0", 0)]
        for a, b, want in cases:
            self.assertEqual(parse.version_cmp(a, b), want, (a, b))
            self.assertEqual(parse.version_cmp(b, a), -want, (b, a))


class TestFirmwarePlan(unittest.TestCase):
    def manifest(self, ver):
        return {"packages": [{"name": "pico2_w", "ver": ver, "url": "x/rpcortex-v2-pico2_w.bin"}]}

    def run_(self, os_, build, ver):
        return firmware.plan(firmware.Running({"os": os_, "build": build, "board": "pico2_w"}),
                             self.manifest(ver))

    def test_cases(self):
        p = self.run_("v2.0.640-beta", "640", "2.0.700")
        self.assertEqual((p.state, p.force), ("update", False))
        p = self.run_("v2.0.640-beta", "640", "2.0.640")
        self.assertEqual((p.state, p.force), ("current", False))
        p = self.run_("v2.0.700-beta", "700", "2.0.640")
        self.assertEqual(p.state, "newer")
        for old in ("v2.01-beta", "v2.03-beta", "v2.05-beta"):
            p = self.run_(old, "635", "2.0.640")
            self.assertEqual((p.state, p.force), ("update", True), old)
            self.assertIn("--force", p.note)
        # v2.00-beta compares as 2.00.N, which is older the ordinary way.
        p = self.run_("v2.00-beta", "300", "2.0.640")
        self.assertEqual((p.state, p.force), ("update", False))
        p = firmware.plan(firmware.Running({"os": "v2.0.640", "board": "esp32"}),
                          self.manifest("2.0.700"))
        self.assertEqual(p.state, "noboard")

    def test_uf2_source(self):
        entry = {"name": "pico2_w", "ver": "2.0.640",
                 "url": "https://h/releases/rpcortex-v2-pico2_w.bin"}
        hist = {"releases": [{"build": 700, "assets": []},
                             {"build": 640, "assets": [{"board": "pico2_w", "kind": "uf2",
                                                        "sha256": "AB" * 32}]}]}
        url, sha = firmware.uf2_source(entry, hist)
        self.assertEqual(url, "https://h/releases/rpcortex-v2-pico2_w.uf2")
        self.assertEqual(sha, "ab" * 32)
        self.assertEqual(firmware.uf2_source(entry, None)[1], None)

    def test_picotool_when_no_drive_mounts(self):
        loaded = []

        def picotool(path):
            with open(path, "rb") as f:
                loaded.append((os.path.basename(path), f.read()))
            return True
        f = firmware.Flasher(touch_1200=lambda p: None, find_drives=lambda: [],
                             copy_uf2=lambda *a: self.fail("nothing to copy to"),
                             picotool_load=picotool, bootsel_on_usb=lambda: True,
                             sleep=lambda s: time.sleep(0.01))
        self.assertEqual(f.flash(make_uf2(), "port", name="fw.uf2", wait=0.05), "picotool")
        self.assertEqual(loaded, [("fw.uf2", make_uf2())])
        self.assertIn("picotool", firmware.flashed_message("fw.uf2", "picotool"))

    def test_no_drive_and_no_picotool_says_where_the_board_is(self):
        f = firmware.Flasher(touch_1200=lambda p: None, find_drives=lambda: [],
                             picotool_load=lambda p: False, bootsel_on_usb=lambda: True,
                             sleep=lambda s: time.sleep(0.01))
        with self.assertRaises(Exception) as ctx:
            f.flash(make_uf2(), "port", wait=0.05)
        self.assertIn("is in BOOTSEL", str(ctx.exception))

    def test_flasher_refuses_the_wrong_chip(self):
        copied = []
        f = firmware.Flasher(touch_1200=lambda p: None, find_drives=lambda: ["/media/RPI-RP2"],
                             copy_uf2=lambda *a: copied.append(a), drive_chip=lambda d: "RP2040",
                             sleep=lambda s: None)
        with self.assertRaises(Exception) as ctx:
            f.flash(make_uf2(), "port")
        self.assertIn("RP2350", str(ctx.exception))
        self.assertEqual(copied, [])


class TestThemes(unittest.TestCase):
    def test_every_orbit_theme(self):
        for name in ("tui", "phosphor", "amber", "ember", "frost", "lilac", "mono", "ocean",
                     "paper"):
            self.assertIn(name, themes.THEMES)
            for role in themes.ROLES + ("bg", "raise_", "accent_ink"):
                self.assertRegex(themes.THEMES[name][role], r"^#[0-9a-f]{6}$")
        self.assertEqual(themes.THEMES["tui"]["accent"], "#f0668e")
        self.assertEqual(themes.THEMES["tui"]["ok"], "#23a55a")
        self.assertEqual(themes.next_theme("paper"), "tui")

    def test_colour_approximations(self):
        self.assertEqual(themes.xterm256("#000000"), 16)
        self.assertEqual(themes.xterm256("#ffffff"), 231)
        self.assertEqual(themes.xterm256("#808080"), 244)
        self.assertIn(themes.basic8("#f0668e"), (1, 5), "the pink accent stays a colour")
        self.assertEqual(themes.basic8("#23a55a"), 2)
        self.assertEqual(themes.basic8("#f0b232"), 3)
        self.assertEqual(themes.basic8("#d6d6d8"), 7)
        self.assertEqual(themes.basic8("#1c1c1e"), 0)
        self.assertTrue(themes.is_light("paper"))
        self.assertFalse(themes.is_light("tui"))

    def test_config_dir_per_system(self):
        env = {"HOME": "/h", "APPDATA": "C:\\Users\\u\\AppData\\Roaming", "XDG_CONFIG_HOME": "/x"}
        self.assertEqual(themes.config_dir("linux", env), os.path.join("/x", "rpc-tui"))
        self.assertEqual(themes.config_dir("linux", {"HOME": "/h"}),
                         os.path.join("/h", ".config", "rpc-tui"))
        self.assertEqual(themes.config_dir("darwin", env),
                         os.path.join("/h", "Library", "Application Support", "rpc-tui"))
        self.assertEqual(themes.config_dir("win32", env),
                         os.path.join("C:\\Users\\u\\AppData\\Roaming", "rpc-tui"))

    def test_config_survives_a_bad_file(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        p = os.path.join(d, "c.json")
        with open(p, "w") as f:
            f.write("{not json")
        self.assertEqual(themes.load_config(p), {})
        self.assertTrue(themes.save_config({"theme": "amber"}, p))
        self.assertEqual(themes.load_config(p), {"theme": "amber"})


class FakeCurses(object):
    """Enough of the curses module for Colours, at a chosen depth."""

    A_NORMAL, A_BOLD, A_DIM, A_REVERSE, A_UNDERLINE = 0, 1 << 21, 1 << 20, 1 << 18, 1 << 17

    class error(Exception):
        pass

    def __init__(self, colours, ext=False):
        self.COLORS = colours
        self.COLOR_PAIRS = 256
        self.ext = ext
        self.pairs = {}

    def has_colors(self):
        return self.COLORS > 0

    def has_extended_color_support(self):
        return self.ext

    def start_color(self):
        pass

    def use_default_colors(self):
        pass

    def init_pair(self, n, fg, bg):
        if not self.ext and (fg >= self.COLORS or bg >= self.COLORS):
            raise self.error("colour out of range")
        self.pairs[n] = (fg, bg)

    def color_pair(self, n):
        return n << 8


class TestColourDepths(unittest.TestCase):
    def test_256(self):
        fc = FakeCurses(256)
        col = cursesui.Colours(fc, "tui")
        self.assertEqual(col.depth, "256")
        self.assertIn((themes.xterm256("#f0668e"), themes.xterm256("#1c1c1e")), fc.pairs.values())

    def test_truecolour_when_offered(self):
        fc = FakeCurses(1 << 24, ext=True)
        col = cursesui.Colours(fc, "tui")
        self.assertEqual(col.depth, "true")
        self.assertIn((0xF0668E, 0x1C1C1E), fc.pairs.values())

    def test_eight_colours_use_the_default_background(self):
        fc = FakeCurses(8)
        col = cursesui.Colours(fc, "tui")
        self.assertEqual(col.depth, "8")
        # Every role on the terminal's own background; only the inverted
        # (accent) pair has a background of its own.
        on_default = [p for p in fc.pairs.values() if p[1] == -1]
        self.assertEqual(len(on_default), len(fc.pairs) - 1)
        self.assertTrue(col.attr("muted", False, False) & fc.A_DIM)
        self.assertTrue(col.attr("text", True, False) & fc.A_REVERSE, "selection by reverse")

    def test_no_colour(self):
        fc = FakeCurses(0)
        col = cursesui.Colours(fc, "phosphor")
        self.assertEqual(col.depth, "mono")
        self.assertEqual(fc.pairs, {})
        self.assertTrue(col.attr("accent", False, False) & fc.A_BOLD)
        self.assertTrue(col.attr("text", True, False) & fc.A_REVERSE)


class TestKeysAndTerminal(unittest.TestCase):
    def test_key_bytes(self):
        self.assertEqual(termemu.key_bytes("up"), b"\x1b[A")
        self.assertEqual(termemu.key_bytes("^C"), b"\x03")
        self.assertEqual(termemu.key_bytes("^]"), b"\x1d")
        self.assertEqual(termemu.key_bytes("enter"), b"\r")
        self.assertEqual(termemu.key_bytes("\u00e9"), "\u00e9".encode("utf-8"))
        self.assertEqual(termemu.key_bytes("resize"), b"")

    def test_curses_key_names(self):
        import curses
        b = cursesui.Backend.__new__(cursesui.Backend)
        b.curses = curses
        self.assertEqual(b.name(curses.KEY_UP), "up")
        self.assertEqual(b.name("\x1d"), "^]")
        self.assertEqual(b.name("\x03"), "^C")
        self.assertEqual(b.name("\n"), "enter")
        self.assertEqual(b.name("\x7f"), "backspace")
        self.assertEqual(b.name("x"), "x")

    def test_line_editor_repaint(self):
        s = termemu.Screen(5, 40)
        s.feed(b"root@vela:/> ec")
        s.feed(b"\r\x1b[36mroot@vela:/> \x1b[0mecho\x1b[K")
        s.feed(b"\x1b[")                      # a sequence split across two reads
        s.feed(b"2D\x1b[K\r\nhi\r\n")
        self.assertEqual(s.text_lines()[0].rstrip(), "root@vela:/> ec")
        self.assertEqual(s.text_lines()[1].rstrip(), "hi")
        s.feed("caf\u00e9".encode("utf-8")[:-1])   # half a character
        s.feed("caf\u00e9".encode("utf-8")[-1:])
        self.assertIn("caf\u00e9", s.text())
        s.feed(b"\x1b[2J\x1b[H!")
        self.assertEqual(s.text(), "!")

    def test_windows_without_curses_says_what_to_install(self):
        real_import = __import__

        def no_curses(name, *a, **k):
            if name == "curses":
                raise ImportError("No module named '_curses'")
            return real_import(name, *a, **k)
        with mock.patch("builtins.__import__", no_curses), mock.patch.object(os, "name", "nt"):
            with self.assertRaises(SystemExit) as ctx:
                cursesui.import_curses()
        self.assertIn("pip install windows-curses", str(ctx.exception))

    def test_version_flag(self):
        from rpctool import tui
        out = io.StringIO()
        with mock.patch.object(sys, "stdout", out):
            self.assertEqual(tui.main(["--version"]), 0)
        self.assertEqual(out.getvalue(), tui.__version__ + "\n")
        rc, out, err = run_cli(None, ["tui", "--version"])
        self.assertEqual((rc, out.decode()), (0, tui.__version__ + "\n"))


if __name__ == "__main__":
    unittest.main()
