"""The six screens. Each draws itself in the main box and handles its keys;
anything for the device goes to the worker as a job (App.submit)."""
import datetime
import os
import posixpath
import shutil
import time

from rpctool import backup as _backup
from rpctool import repo as _repo
from rpctool.errors import RpcError
from rpctool.pkg import install_bytes
from rpctool.util import human, quote

from . import firmware as _fw
from . import parse
from .canvas import pad
from .dialogs import Confirm, Form, _wrap
from .termemu import Screen as TermScreen
from .termemu import key_bytes

VIEW_LIMIT = 256 * 1024


def make_all(app):
    return [DeviceScreen(app), FilesScreen(app), PackagesScreen(app), FirmwareScreen(app),
            LogsScreen(app), TerminalScreen(app)]


def bar(c, frac, width):
    frac = max(0.0, min(1.0, frac))
    n = int(round(frac * width))
    return c.glyphs["fill"] * n + c.glyphs["empty"] * (width - n)


class Screen(object):
    name = title = ""
    number = 0
    help = []
    keys = []
    every = None          # seconds between refreshes while the screen is shown

    def __init__(self, app):
        self.app = app
        self.last = 0.0
        self.loaded = False

    # hooks
    def captures_keys(self):
        return False

    def on_state(self, state):
        pass

    def on_connect(self):
        pass

    def on_show(self):
        if not self.loaded and self.app.connected:
            self.refresh()

    def tick(self):
        if not self.loaded or (self.every and time.monotonic() - self.last >= self.every):
            self.refresh(quiet=True)

    def background_tick(self):
        pass

    def refresh(self, quiet=False):
        self.last = time.monotonic()
        self.loaded = True

    def key(self, k):
        return False

    def keys_line(self):
        return self.keys

    def dot(self):
        return "off"

    def render(self, c, y, x, h, w):
        c.box(y, x, h, w, self.title)

    # helpers
    def need_link(self):
        if not self.app.connected:
            self.app.say("The device is not connected.", "err")
            return False
        if self.app.mode != "link":
            self.app.say("This needs link mode, which this firmware does not have.", "err")
            return False
        return True

    def kv(self, c, y, x, w, rows, kw=11):
        for i, (k, v, role) in enumerate(rows):
            c.put(y + i, x, k, "muted")
            c.put(y + i, x + kw, v, role or "text", width=w - kw)


# --- 1 device -------------------------------------------------------------------

class DeviceScreen(Screen):
    name, title, number = "device", "device", 1
    every = 5.0
    WIFI_EVERY = 30.0
    help = [("r", "read everything again"), ("w", "ask the Wi-Fi state now"),
            ("b", "reboot"), ("f", "flash a .uf2 from this computer (BOOTSEL)"),
            ("B", "back up the device to this computer"), ("R", "restore a backup")]
    keys = [("r", "refresh"), ("w", "wifi"), ("b", "reboot"), ("f", "flash .uf2"),
            ("B", "backup"), ("R", "restore")]

    def __init__(self, app):
        Screen.__init__(self, app)
        self.ver = {}
        self.status = {}
        self.wifi = {}
        self.wifi_at = 0.0

    def on_connect(self):
        self.ver = {}
        self.loaded = False
        self.wifi_at = 0.0
        if self.app.mode == "link":
            # Once per connection: what the image is, and the device's own
            # name (the host part of its prompt) for the top bar.
            self.app.submit("reading the version",
                            lambda d, j: (d.exec("ver").text, d.exec("sysinfo").text),
                            ok=self._got_ver, fail=lambda e: None, quiet=True)

    def _got_ver(self, texts):
        ver, sysinfo = texts
        self.ver = parse.parse_ver(ver)
        self.ver.update(parse.parse_sysinfo(sysinfo))
        self.app.device_name = self.ver.get("device_id") or ""

    def dot(self):
        if self.app.connected:
            return "warn" if self.app.mode == "legacy" else "ok"
        return "warn" if self.app.conn in ("connecting", "login", "flashing") else "down"

    def refresh(self, quiet=False):
        Screen.refresh(self, quiet)
        if not self.app.connected or self.app.mode != "link":
            return
        self.app.submit("reading the status", lambda d, j: d.exec(parse.STATUS_LINE).text,
                        ok=self._got_status, fail=(lambda e: None) if quiet else None,
                        quiet=quiet)
        if not quiet or time.monotonic() - self.wifi_at >= self.WIFI_EVERY:
            self.read_wifi(quiet)

    def read_wifi(self, quiet=True):
        # `wifi status` takes the network lock for a live look, so it is asked
        # for every 30 s, not on every refresh.
        self.wifi_at = time.monotonic()
        self.app.submit("reading the Wi-Fi state", lambda d, j: d.exec("wifi status").text,
                        ok=lambda t: setattr(self, "wifi", parse.parse_wifi(t)),
                        fail=(lambda e: None) if quiet else None, quiet=quiet)

    def _got_status(self, text):
        self.status = parse.parse_status(text)

    def key(self, k):
        app = self.app
        if k == "r":
            if self.need_link():
                self.on_connect()
                self.refresh()
                app.say("Reading the device again.")
            return True
        if k == "w":
            if self.need_link():
                self.read_wifi(quiet=False)
            return True
        if k == "b":
            if self.need_link():
                app.confirm("reboot", "Restart the device? The link comes back on its own.",
                            self._reboot)
            return True
        if k == "f":
            self._ask_flash()
            return True
        if k == "B":
            if self.need_link():
                self._ask_backup()
            return True
        if k == "R":
            if self.need_link():
                self._ask_restore()
            return True
        return False

    def _reboot(self):
        def run(dev, job):
            r = dev.reboot()
            if r is not None:
                raise RpcError(r.text.strip() or "the device refused to restart")
        self.app.submit("rebooting", run, expect_gone=True,
                        ok=lambda r: self.app.say("Rebooting; reconnecting when it is back.",
                                                  "ok"))

    # flash a local .uf2

    def _ask_flash(self):
        app = self.app
        guess = ""
        base = app.cwd or os.getcwd()
        try:
            uf2 = sorted((f for f in os.listdir(base) if f.lower().endswith(".uf2")),
                         key=lambda f: -os.path.getmtime(os.path.join(base, f)))
            guess = uf2[0] if uf2 else ""
        except OSError:
            pass

        def got(path):
            if not path:
                return
            path = os.path.join(base, os.path.expanduser(path))
            try:
                with open(path, "rb") as f:
                    data = f.read()
                from rpctool import flash as _flash
                chip = _flash.uf2_chip(data)
            except (OSError, RpcError) as e:
                app.say(str(e), "err")
                return
            app.confirm("flash", "Write {} ({}, for {}) through BOOTSEL? The board restarts into "
                        "its boot ROM, the image is copied on, and it starts again. Its files "
                        "are kept.".format(os.path.basename(path), human(len(data)),
                                           chip or "any chip"),
                        lambda: flash_job(app, data, os.path.basename(path)))
        app.prompt("flash a .uf2", "Firmware image on this computer:", got, default=guess)

    # backup and restore

    def _ask_backup(self):
        app = self.app
        board = app.hello.get("board", "device")
        name = "rpc-backup-{}-{}.zip".format(board, datetime.date.today().isoformat())
        text = ("Everything a person put on the device: /home, /etc, /usb and the rest. "
                "The system files need sudo to read.")

        def done(path, toggles):
            if not path:
                return
            pk, sysf = toggles
            path = os.path.join(app.cwd or os.getcwd(), os.path.expanduser(path))
            self._backup(path, pk, sysf)
        app.open(Form("back up", text, "Save to (a folder, or a name ending .zip):", name,
                      [("the installed packages", False),
                       ("the settings, Wi-Fi networks, accounts, device key, SSH key", False)],
                      done))

    def _backup(self, path, packages, system):
        app = self.app
        warnings = []

        def run(dev, job):
            return _backup.backup(dev, path, packages, warn=warnings.append,
                                  progress=lambda p, sz: (lambda d, t: job.progress(
                                      d, t, posixpath.basename(p))),
                                  with_system=system)

        def ok(r):
            n, size, skipped = r
            lines = ["Backed up {} file{}, {}, to {}.".format(n, "" if n == 1 else "s",
                                                             human(size), path)]
            if skipped:
                lines.append("{} skipped:".format(skipped))
            lines += ["  " + w for w in warnings]
            lines += ["", "Not included:"]
            for what, option in _backup.not_included(packages, system):
                lines.append("  {}{}".format(what, "  (the switch for it: {})".format(option)
                                             if option else ""))
            app.say(lines[0], "warn" if skipped else "ok")
            app.view("backup", "\n".join(lines))
        app.submit("backing up", run, ok=ok, cancellable=True)

    def _ask_restore(self):
        app = self.app

        def got(path):
            if not path:
                return
            path = os.path.join(app.cwd or os.getcwd(), os.path.expanduser(path))
            try:
                sysfiles = _backup.system_paths(path)
            except (RpcError, OSError, ValueError) as e:
                app.say("{}: {}".format(path, e), "err")
                return
            if not sysfiles:
                app.confirm("restore", "Put {} back on the device? Files there with the same "
                            "names are replaced.".format(os.path.basename(path)),
                            lambda: self._restore(path, False))
                return

            def answer(k):
                if k == "a":
                    self._restore(path, True)
                elif k == "f":
                    self._restore(path, False)
            app.open(Confirm("restore with system files",
                             _backup.system_restore_notice(sysfiles), answer,
                             (("a", "everything"), ("f", "files only, leave the system"),
                              ("n", "cancel")), danger=True))
        app.prompt("restore", "Backup to restore (a folder or a .zip):", got)

    def _restore(self, path, system):
        app = self.app
        warnings = []

        def run(dev, job):
            return _backup.restore(dev, path, system=system, confirm=lambda notice: True,
                                   warn=warnings.append,
                                   progress=lambda p, sz: (lambda d, t: job.progress(
                                       d, t, posixpath.basename(p))))

        def ok(r):
            done, failed = r
            text = "Restored {} item{}{}.".format(done, "" if done == 1 else "s",
                                                  ", {} failed".format(failed) if failed else "")
            if warnings:
                app.view("restore", text + "\n\n" + "\n".join(warnings))
            app.say(text + (" The device is restarting." if system and not failed else ""),
                    "warn" if failed else "ok")
        app.submit("restoring", run, ok=ok, cancellable=True)

    def render(self, c, y, x, h, w):
        app = self.app
        hello = app.hello
        top_h = 10
        c.box(y, x, top_h, w, "device", legend_right=app.conn)
        ver = self.ver
        version = ver.get("version") or hello.get("os", "-")
        if ver.get("codename"):
            version += "  ({})".format(ver["codename"])
        build = hello.get("build", "-")
        if ver.get("commit"):
            build += "  commit " + ver["commit"]
        feats = ", ".join(ver.get("features", [])) or "-"
        if ver.get("custom"):
            feats += "  (custom image: {})".format(ver["custom"])
        link = "-"
        if app.mode == "link":
            link = "link protocol {}".format(hello.get("proto", "?"))
        elif app.mode == "legacy":
            link = "legacy (no link mode)"
        rows = [("name", ver.get("device_id", "-"), "accent"),
                ("firmware", version, None),
                ("build", build, None),
                ("board", "{}  {}".format(hello.get("board", "-"), hello.get("arch", "")), None),
                ("features", feats, None),
                ("account", hello.get("user", "-"), None),
                ("port", app.conn_detail if app.connected else "-", None),
                ("link", link, None)]
        if not app.connected:
            rows = [("state", app.conn, "down" if app.conn == "disconnected" else "warn"),
                    ("detail", app.conn_detail or "-", "muted")] + rows[:1]
        self.kv(c, y + 1, x + 2, w - 4, rows[:top_h - 2])

        hy, hh = y + top_h, h - top_h
        c.box(hy, x, hh, w, "health", legend_right="every {:g} s".format(self.every))
        st = self.status
        mem, df = st.get("mem", {}), st.get("df", {})
        bw = max(8, min(24, w - 52))
        rows = []
        if mem.get("total"):
            frac = 1.0 - float(mem.get("free", 0)) / mem["total"]
            rows.append(("memory", "{} {} of {} KB free, largest {} KB".format(
                bar(c, frac, bw), mem.get("free", "?"), mem["total"], mem.get("largest", "?")),
                "warn" if mem.get("largest", 999) < 32 else None))
        else:
            rows.append(("memory", "-", "muted"))
        if df.get("total"):
            frac = float(df.get("pct", 0)) / 100.0
            rows.append(("storage", "{} {} of {} KB free, {}% used".format(
                bar(c, frac, bw), df.get("free", "?"), df["total"], df.get("pct", "?")),
                "warn" if df.get("pct", 0) >= 90 else None))
        else:
            rows.append(("storage", "-", "muted"))
        rows.append(("uptime", parse.human_uptime(st.get("uptime")), None))
        wf = self.wifi
        state = wf.get("state")
        if state == "up":
            wtext = "connected to {}  {}".format(wf.get("network", "?"), wf.get("address", ""))
            role = "ok"
        elif state == "down":
            wtext = "radio on, {}".format(wf.get("link", "not connected"))
            role = "warn"
        elif state == "off":
            wtext, role = "radio off", "muted"
        elif state == "none":
            wtext, role = wf.get("note") or "no Wi-Fi on this board or image", "muted"
        else:
            wtext, role = wf.get("note", "-"), "muted"
        rows.append(("wi-fi", wtext, role))
        if wf.get("signal"):
            rows.append(("signal", wf["signal"], None))
        self.kv(c, hy + 1, x + 2, w - 4, rows[:hh - 2])


def flash_job(app, data, name, verify_note=None):
    """The BOOTSEL flash: the worker lets go of the port (Windows lets only
    one program have it), resets the board into BOOTSEL, waits for its drive,
    copies the image on, and reconnects when the console comes back."""
    flasher = app.flasher or _fw.Flasher()

    def run(dev, job):
        port = job.worker.release_port() if dev is not None else None
        drive = flasher.flash(data, port, name=name, step=job.step, cancelled=job.cancelled)
        return drive

    app.submit("flashing", run, link=False, cancellable=True,
               ok=lambda drive: app.say(_fw.flashed_message(name, drive), "ok"))


# --- 2 files --------------------------------------------------------------------

class Pane(object):
    def __init__(self, label, path):
        self.label = label
        self.path = path
        self.entries = []         # (name, is_dir, size)
        self.sel = 0
        self.top = 0
        self.error = ""

    def current(self):
        if 0 <= self.sel < len(self.entries):
            return self.entries[self.sel]
        return None

    def set_entries(self, ents, keep=None):
        ents = sorted(ents, key=lambda e: (not e[1], e[0].lower()))
        if self.path not in ("/", "") and os.path.dirname(self.path) != self.path:
            ents = [("..", True, 0)] + ents
        self.entries = ents
        self.error = ""
        names = [e[0] for e in ents]
        if keep in names:
            self.sel = names.index(keep)
        self.sel = max(0, min(self.sel, len(ents) - 1))


class FilesScreen(Screen):
    name, title, number = "files", "files", 2
    help = [("Left, Right", "the device or this computer"), ("Up, Down, PgUp, PgDn", "move"),
            ("Enter", "open a folder, view a file"), ("Backspace", "the folder above"),
            ("c", "copy to the other side (SHA-256 checked)"), ("v", "view a text file"),
            ("m", "make a folder"), ("n", "rename"), ("d or Delete", "delete"),
            ("r", "list again")]
    keys = [("←→", "side"), ("Enter", "open"), ("c", "copy"), ("v", "view"),
            ("m", "mkdir"), ("n", "rename"), ("d", "delete")]

    def __init__(self, app):
        Screen.__init__(self, app)
        self.dev = Pane("device", "/")
        self.loc = Pane("computer", os.path.abspath(app.cwd or os.getcwd()))
        self.side = 0
        self.list_local()

    @property
    def pane(self):
        return self.dev if self.side == 0 else self.loc

    def on_connect(self):
        self.dev.path = self.app.hello.get("cwd") or "/"
        self.loaded = False
        if self.app.screen is self:
            self.refresh()

    def tick(self):
        if not self.loaded:
            self.refresh(quiet=True)

    def refresh(self, quiet=False):
        Screen.refresh(self, quiet)
        self.list_local()
        self.list_device(quiet=quiet)

    def list_device(self, keep=None, quiet=False):
        if not self.app.connected or self.app.mode != "link":
            return
        path = self.dev.path

        def ok(ents):
            if path == self.dev.path:
                self.dev.set_entries([(e.name, e.is_dir, e.size) for e in ents], keep)

        def fail(e):
            from .app import describe
            self.dev.error = describe(e)
            self.dev.entries = [("..", True, 0)] if path != "/" else []
        self.app.submit("listing " + path, lambda d, j: d.ls(path), ok=ok, fail=fail,
                        quiet=quiet)

    def list_local(self, keep=None):
        p = self.loc
        try:
            ents = []
            for name in os.listdir(p.path):
                full = os.path.join(p.path, name)
                try:
                    isdir = os.path.isdir(full)
                    size = 0 if isdir else os.path.getsize(full)
                except OSError:
                    isdir, size = False, 0
                ents.append((name, isdir, size))
            p.set_entries(ents, keep)
        except OSError as e:
            p.entries, p.error = [("..", True, 0)], str(e)

    def dot(self):
        return "ok" if self.app.connected and self.loaded else "off"

    def _join(self, pane, name):
        if pane is self.dev:
            if name == "..":
                return posixpath.dirname(pane.path.rstrip("/")) or "/"
            return posixpath.join(pane.path, name)
        if name == "..":
            return os.path.dirname(pane.path)
        return os.path.join(pane.path, name)

    def _open(self, pane, path):
        pane.path = path
        pane.sel = pane.top = 0
        if pane is self.dev:
            self.dev.entries = []
            self.list_device()
        else:
            self.list_local()

    def key(self, k):
        pane = self.pane
        n = len(pane.entries)
        if k in ("left", "h"):
            self.side = 0
        elif k in ("right", "l"):
            self.side = 1
        elif k in ("down", "j"):
            pane.sel = min(n - 1, pane.sel + 1)
        elif k in ("up", "k"):
            pane.sel = max(0, pane.sel - 1)
        elif k == "pgdn":
            pane.sel = min(n - 1, pane.sel + 10)
        elif k == "pgup":
            pane.sel = max(0, pane.sel - 10)
        elif k == "home":
            pane.sel = 0
        elif k == "end":
            pane.sel = max(0, n - 1)
        elif k == "backspace":
            if pane.path not in ("/",):
                self._open(pane, self._join(pane, ".."))
        elif k == "enter":
            e = pane.current()
            if e and e[1]:
                if pane is self.dev and not self.need_link():
                    return True
                self._open(pane, self._join(pane, e[0]))
            elif e:
                self._view(pane, e)
        elif k == "r":
            self.refresh()
        elif k == "v":
            e = pane.current()
            if e and not e[1]:
                self._view(pane, e)
        elif k == "c":
            self._copy()
        elif k == "m":
            self._mkdir()
        elif k == "n":
            self._rename()
        elif k in ("d", "delete"):
            self._delete()
        else:
            return False
        return True

    # actions

    def _view(self, pane, e):
        app = self.app
        name, _, size = e
        path = self._join(pane, name)
        if size > VIEW_LIMIT:
            app.say("{} is {}; the viewer takes up to {}. Copy it instead.".format(
                name, human(size), human(VIEW_LIMIT)), "warn")
            return

        def show(data):
            if b"\x00" in data[:4096]:
                head = " ".join("{:02x}".format(b) for b in bytearray(data[:64]))
                app.view(name, "{}: a binary file, {}.\n\nThe first bytes:\n{}".format(
                    path, human(len(data)), head))
            else:
                app.view(name, data.decode("utf-8", "replace"))
        if pane is self.dev:
            if self.need_link():
                app.submit("reading " + name, lambda d, j: d.get(path, j.progress), ok=show,
                           cancellable=True)
        else:
            try:
                with open(path, "rb") as f:
                    show(f.read(VIEW_LIMIT + 1))
            except OSError as ex:
                app.say(str(ex), "err")

    def _copy(self):
        app = self.app
        src = self.pane
        e = src.current()
        if not e or e[0] == "..":
            return
        if not self.need_link():
            return
        name, isdir, size = e
        dst = self.loc if src is self.dev else self.dev
        spath, dpath = self._join(src, name), self._join(dst, name)
        exists = any(x[0] == name for x in dst.entries)

        if src is self.dev:
            def run(dev, job):
                return _pull(dev, job, spath, dpath, isdir)
        else:
            def run(dev, job):
                return _push(dev, job, spath, dpath, isdir)

        def ok(r):
            files, nbytes = r
            app.say("Copied {} ({} file{}, {}), sha256 verified.".format(
                name, files, "" if files == 1 else "s", human(nbytes)), "ok")
            self.list_local(keep=name)
            self.list_device(keep=name)

        def go():
            app.submit("copying " + name, run, ok=ok, cancellable=True)
        if exists:
            app.confirm("replace", "{} already has {}. Replace it?".format(dst.label, name), go,
                        danger=True)
        else:
            go()

    def _mkdir(self):
        app = self.app
        pane = self.pane
        if pane is self.dev and not self.need_link():
            return

        def got(name):
            if not name:
                return
            path = self._join(pane, name)
            if pane is self.dev:
                app.submit("making " + name, lambda d, j: d.mkdir(path),
                           ok=lambda r: (app.say("Made {}.".format(path), "ok"),
                                         self.list_device(keep=name)))
            else:
                try:
                    os.mkdir(path)
                    app.say("Made {}.".format(path), "ok")
                except OSError as ex:
                    app.say(str(ex), "err")
                self.list_local(keep=name)
        app.prompt("make a folder", "New folder in {}:".format(pane.path), got)

    def _rename(self):
        app = self.app
        pane = self.pane
        e = pane.current()
        if not e or e[0] == "..":
            return
        if pane is self.dev and not self.need_link():
            return
        old = self._join(pane, e[0])

        def got(name):
            if not name or name == e[0]:
                return
            new = self._join(pane, name)
            if pane is self.dev:
                app.submit("renaming " + e[0], lambda d, j: d.mv(old, new),
                           ok=lambda r: (app.say("Renamed to {}.".format(name), "ok"),
                                         self.list_device(keep=name)))
            else:
                try:
                    os.rename(old, new)
                    app.say("Renamed to {}.".format(name), "ok")
                except OSError as ex:
                    app.say(str(ex), "err")
                self.list_local(keep=name)
        app.prompt("rename", "New name for {}:".format(e[0]), got, default=e[0])

    def _delete(self):
        app = self.app
        pane = self.pane
        e = pane.current()
        if not e or e[0] == "..":
            return
        if pane is self.dev and not self.need_link():
            return
        name, isdir, _ = e
        path = self._join(pane, name)
        what = "the folder {} and everything in it".format(path) if isdir else path

        def go():
            if pane is self.dev:
                app.submit("deleting " + name, lambda d, j: d.rm(path, recursive=isdir),
                           ok=lambda r: (app.say("Deleted {}.".format(path), "ok"),
                                         self.list_device()))
            else:
                try:
                    if isdir:
                        shutil.rmtree(path)
                    else:
                        os.remove(path)
                    app.say("Deleted {}.".format(path), "ok")
                except OSError as ex:
                    app.say(str(ex), "err")
                self.list_local()
        app.confirm("delete", "Delete {} on {}? This cannot be undone.".format(what, pane.label),
                    go, danger=True)

    def render(self, c, y, x, h, w):
        lw = w // 2
        for i, (pane, px, pw) in enumerate(((self.dev, x, lw), (self.loc, x + lw, w - lw))):
            active = i == self.side
            label = "{}:{}".format(pane.label, pane.path)
            c.box(y, px, h, pw, label, role="line_hi" if active else "line",
                  legend_role="accent" if active else "legend")
            room = h - 2
            if pane.sel < pane.top:
                pane.top = pane.sel
            if pane.sel >= pane.top + room:
                pane.top = pane.sel - room + 1
            inner = pw - 2
            if pane.error and not pane.entries:
                c.put(y + 1, px + 2, pane.error, "down", width=inner - 2)
            if pane is self.dev and not self.app.connected:
                c.put(y + 1, px + 2, "not connected", "muted")
                continue
            for r, ent in enumerate(pane.entries[pane.top:pane.top + room]):
                idx = pane.top + r
                name, isdir, size = ent
                sel = idx == pane.sel
                size_s = "" if isdir else human(size)
                text = " " + pad(name + ("/" if isdir and name != ".." else ""),
                                 inner - 12) + "{:>10} ".format(size_s)
                role = "accent" if isdir else "text"
                if sel and not active:
                    role = "legend"
                c.put(y + 1 + r, px + 1, text, role, hl=sel and active, width=inner)
            if pane.error and pane.entries:
                c.put(y + h - 1, px + 2, " " + pane.error + " ", "down", width=inner - 2)


def _pull(dev, job, src, dst, isdir):
    """Device to computer. Each file is checked by the library: the device
    sends the SHA-256 of what it read, compared with what arrived."""
    files = nbytes = 0
    if not isdir:
        data = dev.get(src, lambda d, t: job.progress(d, t, posixpath.basename(src)))
        _write(dst, data)
        return 1, len(data)
    for d, ents in dev.walk(src):
        rel = posixpath.relpath(d, src)
        ldir = dst if rel == "." else os.path.join(dst, *rel.split("/"))
        os.makedirs(ldir, exist_ok=True)
        for e in ents:
            if e.is_dir:
                continue
            rp = posixpath.join(d, e.name)
            data = dev.get(rp, lambda a, b: job.progress(a, b, e.name))
            _write(os.path.join(ldir, e.name), data)
            files += 1
            nbytes += len(data)
    return files, nbytes


def _push(dev, job, src, dst, isdir):
    """Computer to device. The device checks each file against the SHA-256
    sent with it and keeps nothing that does not match."""
    if not isdir:
        with open(src, "rb") as f:
            data = f.read()
        dev.put(data, dst, lambda d, t: job.progress(d, t, os.path.basename(src)))
        return 1, len(data)
    files = nbytes = 0
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort()
        rel = os.path.relpath(dirpath, src)
        rdir = dst if rel == "." else posixpath.join(dst, *rel.split(os.sep))
        dev.mkdir(rdir, parents=True)
        for fn in sorted(filenames):
            with open(os.path.join(dirpath, fn), "rb") as f:
                data = f.read()
            dev.put(data, posixpath.join(rdir, fn), lambda a, b: job.progress(a, b, fn))
            files += 1
            nbytes += len(data)
    return files, nbytes


def _write(path, data):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


# --- 3 packages ---------------------------------------------------------------------

class PackagesScreen(Screen):
    name, title, number = "packages", "packages", 3
    help = [("Up, Down", "move"), ("i or Enter", "install, or upgrade, the package"),
            ("u", "upgrade it"), ("U", "upgrade every package with an update"),
            ("d", "remove it"), ("a", "install a .app file from this computer"),
            ("r", "read the list and the repository again")]
    keys = [("i", "install"), ("u", "upgrade"), ("U", "all"), ("d", "remove"),
            ("a", "local .app"), ("r", "refresh")]

    def __init__(self, app):
        Screen.__init__(self, app)
        self.installed = None
        self.index = None
        self.index_error = ""
        self.sel = 0
        self.top = 0

    def on_connect(self):
        self.installed = None
        self.loaded = False

    @property
    def arch(self):
        h = self.app.hello
        if h.get("arch") in ("armv6m", "armv8m"):
            return h["arch"]
        return _repo.board_arch(h.get("board"))

    def tick(self):
        if not self.loaded:
            self.refresh(quiet=True)

    def refresh(self, quiet=False):
        Screen.refresh(self, quiet)
        if self.app.connected and self.app.mode == "link":
            self.app.submit("reading the installed packages", _read_installed,
                            ok=lambda r: setattr(self, "installed", r), quiet=quiet)
        self.index_error = ""

        def bad(e):
            from .app import describe
            self.index_error = describe(e)
        self.app.host(lambda: _repo.load_index(self.app.index_url),
                      ok=lambda r: setattr(self, "index", r), fail=bad)

    def rows(self):
        """(name, installed, repo, state, desc, package) for every package."""
        inst = self.installed or {}
        arch = self.arch
        avail = {}
        for p in self.index or []:
            if arch and not _repo.arch_runs_here(p.arch, arch):
                continue
            key = p.name.lower()
            if key not in avail:
                try:
                    avail[key] = _repo.choose(self.index, p.name, arch)
                except RpcError:
                    continue
        names = {n.lower(): n for n in inst}
        for k, p in avail.items():
            names.setdefault(k, p.name)
        out = []
        for k in sorted(names):
            name = names[k]
            have = next((v for n, v in inst.items() if n.lower() == k), None)
            p = avail.get(k)
            if have is not None and p is not None:
                state = "update" if parse.version_cmp(p.ver, have) > 0 else "installed"
            elif have is not None:
                state = "installed"
            else:
                state = "available"
            out.append((name, have, p.ver if p else None, state, p.desc if p else "", p))
        return out

    def dot(self):
        rows = self.rows() if self.installed is not None else []
        if any(r[3] == "update" for r in rows):
            return "warn"
        return "ok" if self.installed is not None else "off"

    def key(self, k):
        rows = self.rows()
        n = len(rows)
        cur = rows[self.sel] if 0 <= self.sel < n else None
        if k in ("down", "j"):
            self.sel = min(n - 1, self.sel + 1)
        elif k in ("up", "k"):
            self.sel = max(0, self.sel - 1)
        elif k == "pgdn":
            self.sel = min(n - 1, self.sel + 10)
        elif k == "pgup":
            self.sel = max(0, self.sel - 10)
        elif k == "r":
            self.refresh()
        elif k in ("i", "enter", "u"):
            if cur and self.need_link():
                if cur[5] is None:
                    self.app.say("{} is not in the repository; 'a' installs a file."
                                 .format(cur[0]), "warn")
                elif cur[3] == "installed" and k != "i":
                    self.app.say("{} {} is the newest there is.".format(cur[0], cur[1]))
                else:
                    self._install([cur[5]])
        elif k == "U":
            ups = [r[5] for r in rows if r[3] == "update"]
            if not ups:
                self.app.say("Every installed package is up to date.")
            elif self.need_link():
                self.app.confirm("upgrade", "Upgrade {}?".format(", ".join(
                    "{} to {}".format(p.name, p.ver) for p in ups)), lambda: self._install(ups))
        elif k == "d":
            if cur and cur[1] is not None and self.need_link():
                self.app.confirm("remove", "Remove {} {} from the device? (sudo)".format(
                    cur[0], cur[1]), lambda: self._remove(cur[0]), danger=True)
        elif k == "a":
            if self.need_link():
                self._ask_local()
        else:
            return False
        return True

    def _done(self, results):
        bad = [(name, r) for name, r in results if r.rc]
        text = "\n\n".join("{}:\n{}".format(name, r.text.strip()) for name, r in results)
        if bad:
            self.app.view("install", text)
            self.app.say("{} did not install.".format(", ".join(n for n, _ in bad)), "err")
        else:
            last = results[-1][1].text.strip().splitlines() if results else []
            self.app.say(parse.clean(last[-1]) if last else "Installed.", "ok")
        self.refresh(quiet=True)

    def _install(self, pkgs):
        arch = self.arch

        def run(dev, job):
            # `pkg install <file>` does not check the architecture, so this is
            # what keeps ARMv8-M code off an RP2040 (as in `rpc install`).
            if arch is None:
                raise RpcError("the board's architecture is not known; "
                               "install a file with 'a' instead")
            out = []
            for p in pkgs:
                job.step("downloading {} {}".format(p.name, p.ver))
                # Checked against the index's SHA-256 before anything is sent.
                data, verified = _repo.download(
                    p, lambda d, t: job.progress(d, t, "download " + p.name))
                r = install_bytes(dev, data, p.name + ".app",
                                  lambda d, t: job.progress(d, t, "send " + p.name))
                out.append((p.name, r))
            return out
        self.app.submit("installing " + ", ".join(p.name for p in pkgs), run, ok=self._done,
                        cancellable=True)

    def _remove(self, name):
        def run(dev, job):
            return [(name, dev.exec(dev.sudo_line("pkg remove " + quote(name))))]
        self.app.submit("removing " + name, run, ok=self._done)

    def _ask_local(self):
        app = self.app

        def got(path):
            if not path:
                return
            path = os.path.join(app.cwd or os.getcwd(), os.path.expanduser(path))
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError as e:
                app.say(str(e), "err")
                return
            name = os.path.basename(path)
            app.submit("installing " + name,
                       lambda d, j: [(name, install_bytes(d, data, name, j.progress))],
                       ok=self._done, cancellable=True)
        app.prompt("install a file", "Package file (.app) on this computer:", got)

    def render(self, c, y, x, h, w):
        src = self.app.index_url or "the package repository"
        right = "{} for {}".format(len(self.index or []), self.arch or "?")
        c.box(y, x, h, w, "packages", legend_right=right)
        rows = self.rows()
        inner = w - 4
        c.put(y + 1, x + 2, "{:<14} {:<10} {:<10} {:<9} {}".format(
            "name", "installed", "repo", "", "about"), "muted", width=inner)
        room = h - 4
        if self.sel >= len(rows):
            self.sel = max(0, len(rows) - 1)
        if self.sel < self.top:
            self.top = self.sel
        if self.sel >= self.top + room:
            self.top = self.sel - room + 1
        for i, (name, have, ver, state, desc, _) in enumerate(rows[self.top:self.top + room]):
            idx = self.top + i
            sel = idx == self.sel
            line = "{:<14} {:<10} {:<10} ".format(pad(name, 14), pad(have or "-", 10),
                                                  pad(ver or "-", 10))
            c.put(y + 2 + i, x + 1, " " + pad(line, inner), "text", hl=sel, width=inner + 1)
            role = {"update": "warn", "installed": "ok"}.get(state, "muted")
            label = {"update": "update", "installed": "installed", "available": ""}[state]
            c.put(y + 2 + i, x + 2 + len(line), pad(label, 9), role, hl=sel)
            c.put(y + 2 + i, x + 2 + len(line) + 10, desc, "muted", hl=sel,
                  width=inner - len(line) - 10)
        note = ""
        if self.installed is None:
            note = "reading what is installed..." if self.app.connected else "not connected"
        if self.index_error:
            note = "repository: " + self.index_error
        elif self.index is None:
            note = note or "reading {}...".format(src)
        if note:
            c.put(y + h - 2, x + 2, note, "down" if self.index_error else "muted", width=inner)


def _read_installed(dev, job):
    # _pkgs is the machine-readable list, for programs, and needs no admin;
    # firmware without it gets `pkg list`.
    r = dev.exec("_pkgs")
    got = parse.parse_pkgs(r.text)
    if got is None:
        got = parse.parse_pkg_list(dev.exec("pkg list").text)
    return got


# --- 4 firmware -------------------------------------------------------------------

class FirmwareScreen(Screen):
    name, title, number = "firmware", "firmware", 4
    help = [("u", "update over the device's own network (sudo update online)"),
            ("f", "download the .uf2 for this board here and flash it through BOOTSEL"),
            ("r", "read the release list again")]
    keys = [("u", "update online"), ("f", "download + flash .uf2"), ("r", "check again")]

    def __init__(self, app):
        Screen.__init__(self, app)
        self.manifest = None
        self.history = None
        self.error = ""

    def on_show(self):
        if self.manifest is None and not self.error:
            self.refresh()

    def tick(self):
        pass

    def background_tick(self):
        if not self.loaded and self.app.connected:
            self.refresh(quiet=True)

    def refresh(self, quiet=False):
        Screen.refresh(self, quiet)
        self.error = ""
        app = self.app

        def fetch():
            m = _fw.load_json(app.manifest_url or _fw.MANIFEST_URL)
            try:
                hist = _fw.load_json(app.history_url or _fw.HISTORY_URL)
            except RpcError:
                hist = None
            return m, hist

        def ok(r):
            self.manifest, self.history = r

        def bad(e):
            from .app import describe
            self.error = describe(e)
        app.host(fetch, ok=ok, fail=bad)

    @property
    def plan(self):
        return _fw.plan(_fw.Running(self.app.hello), self.manifest)

    def dot(self):
        if not self.app.hello or self.manifest is None:
            return "off"
        p = self.plan
        return "warn" if p.update_available else "ok" if p.state == "current" else "off"

    def key(self, k):
        app = self.app
        if k == "r":
            self.refresh()
            app.say("Reading the release list.")
            return True
        if k == "u":
            if not self.need_link():
                return True
            p = self.plan
            if p.latest is None:
                app.say(p.note or "The release list has not been read.", "warn")
                return True
            ver = _fw.display_ver(p.latest.get("ver"))
            run = _fw.Running(app.hello)
            text = ("Update {} to {}? The device downloads it itself, over its own Wi-Fi, "
                    "checks it against the release's SHA-256, writes it and restarts. Its "
                    "files are kept.".format(run.display, ver))
            force = p.force
            if p.state == "current":
                text = "{} is already the latest. Install it again (--force)?".format(ver)
                force = True
            elif p.state == "newer":
                text = ("This device runs {}, newer than the latest release, {}. Go back to "
                        "the release (--force)?".format(run.display, ver))
                force = True
            elif p.force:
                text += "\n\n" + p.note
            app.confirm("update", text, lambda: self._update(force, ver))
            return True
        if k == "f":
            self._ask_uf2()
            return True
        return False

    def _update(self, force, ver):
        app = self.app

        def run(dev, job):
            job.step("the device is downloading {}".format(ver))
            return dev.exec(_fw.update_line(dev, force))

        def ok(r):
            if r is None:
                app.say("The device is restarting into {}; reconnecting.".format(ver), "ok")
                return
            text = parse.clean(r.text).strip()
            app.view("update", text or "(no output)")
            app.say(text.splitlines()[-1] if text else "update finished",
                    "ok" if r.rc == 0 else "err")
        app.submit("updating to " + ver, run, ok=ok, expect_gone=True, cancellable=True)

    def _ask_uf2(self):
        app = self.app
        p = self.plan
        if p.latest is None:
            app.say(p.note or "The release list has not been read.", "warn")
            return
        url, sha = _fw.uf2_source(p.latest, self.history)
        ver = _fw.display_ver(p.latest.get("ver"))
        text = ("Download {} for {} and flash it through BOOTSEL? The board restarts into its "
                "boot ROM, the image is copied onto its drive, and it starts again. Files are "
                "kept.\n\n{}\n{}".format(ver, p.latest.get("name"), url,
                                          "SHA-256 " + sha if sha else
                                          "The release history publishes no SHA-256 for it; "
                                          "only the UF2 format and the chip are checked."))
        app.confirm("download and flash", text, lambda: self._flash(url, sha, ver))

    def _flash(self, url, sha, ver):
        app = self.app
        flasher = app.flasher or _fw.Flasher()
        name = posixpath.basename(url)

        def run(dev, job):
            data, verified = _fw.download_uf2(
                url, sha, lambda d, t: job.progress(d, t, "download " + name))
            port = job.worker.release_port() if dev is not None else None
            return flasher.flash(data, port, name=name, step=job.step, cancelled=job.cancelled)
        app.submit("flashing " + ver, run, link=False, cancellable=True,
                   ok=lambda drive: app.say(_fw.flashed_message(name, drive), "ok"))

    def render(self, c, y, x, h, w):
        app = self.app
        run = _fw.Running(app.hello) if app.hello else None
        p = self.plan if app.hello else _fw.Plan()
        c.box(y, x, 9, w, "firmware")
        latest = _fw.display_ver(p.latest.get("ver")) if p.latest else "-"
        state = {"update": ("update available", "warn"), "current": ("up to date", "ok"),
                 "newer": ("newer than the latest release", "muted"),
                 "noboard": ("no release for this board", "muted")}.get(
            p.state, ("unknown", "muted"))
        rows = [("running", run.display if run else "-", "accent"),
                ("board", app.hello.get("board", "-"), None),
                ("latest", latest, None),
                ("state", state[0], state[1])]
        if p.force:
            rows.append(("needs", "--force, once (the 2026-11-21 version change)", "warn"))
        if p.latest and p.latest.get("size"):
            rows.append(("size", human(p.latest["size"]), None))
        self.kv(c, y + 1, x + 2, w - 4, rows[:7])
        ny, nh = y + 9, h - 9
        c.box(ny, x, nh, w, "notes")
        lines = []
        if self.error:
            lines.append(("The release list could not be read: " + self.error, "down"))
        elif self.manifest is None:
            lines.append(("Reading the release list...", "muted"))
        if p.note:
            lines += [(ln, "warn" if p.force else "muted") for ln in _wrap(p.note, w - 4)]
        lines += [("", None),
                  ("u  sudo update online: the device fetches and checks the image itself; "
                   "it needs Wi-Fi.", "muted"),
                  ("f  the .uf2 is downloaded here and copied onto the BOOTSEL drive; the "
                   "device needs no network.", "muted")]
        out = []
        for t, role in lines:
            for ln in _wrap(t, w - 4) or [""]:
                out.append((ln, role))
        for i, (ln, role) in enumerate(out[:nh - 2]):
            c.put(ny + 1 + i, x + 2, ln, role or "text", width=w - 4)


# --- 5 logs and tasks -------------------------------------------------------------

class LogsScreen(Screen):
    name, title, number = "logs", "logs + tasks", 5
    every = 3.0
    help = [("Up, Down", "choose a task"), ("x", "stop the task"),
            ("PgUp, PgDn", "scroll the log"), ("r", "read again")]
    keys = [("↑↓", "task"), ("x", "kill"), ("PgUp/PgDn", "log"), ("r", "refresh")]

    def __init__(self, app):
        Screen.__init__(self, app)
        self.tasks = []
        self.summary = ""
        self.log = []
        self.sel = 0
        self.scroll = 0     # lines up from the end

    def on_connect(self):
        self.loaded = False

    def refresh(self, quiet=False):
        Screen.refresh(self, quiet)
        if not self.app.connected or self.app.mode != "link":
            return

        def ok(text):
            self.tasks, self.summary, self.log = parse.parse_logs(text)
        self.app.submit("reading the log and tasks", lambda d, j: d.exec(parse.LOGS_LINE).text,
                        ok=ok, fail=(lambda e: None) if quiet else None, quiet=quiet)

    def dot(self):
        if not self.log:
            return "off"
        return "warn" if any(k == "!" for k, _ in self.log[-10:]) else "ok"

    def key(self, k):
        n = len(self.tasks)
        if k in ("down", "j"):
            self.sel = min(n - 1, self.sel + 1)
        elif k in ("up", "k"):
            self.sel = max(0, self.sel - 1)
        elif k == "pgup":
            self.scroll += 5
        elif k == "pgdn":
            self.scroll = max(0, self.scroll - 5)
        elif k == "r":
            self.refresh()
        elif k == "x":
            if 0 <= self.sel < n and self.need_link():
                t = self.tasks[self.sel]
                self.app.confirm("kill", "Stop task {} ({})? It stops at its next yield."
                                 .format(t.pid, t.name), lambda: self._kill(t), danger=True)
        else:
            return False
        return True

    def _kill(self, t):
        app = self.app

        def ok(r):
            text = parse.clean(r.text).strip()
            app.say(text.splitlines()[-1] if text else "kill {}".format(t.pid),
                    "ok" if r.rc == 0 else "err")
            self.refresh(quiet=True)
        app.submit("stopping {}".format(t.pid), lambda d, j: d.exec("kill {}".format(t.pid)),
                   ok=ok)

    def render(self, c, y, x, h, w):
        th = min(max(6, len(self.tasks) + 4), h // 2)
        c.box(y, x, th, w, "tasks", legend_right=self.summary or None)
        inner = w - 4
        c.put(y + 1, x + 2, "{:<5} {:<14} {:<7} {:<5} {:<11} {}".format(
            "pid", "name", "state", "core", "stack", "cpu"), "muted", width=inner)
        room = th - 3
        if self.sel >= len(self.tasks):
            self.sel = max(0, len(self.tasks) - 1)
        top = max(0, self.sel - room + 1)
        for i, t in enumerate(self.tasks[top:top + room]):
            sel = top + i == self.sel
            line = "{:<5} {:<14} {:<7} {:<5} {:<11} {}".format(
                t.pid, pad(t.name, 14), t.state, t.core, t.stack, t.cpu)
            role = {"run": "ok", "block": "warn", "done": "muted"}.get(t.state, "text")
            c.put(y + 2 + i, x + 1, " " + pad(line, inner), role, hl=sel, width=inner + 1)
        ly, lh = y + th, h - th
        c.box(ly, x, lh, w, "log", legend_right="device lines: {}".format(len(self.app.noise))
              if self.app.noise else None)
        lines = [(k, t) for k, t in self.log] + [("~", "[device] " + n) for n in self.app.noise[-20:]]
        room = lh - 2
        self.scroll = min(self.scroll, max(0, len(lines) - room))
        end = len(lines) - self.scroll
        for i, (kind, text) in enumerate(lines[max(0, end - room):end]):
            role = {"!": "down", "?": "warn", "@": "ok", "~": "legend"}.get(kind, "text")
            c.put(ly + 1 + i, x + 2, text, role, width=inner)
        if not lines:
            c.put(ly + 1, x + 2, "nothing yet" if self.app.connected else "not connected", "muted")


# --- 6 terminal --------------------------------------------------------------------

class TerminalScreen(Screen):
    name, title, number = "terminal", "terminal", 6
    help = [("Enter", "open the device's console here (leaves link mode)"),
            ("Ctrl+]", "back to link mode; anything running at the prompt gets a Ctrl+C")]
    keys = [("Enter", "open the console")]

    def __init__(self, app):
        Screen.__init__(self, app)
        self.emu = TermScreen(16, 58)

    @property
    def active(self):
        return self.app.conn == "terminal"

    def captures_keys(self):
        return self.active

    def keys_line(self):
        if self.active:
            return [("Ctrl+]", "back to link mode"), ("", "every other key goes to the device")]
        return self.keys

    def tick(self):
        pass

    def on_show(self):
        pass

    def feed(self, data):
        self.emu.feed(data)

    def dot(self):
        return "ok" if self.active else "off"

    def key(self, k):
        app = self.app
        if self.active:
            if k == "^]":
                app.worker.leave_terminal()
                app.say("Back to link mode.")
            elif k != "resize":
                data = key_bytes(k)
                if data:
                    app.worker.send_keys(data)
            return True
        if k == "enter":
            if not app.connected:
                app.say("The device is not connected.", "err")
            elif app.worker.busy_job() is not None:
                app.say("Wait for '{}' to finish first.".format(app.worker.busy_job().label),
                        "warn")
            else:
                app.worker.enter_terminal()
                app.say("Console open. Ctrl+] comes back to link mode.")
            return True
        return False

    def render(self, c, y, x, h, w):
        right = "Ctrl+] link mode" if self.active else None
        c.box(y, x, h, w, "terminal", legend_right=right,
              role="line_hi" if self.active else "line")
        rows, cols = h - 2, w - 2
        if (rows, cols) != (self.emu.rows, self.emu.cols):
            self.emu.resize(rows, cols)
        if not self.active:
            msg = ["The device's own console, as if typed at it.", "",
                   "Enter opens it: link mode is left (BYE) and every key goes",
                   "to the device. Ctrl+] comes back to link mode.", ""]
            lines = self.emu.text_lines()
            used = [ln for ln in lines if ln.strip()]
            for i, ln in enumerate(msg):
                c.put(y + 1 + i, x + 2, ln, "muted", width=cols - 2)
            if used:
                c.put(y + 1 + len(msg), x + 2, "last seen:", "legend")
                for i, ln in enumerate(used[-(rows - len(msg) - 2):]):
                    c.put(y + 2 + len(msg) + i, x + 1, ln, "muted", width=cols)
            return
        for i, ln in enumerate(self.emu.text_lines()[:rows]):
            c.put(y + 1 + i, x + 1, ln, "text", width=cols)
        c.show_cursor(y + 1 + self.emu.y, x + 1 + min(self.emu.x, cols - 1))
