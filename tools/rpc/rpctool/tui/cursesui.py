"""The curses side: a Canvas that draws with curses, the theme turned into
colour pairs, and keys turned into the names the app uses.

curses is imported only here, so the rest of the TUI (and its tests) runs
where curses is missing. On Windows it comes from the windows-curses package.
"""
import os
import sys

from . import themes
from .canvas import ASCII_GLYPHS, UNICODE_GLYPHS, Canvas

WINDOWS_HINT = ("rpc tui needs curses, which Python on Windows does not include.\n"
                "Install it with:\n\n    pip install windows-curses\n")


def import_curses():
    """curses, or an RpcError-free SystemExit message saying how to get it."""
    try:
        import curses  # noqa: F401
        return curses
    except ImportError:
        if os.name == "nt":
            raise SystemExit(WINDOWS_HINT)
        raise SystemExit("rpc tui needs Python's curses module, which this Python was built "
                         "without.")


# Roles that read as secondary: drawn dim where there are only 8 colours or
# none, since the nearest basic colour of a grey is plain white.
_DIM = ("muted", "legend", "line", "off")


class Colours(object):
    """Colour pairs for one theme, at the depth the terminal has."""

    def __init__(self, curses, name, depth=None):
        self.c = curses
        self.name = name
        self.depth = depth or self.detect()
        self.attrs = {}
        self.build()

    def detect(self):
        c = self.c
        if not c.has_colors():
            return "mono"
        try:
            c.start_color()
        except c.error:
            return "mono"
        try:
            c.use_default_colors()
        except c.error:
            pass
        n = getattr(c, "COLORS", 8)
        ext = getattr(c, "has_extended_color_support", lambda: False)()
        if n >= 1 << 24 and ext:
            return "true"
        if n >= 256:
            return "256"
        return "8"

    def _col(self, hexstr, depth):
        if depth == "true":
            r, g, b = themes.rgb(hexstr)
            return (r << 16) | (g << 8) | b
        if depth == "256":
            return themes.xterm256(hexstr)
        return themes.basic8(hexstr)

    def build(self):
        c = self.c
        t = themes.THEMES[self.name]
        if self.depth == "mono":
            a = {"text": c.A_NORMAL, "muted": c.A_DIM, "legend": c.A_DIM, "accent": c.A_BOLD,
                 "line": c.A_NORMAL, "line_hi": c.A_BOLD, "ok": c.A_NORMAL, "warn": c.A_BOLD,
                 "down": c.A_BOLD | c.A_UNDERLINE, "off": c.A_DIM, "sel": c.A_BOLD}
            for role, attr in a.items():
                self.attrs[(role, False, False)] = attr
                self.attrs[(role, True, False)] = attr | c.A_REVERSE
                self.attrs[(role, False, True)] = c.A_REVERSE | c.A_BOLD
                self.attrs[(role, True, True)] = c.A_REVERSE | c.A_BOLD
            self.background = c.A_NORMAL
            return
        try:
            self._pairs(t, self.depth)
        except (c.error, ValueError, OverflowError):
            if self.depth == "true":
                self.depth = "256" if getattr(c, "COLORS", 8) >= 256 else "8"
                self._pairs(t, self.depth)
            else:
                raise

    def _pairs(self, t, depth):
        c = self.c
        light = themes.is_light(self.name)
        if depth == "8":
            bg = 7 if light else -1
            raise_bg = None
        else:
            bg = self._col(t["bg"], depth)
            raise_bg = self._col(t["raise_"], depth)
        n = 1
        for role in themes.ROLES:
            fg = self._col(t[role], depth)
            if depth == "8" and light and fg == 7:
                fg = 0
            extra = c.A_DIM if depth == "8" and role in _DIM else 0
            if role in ("accent", "sel"):
                extra |= c.A_BOLD
            c.init_pair(n, fg, bg)
            self.attrs[(role, False, False)] = c.color_pair(n) | extra
            n += 1
            if raise_bg is None:
                self.attrs[(role, True, False)] = c.color_pair(n - 1) | c.A_REVERSE
            else:
                c.init_pair(n, fg, raise_bg)
                self.attrs[(role, True, False)] = c.color_pair(n) | extra
                n += 1
        ink = self._col(t["accent_ink"], depth)
        acc = self._col(t["accent"], depth)
        c.init_pair(n, ink, acc)
        inv = c.color_pair(n) | c.A_BOLD
        for role in themes.ROLES:
            self.attrs[(role, False, True)] = inv
            self.attrs[(role, True, True)] = inv
        self.background = self.attrs[("text", False, False)]

    def attr(self, role, hl, inv):
        return self.attrs.get((role, hl, inv)) or self.attrs.get(("text", hl, inv), 0)


class CursesCanvas(Canvas):
    def __init__(self, scr, colours, unicode=True):
        self.scr = scr
        self.colours = colours
        self.glyphs = UNICODE_GLYPHS if unicode else ASCII_GLYPHS
        self.cursor = None

    def size(self):
        return self.scr.getmaxyx()

    def clear(self):
        self.scr.erase()

    def _put(self, y, x, text, role, hl, inv):
        try:
            self.scr.addstr(y, x, text, self.colours.attr(role, hl, inv))
        except self.colours.c.error:
            # Writing the bottom-right cell moves the cursor off the screen,
            # which curses reports as an error after drawing it. Harmless.
            pass


class Backend(object):
    def __init__(self, curses, scr, theme, unicode=True, depth=None):
        self.curses = curses
        self.scr = scr
        self.unicode = unicode
        self.depth = depth
        scr.keypad(True)
        try:
            curses.raw()          # Ctrl+C and Ctrl+] arrive as keys, for the terminal
        except curses.error:
            pass
        curses.noecho()
        self.colours = Colours(curses, theme, depth)
        self.canvas = CursesCanvas(scr, self.colours, unicode)
        self._apply_background()

    def set_theme(self, name):
        self.colours = Colours(self.curses, name, self.colours.depth)
        self.canvas.colours = self.colours
        self._apply_background()

    def _apply_background(self):
        try:
            self.scr.bkgd(" ", self.colours.background)
        except self.curses.error:
            pass

    def refresh(self):
        c = self.curses
        cur = self.canvas.cursor
        try:
            if cur is None:
                c.curs_set(0)
            else:
                c.curs_set(1)
                self.scr.move(*cur)
        except c.error:
            pass
        self.scr.refresh()

    def getkey(self, timeout):
        c = self.curses
        self.scr.timeout(int(timeout * 1000))
        try:
            k = self.scr.get_wch()
        except c.error:
            return None
        except KeyboardInterrupt:
            return "^C"
        return self.name(k)

    def name(self, k):
        c = self.curses
        if isinstance(k, int):
            if k == c.KEY_RESIZE:
                try:
                    if os.name == "nt":
                        c.resize_term(0, 0)
                    else:
                        c.update_lines_cols()
                except (c.error, AttributeError):
                    pass
                return "resize"
            table = {c.KEY_UP: "up", c.KEY_DOWN: "down", c.KEY_LEFT: "left",
                     c.KEY_RIGHT: "right", c.KEY_HOME: "home", c.KEY_END: "end",
                     c.KEY_PPAGE: "pgup", c.KEY_NPAGE: "pgdn", c.KEY_DC: "delete",
                     c.KEY_IC: "insert", c.KEY_BACKSPACE: "backspace", c.KEY_ENTER: "enter",
                     c.KEY_BTAB: "btab"}
            for extra, nm in (("PADENTER", "enter"), ("CTL_LEFT", "left"),
                              ("CTL_RIGHT", "right"), ("KEY_A1", "home"), ("KEY_C1", "end")):
                if hasattr(c, extra):
                    table[getattr(c, extra)] = nm
            if k in table:
                return table[k]
            for n in range(1, 13):
                if k == c.KEY_F(n):
                    return "f{}".format(n)
            return None
        if k in ("\n", "\r"):
            return "enter"
        if k == "\t":
            return "tab"
        if k == "\x1b":
            return "esc"
        if k in ("\x7f", "\x08"):
            return "backspace"
        if len(k) == 1 and ord(k) < 32:
            return "^" + chr(ord(k) + 64)
        return k


def wants_unicode():
    if os.name == "nt":
        return True
    import locale
    enc = (locale.getpreferredencoding(False) or "").lower()
    return "utf" in enc


def run(app, theme_depth=None, ascii_only=False):
    """Run the app full screen until it quits."""
    curses = import_curses()
    os.environ.setdefault("ESCDELAY", "25")      # Esc on its own, without a long pause
    import locale
    try:
        locale.setlocale(locale.LC_ALL, "")
    except locale.Error:
        pass
    unicode = wants_unicode() and not ascii_only

    def main(scr):
        from .app import run_loop
        backend = Backend(curses, scr, app.theme, unicode, theme_depth)
        app.on_theme = backend.set_theme
        run_loop(app, backend)
    try:
        curses.wrapper(main)
    finally:
        sys.stdout.flush()
