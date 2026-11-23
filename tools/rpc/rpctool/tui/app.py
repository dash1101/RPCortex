"""The application: the screens, what each key does, and the frame around them.

Nothing in here touches the port or curses. Work for the device goes to the
Worker as a Job; what comes back is read from the event queue in pump(). The
frame is drawn on a Canvas, which is curses when it runs and a grid of text
when it is tested, so every screen and every key can be driven from a test.

The layout follows Orbit: a top bar with the device and its state, a "nav"
box listing the screens with a status square each, the screen in the main
box, and a "status" box at the bottom with the message and the keys.
"""
import queue
import threading
import time

from rpctool.errors import LinkError, RpcError

from . import themes
from .dialogs import Choose, Confirm, Form, Prompt, Viewer  # noqa: F401  (screens use them)
from .worker import Cancelled, Job

MIN_ROWS, MIN_COLS = 24, 80
NAV_W = 18


def describe(exc):
    """An error, said for a person."""
    if isinstance(exc, Cancelled):
        return "Cancelled."
    if isinstance(exc, LinkError):
        return "{} [{}]".format(exc.message or exc.code, exc.code)
    return str(exc) or type(exc).__name__


class App(object):
    def __init__(self, worker, theme=None, config_path=None, index_url=None,
                 manifest_url=None, history_url=None, flasher=None, cwd=None):
        from . import screens
        self.worker = worker
        self.events = worker.events
        self.config_path = config_path
        self.config = themes.load_config(config_path)
        self.theme = theme or self.config.get("theme") or themes.DEFAULT
        if self.theme not in themes.THEMES:
            self.theme = themes.DEFAULT
        self.index_url = index_url
        self.manifest_url = manifest_url
        self.history_url = history_url
        self.flasher = flasher
        self.cwd = cwd
        self.hello = {}
        self.device_name = ""         # sysinfo's Device ID, the host part of the prompt
        self.mode = None
        self.conn = "connecting"
        self.conn_detail = ""
        self.msg = ("Connecting...", "info")
        self.busy = None
        self.progress = None          # (label, done, total)
        self.step = ""
        self.dialogs = []
        self.noise = []
        self.running = True
        self.dirty = True
        self.on_theme = None          # set by the curses layer
        self.screens = screens.make_all(self)
        self.idx = 0
        self._started = time.monotonic()

    # --- for the screens ---------------------------------------------------------

    @property
    def connected(self):
        return self.conn in ("connected", "terminal")

    @property
    def screen(self):
        return self.screens[self.idx]

    def submit(self, label, fn, ok=None, fail=None, cancellable=False, quiet=False,
               expect_gone=False, link=True):
        job = Job(label, fn, ok=ok, fail=fail, cancellable=cancellable, quiet=quiet,
                  expect_gone=expect_gone, link=link)
        return self.worker.submit(job)

    def host(self, fn, ok=None, fail=None):
        """Work for this computer, not the device (fetching an index): its own
        thread, so the link is not held while it waits on the network."""
        def run():
            try:
                r = fn()
            except Exception as e:  # reported, never raised into the thread
                self.events.put(("hostfail", fail, e))
            else:
                self.events.put(("hostdone", ok, r))
        t = threading.Thread(target=run, name="rpc-host")
        t.daemon = True
        t.start()
        return t

    def say(self, text, kind="info"):
        self.msg = (text, kind)
        self.dirty = True

    def open(self, dialog):
        self.dialogs.append(dialog)
        self.dirty = True
        return dialog

    def confirm(self, title, text, on_yes, danger=False, yes="y", yes_label="yes"):
        def done(k):
            if k == yes:
                on_yes()
        return self.open(Confirm(title, text, done, ((yes, yes_label), ("n", "no")), danger=danger))

    def prompt(self, title, label, done, default="", masked=False, hint=None):
        return self.open(Prompt(title, label, done, default, masked, hint))

    def view(self, title, text):
        return self.open(Viewer(title, text))

    # --- events from the worker ----------------------------------------------------

    def pump(self, wait=0.0):
        """Handle everything the worker has said. True if anything changed."""
        changed = False
        deadline = time.monotonic() + wait
        while True:
            try:
                left = deadline - time.monotonic()
                ev = self.events.get(timeout=left) if left > 0 else self.events.get_nowait()
            except queue.Empty:
                break
            self._event(ev)
            changed = True
        if changed:
            self.dirty = True
        return changed

    def _event(self, ev):
        kind = ev[0]
        if kind == "state":
            was = self.conn
            self.conn, self.conn_detail = ev[1], ev[2]
            if self.conn == "connected" and was != "connected":
                self.say("Connected on {}.".format(ev[2]), "ok")
            elif self.conn == "disconnected":
                self.say("Disconnected: {}. Trying again.".format(ev[2]) if ev[2]
                         else "Disconnected. Trying again.", "err")
            for s in self.screens:
                s.on_state(self.conn)
        elif kind == "hello":
            self.hello, self.mode = ev[1], ev[2]
            for s in self.screens:
                s.on_connect()
            if self.mode == "legacy":
                self.say("This firmware has no link mode. Firmware (4) can flash a new one.",
                         "warn")
        elif kind == "start":
            job = ev[1]
            if not job.quiet:
                self.busy = job
                self.progress = None
                self.step = ""
        elif kind == "progress":
            job, label, done, total = ev[1:]
            if job is self.busy:
                self.progress = (label, done, total)
        elif kind == "step":
            if ev[1] is self.busy:
                self.step = ev[2]
                self.say(ev[2])
        elif kind in ("done", "fail"):
            job = ev[1]
            if job is self.busy:
                self.busy = None
                self.progress = None
                self.step = ""
            if kind == "done":
                if job.ok:
                    self._call(job.ok, ev[2])
                elif not job.quiet:
                    self.say("{}: done.".format(job.label), "ok")
            else:
                if job.fail:
                    self._call(job.fail, ev[2])
                elif not job.quiet or not isinstance(ev[2], Cancelled):
                    self.say("{}: {}".format(job.label, describe(ev[2])), "err")
        elif kind == "hostdone":
            if ev[1]:
                self._call(ev[1], ev[2])
        elif kind == "hostfail":
            if ev[1]:
                self._call(ev[1], ev[2])
            else:
                self.say(describe(ev[2]), "err")
        elif kind == "ask":
            self._ask(*ev[1:])
        elif kind == "login":
            self._login(ev[1], ev[2])
        elif kind == "ports":
            self._choose_port(ev[1])
        elif kind == "noise":
            self.noise.append(ev[1])
            del self.noise[:-200]
        elif kind == "term":
            self.screens_by_name["terminal"].feed(ev[1])

    def _call(self, fn, arg):
        try:
            fn(arg)
        except RpcError as e:
            self.say(describe(e), "err")

    @property
    def screens_by_name(self):
        return {s.name: s for s in self.screens}

    def _ask(self, kind, text, masked, default, reply):
        if kind == "sudo":
            hint = ("The device asks for the signed-in account's password before it changes "
                    "the system. It goes to the device with AUTH and is not kept here.")
            self.prompt("sudo", text, reply, masked=True, hint=hint)
        else:
            self.prompt(kind, text, reply, default=default, masked=masked)

    def _login(self, why, user):
        def got_user(name):
            if not name:
                self.say("Not logged in. Log in at the Terminal (6), or press l to try again.",
                         "warn")
                return

            def got_pw(pw):
                self.worker.set_login(name, pw or "")
            self.prompt("log in", "Password for {} (leave empty for an account with none)"
                        .format(name), got_pw, masked=True)
        self.prompt("log in", "{}\nThe device is at its login prompt. Account:".format(why),
                    got_user, default=user or "")

    def _choose_port(self, ports):
        labels = [p.label() if hasattr(p, "label") else str(p) for p in ports]

        def done(i):
            if i is None:
                self.say("No device chosen.", "warn")
                return
            p = ports[i]
            self.worker.set_port(getattr(p, "device", p))
        self.open(Choose("choose a device", "Several devices are connected.", labels, done))

    # --- keys ---------------------------------------------------------------------

    def key(self, k):
        self.dirty = True
        if k == "resize":
            return
        if self.dialogs:
            d = self.dialogs[-1]
            if d.key(k):
                if d in self.dialogs:
                    self.dialogs.remove(d)
            return
        scr = self.screen
        if scr.captures_keys():
            scr.key(k)
            return
        if k == "esc":
            if self.worker.cancel():
                self.say("Cancelling...", "warn")
            return
        if scr.key(k):
            return
        if k in ("q", "^C"):
            self.quit()
        elif k in "123456" and len(k) == 1:
            self.go(int(k) - 1)
        elif k == "tab":
            self.go((self.idx + 1) % len(self.screens))
        elif k == "btab":
            self.go((self.idx - 1) % len(self.screens))
        elif k == "t":
            self.set_theme(themes.next_theme(self.theme))
        elif k == "?":
            self.view("help", self.help_text())
        elif k == "l" and self.conn == "login":
            self.worker.retry_now()

    def go(self, i):
        if i != self.idx:
            self.idx = i
            self.screen.on_show()

    def set_theme(self, name):
        self.theme = name
        self.config["theme"] = name
        themes.save_config(self.config, self.config_path)
        if self.on_theme:
            self.on_theme(name)
        self.say("Theme: {}.".format(name))

    def quit(self):
        busy = self.worker.busy_job()

        def really():
            self.running = False
        if busy is not None:
            self.confirm("quit", "'{}' is still running. Quit anyway? It is cancelled first."
                         .format(busy.label), lambda: (self.worker.cancel(), really()))
        else:
            really()

    def help_text(self):
        lines = ["rpc tui: control an RPCortex device over its link protocol.", "",
                 "Everywhere", "  1-6 or Tab, Shift+Tab   switch screen",
                 "  t                       next theme ({})".format(", ".join(themes.ORDER)),
                 "  Esc                     cancel what is running",
                 "  ?                       this help", "  q                       quit", ""]
        for s in self.screens:
            lines.append("{}  ({})".format(s.title, s.number))
            for k, label in s.help:
                lines.append("  {:<22}  {}".format(k, label))
            lines.append("")
        lines += ["sudo: when the device asks, the password is asked for here, masked, and",
                  "sent with AUTH. $RPC_PASSWORD is tried first when it is set.",
                  "Settings (the theme): {}".format(self.config_path or themes.config_path())]
        return "\n".join(lines)

    # --- the loop -----------------------------------------------------------------

    def tick(self):
        """Periodic work: refreshes, when the link has nothing else to do."""
        if self.connected and self.worker.idle() and not self.dialogs:
            self.screen.tick()
            for s in self.screens:
                if s is not self.screen:
                    s.background_tick()

    # --- drawing --------------------------------------------------------------------

    def render(self, c):
        c.clear()
        c.show_cursor(None, None)
        h, w = c.size()
        if h < MIN_ROWS or w < MIN_COLS:
            c.put(0, 0, "rpc tui needs at least {}x{};".format(MIN_COLS, MIN_ROWS), "warn")
            c.put(1, 0, "this terminal is {}x{}.".format(w, h), "warn")
            c.put(2, 0, "Make it larger, or press q to quit.", "muted")
            return
        self._top(c, w)
        body_h = h - 1 - 4
        self._nav(c, 1, 0, body_h, NAV_W)
        self.screen.render(c, 1, NAV_W + 1, body_h, w - NAV_W - 1)
        self._status(c, h - 4, w)
        for d in self.dialogs:
            d.render(c)

    def dot_role(self, state):
        return {"ok": "ok", "warn": "warn", "down": "down"}.get(state, "off")

    def _top(self, c, w):
        g = c.glyphs
        c.put(0, 0, "[", "text")
        c.put(0, 1, "rpc", "accent")
        c.put(0, 4, "]", "text")
        state = {"connected": "ok", "terminal": "ok", "connecting": "warn", "login": "warn",
                 "choose-port": "warn", "flashing": "warn"}.get(self.conn, "down")
        c.put(0, 6, g["dot"], self.dot_role(state))
        hello = self.hello
        if self.connected and hello:
            parts = ([self.device_name] if self.device_name else []) + [
                hello.get("board", "?"), hello.get("os", "?"),
                "{}@{}".format(hello.get("user", "?"), self.conn_detail or "")]
            if self.conn == "terminal":
                parts.append("terminal")
            summary = " {} ".format(g["sep"]).join(parts)
        else:
            summary = {"connecting": "connecting...", "login": "at the login prompt",
                       "choose-port": "choose a device", "flashing": "flashing"}.get(
                self.conn, "disconnected")
            if self.conn_detail and self.conn not in ("connecting",):
                summary += ": " + self.conn_detail
        tags = []
        if self.busy is not None:
            tags.append("busy")
        if self.mode == "legacy":
            tags.append("legacy")
        tag = "  ".join("[{}]".format(t) for t in tags)
        c.put(0, 8, summary, "muted", width=w - 9 - len(tag) - 1)
        if tag:
            c.put(0, w - len(tag), tag, "warn")

    def _nav(self, c, y, x, h, w):
        g = c.glyphs
        c.box(y, x, h, w, "nav")
        for i, s in enumerate(self.screens):
            r = y + 1 + i
            on = i == self.idx
            if on:
                c.put(r, x + 1, g["bar"], "sel")
            c.put(r, x + 2, g["dot"], self.dot_role(s.dot()))
            c.put(r, x + 4, "{} {}".format(s.number, s.title), "accent" if on else "text",
                  width=w - 5)
        c.put(y + h - 2, x + 2, "t " + self.theme, "muted", width=w - 3)

    def _status(self, c, y, w):
        g = c.glyphs
        c.box(y, 0, 4, w, "status")
        text, kind = self.msg
        role = {"err": "down", "ok": "ok", "warn": "warn"}.get(kind, "text")
        line = text.split("\n")[0]
        if self.busy is not None:
            label = self.busy.label
            if self.progress:
                plabel, done, total = self.progress
                frac = float(done) / total if total else 1.0
                bar_w = 16
                fill = int(round(frac * bar_w))
                bar = g["fill"] * fill + g["empty"] * (bar_w - fill)
                line = "{}  {} {:3d}%  {}".format(plabel, bar, int(frac * 100),
                                                 _amount(done, total))
            else:
                line = label + ("  " + self.step if self.step else "") + "..."
            if self.busy.cancellable:
                line += "   Esc cancels"
            role = "accent"
        c.put(y + 1, 2, line, role, width=w - 4)
        keys = self.screen.keys_line() + [("t", "theme"), ("?", "help"), ("q", "quit")]
        if self.screen.captures_keys():
            keys = self.screen.keys_line()
        x = 2
        for i, (k, label) in enumerate(keys):
            piece = "{} {}".format(k, label)
            if x + len(piece) + 3 > w - 2:
                break
            if i:
                c.put(y + 2, x, g["sep"], "muted")
                x += 2
            c.put(y + 2, x, k, "text")
            c.put(y + 2, x + len(k) + 1, label, "muted")
            x += len(piece) + 1


def _amount(done, total):
    from rpctool.util import human
    return "{} of {}".format(human(done), human(total)) if total else human(done)


def run_loop(app, backend, frame_wait=0.1, max_seconds=None):
    """Draw, read a key, handle events, repeat, until the app quits. The
    backend gives canvas, getkey(timeout) and refresh()."""
    start = time.monotonic()
    while app.running:
        app.pump()
        app.tick()
        if app.dirty:
            app.dirty = False
            app.render(backend.canvas)
            backend.refresh()
        k = backend.getkey(frame_wait)
        if k is not None:
            app.key(k)
        if max_seconds is not None and time.monotonic() - start > max_seconds:
            break
