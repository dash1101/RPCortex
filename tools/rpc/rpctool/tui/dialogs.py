"""Dialogs: a box over the screen that takes every key until it closes.

Each has render(canvas) and key(k), which returns True when the dialog is
finished (closed). The answer goes to a callback given when it was made.
"""
from .canvas import cell_width, fit


def _wrap(text, width):
    out = []
    for para in text.split("\n"):
        if not para:
            out.append("")
            continue
        indent = len(para) - len(para.lstrip(" "))
        words = para.split(" ")
        line = ""
        for w in words:
            if not w and not line:
                continue
            cand = (line + " " + w) if line else (" " * indent + w)
            if sum(cell_width(c) for c in cand) > width and line:
                out.append(line)
                line = " " * indent + w
            else:
                line = cand
        out.append(line)
    return out


class Dialog(object):
    title = ""
    width = 64

    def frame(self, canvas, lines_needed):
        h, w = canvas.size()
        bw = min(self.width, w - 4)
        bh = min(lines_needed + 2, h - 2)
        y = max(1, (h - bh) // 2)
        x = max(1, (w - bw) // 2)
        for r in range(y, y + bh):
            canvas.fill(r, x, bw)
        canvas.box(y, x, bh, bw, self.title, role="line_hi", legend_role="accent")
        return y + 1, x + 2, bh - 2, bw - 4

    def key(self, k):
        return True

    def body_width(self, canvas):
        return min(self.width, canvas.size()[1] - 4) - 4


class Prompt(Dialog):
    """One line of text. masked: show a bullet for each character and keep
    the text only until it is handed over."""

    def __init__(self, title, label, done, default="", masked=False, hint=None):
        self.title = title
        self.label = label
        self.done = done
        self.value = default
        self.masked = masked
        self.hint = hint

    def render(self, canvas):
        bw = self.body_width(canvas)
        lines = _wrap(self.label, bw)
        extra = _wrap(self.hint, bw) if self.hint else []
        y, x, h, w = self.frame(canvas, len(lines) + len(extra) + 4)
        r = y
        for ln in lines[:max(0, h - 3)]:
            canvas.put(r, x, ln, "text")
            r += 1
        r += 1
        shown = ("•" * len(self.value)) if self.masked else self.value
        tail = shown[-(w - 3):] if len(shown) > w - 3 else shown
        canvas.put(r, x, "> ", "accent")
        canvas.put(r, x + 2, tail + " ", "text", hl=True, width=w - 2)
        canvas.fill(r, x + 2 + len(tail) + 1, max(0, w - 3 - len(tail)), "text", hl=True)
        canvas.show_cursor(r, x + 2 + len(tail))
        r += 1
        for ln in extra:
            canvas.put(r, x, ln, "muted")
            r += 1
        canvas.put(y + h - 1, x, "Enter ok · Esc cancel", "muted")

    def key(self, k):
        if k == "enter":
            v, self.value = self.value, ""
            self.done(v)
            return True
        if k == "esc":
            self.value = ""
            self.done(None)
            return True
        if k == "backspace":
            self.value = self.value[:-1]
        elif k == "^U":
            self.value = ""
        elif len(k) == 1 and k >= " ":
            self.value += k
        return False


class Confirm(Dialog):
    """A question with choices, each with its key. The first is the default
    for Enter only when `default` says so; Esc is always the last ("no")."""

    def __init__(self, title, text, done, choices=(("y", "yes"), ("n", "no")), default=None,
                 danger=False):
        self.title = title
        self.text = text
        self.done = done
        self.choices = list(choices)
        self.default = default
        self.danger = danger
        self.top = 0
        self.width = 76

    def render(self, canvas):
        bw = self.body_width(canvas)
        lines = _wrap(self.text, bw)
        y, x, h, w = self.frame(canvas, len(lines) + 3)
        room = h - 2
        self.top = max(0, min(self.top, len(lines) - room))
        for i, ln in enumerate(lines[self.top:self.top + room]):
            canvas.put(y + i, x, ln, "warn" if self.danger and i + self.top == 0 else "text")
        cx = x
        for k, label in self.choices:
            t = "[{}] {}".format(k, label)
            canvas.put(y + h - 1, cx, t, "accent" if k == self.default else "text")
            cx += len(t) + 3
        canvas.put(y + h - 1, cx, "Esc cancel", "muted")

    def key(self, k):
        if k in ("down", "j"):
            self.top += 1
            return False
        if k in ("up", "k"):
            self.top = max(0, self.top - 1)
            return False
        for ck, _ in self.choices:
            if k == ck or (k == "enter" and ck == self.default):
                self.done(ck)
                return True
        if k == "esc":
            self.done(None)
            return True
        return False


class Choose(Dialog):
    """Pick one line from a list."""

    def __init__(self, title, text, items, done):
        self.title = title
        self.text = text
        self.items = list(items)
        self.done = done
        self.sel = 0

    def render(self, canvas):
        bw = self.body_width(canvas)
        lines = _wrap(self.text, bw)
        y, x, h, w = self.frame(canvas, len(lines) + len(self.items) + 3)
        r = y
        for ln in lines:
            canvas.put(r, x, ln)
            r += 1
        r += 1
        for i, it in enumerate(self.items):
            canvas.put(r, x, " " + fit(it, w - 2) + " ", "accent" if i == self.sel else "text",
                       hl=i == self.sel, width=w)
            r += 1
        canvas.put(y + h - 1, x, "↑↓ move · Enter choose · Esc cancel", "muted")

    def key(self, k):
        if k in ("down", "j"):
            self.sel = min(len(self.items) - 1, self.sel + 1)
        elif k in ("up", "k"):
            self.sel = max(0, self.sel - 1)
        elif k == "enter" and self.items:
            self.done(self.sel)
            return True
        elif k == "esc":
            self.done(None)
            return True
        return False


class Form(Dialog):
    """A text field and some switches. Tab or the arrows move, Space toggles,
    Enter starts."""

    def __init__(self, title, text, field_label, value, toggles, done):
        self.title = title
        self.text = text
        self.field_label = field_label
        self.value = value
        self.toggles = [[label, bool(on)] for label, on in toggles]
        self.done = done
        self.focus = 0
        self.width = 70

    def render(self, canvas):
        bw = self.body_width(canvas)
        lines = _wrap(self.text, bw) if self.text else []
        y, x, h, w = self.frame(canvas, len(lines) + len(self.toggles) + 5)
        r = y
        for ln in lines:
            canvas.put(r, x, ln, "muted")
            r += 1
        canvas.put(r, x, self.field_label, "legend")
        r += 1
        tail = self.value[-(w - 3):]
        canvas.put(r, x, "> ", "accent" if self.focus == 0 else "muted")
        canvas.put(r, x + 2, tail + " ", "text", hl=True, width=w - 2)
        canvas.fill(r, x + 2 + len(tail) + 1, max(0, w - 3 - len(tail)), "text", hl=True)
        if self.focus == 0:
            canvas.show_cursor(r, x + 2 + len(tail))
        r += 2
        for i, (label, on) in enumerate(self.toggles):
            t = "[{}] {}".format("x" if on else " ", label)
            canvas.put(r, x, t, "accent" if self.focus == i + 1 else "text", hl=self.focus == i + 1)
            r += 1
        canvas.put(y + h - 1, x, "Tab move · Space switch · Enter start · Esc cancel",
                   "muted")

    def key(self, k):
        n = len(self.toggles) + 1
        if k in ("tab", "down"):
            self.focus = (self.focus + 1) % n
        elif k in ("btab", "up"):
            self.focus = (self.focus - 1) % n
        elif k == "enter":
            self.done(self.value, [on for _, on in self.toggles])
            return True
        elif k == "esc":
            self.done(None, None)
            return True
        elif self.focus == 0:
            if k == "backspace":
                self.value = self.value[:-1]
            elif len(k) == 1 and k >= " ":
                self.value += k
        elif k == " ":
            t = self.toggles[self.focus - 1]
            t[1] = not t[1]
        return False


class Viewer(Dialog):
    """Text to read: a file, a command's output, the help."""

    def __init__(self, title, text, done=None):
        self.title = title
        self.lines = text.replace("\r", "").replace("\t", "    ").split("\n")
        self.top = 0
        self.done = done
        self.page = 10

    def render(self, canvas):
        h, w = canvas.size()
        self.width = w - 4
        y, x, bh, bw = self.frame(canvas, h - 4)
        self.page = max(1, bh - 1)
        self.top = max(0, min(self.top, len(self.lines) - self.page))
        for i, ln in enumerate(self.lines[self.top:self.top + self.page]):
            canvas.put(y + i, x, ln, "text", width=bw)
        pos = "{}-{} of {}".format(self.top + 1, min(len(self.lines), self.top + self.page),
                                   len(self.lines))
        canvas.put(y + bh - 1, x, "↑↓ PgUp PgDn scroll · Esc or q close   " + pos,
                   "muted")

    def key(self, k):
        if k in ("down", "j"):
            self.top += 1
        elif k in ("up", "k"):
            self.top = max(0, self.top - 1)
        elif k in ("pgdn", " "):
            self.top += self.page
        elif k == "pgup":
            self.top = max(0, self.top - self.page)
        elif k == "home":
            self.top = 0
        elif k == "end":
            self.top = len(self.lines)
        elif k in ("esc", "q", "enter"):
            if self.done:
                self.done()
            return True
        return False
