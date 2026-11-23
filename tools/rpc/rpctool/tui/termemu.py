"""A small terminal screen for the Terminal tab.

Enough of a VT100 for the device's console: printable text, CR, LF, BS, tab,
and the escape sequences the line editor and the full-screen commands use
(cursor moves, erase in line and display, cursor position). Colour is read
and dropped. Anything else is ignored rather than drawn.
"""
import re

_CSI = re.compile(r"\x1b\[([0-9;?]*)([ -/]*)([@-~])")
_PARTIAL = re.compile(r"\x1b\[[0-9;?]*[ -/]*\Z")


class Screen(object):
    def __init__(self, rows=24, cols=80, scrollback=500):
        self.rows, self.cols = rows, cols
        self.scrollback = scrollback
        self.history = []
        self.lines = [[" "] * cols for _ in range(rows)]
        self.y = self.x = 0
        self._pending = ""
        self._utf8 = b""

    def resize(self, rows, cols):
        rows, cols = max(1, rows), max(1, cols)
        if cols != self.cols:
            for i, ln in enumerate(self.lines):
                self.lines[i] = (ln + [" "] * cols)[:cols]
            self.cols = cols
        while rows < len(self.lines):
            if self.y > 0:
                self._push(self.lines.pop(0))
                self.y -= 1
            else:
                self.lines.pop()
        while rows > len(self.lines):
            self.lines.append([" "] * cols)
        self.rows = rows
        self.y = min(self.y, rows - 1)
        self.x = min(self.x, cols - 1)

    def _push(self, line):
        self.history.append("".join(line).rstrip())
        if len(self.history) > self.scrollback:
            del self.history[:len(self.history) - self.scrollback]

    def _newline(self):
        self.y += 1
        if self.y >= self.rows:
            self._push(self.lines.pop(0))
            self.lines.append([" "] * self.cols)
            self.y = self.rows - 1

    def feed(self, data):
        if isinstance(data, bytes):
            data = self._utf8 + data
            # Keep an incomplete UTF-8 sequence for the next feed.
            cut = len(data)
            for back in range(1, min(4, len(data)) + 1):
                b = data[-back]
                if b & 0xC0 == 0x80:
                    continue
                need = 2 if b & 0xE0 == 0xC0 else 3 if b & 0xF0 == 0xE0 else 4 if b & 0xF8 == 0xF0 else 1
                if need > back:
                    cut = len(data) - back
                break
            self._utf8 = data[cut:]
            data = data[:cut].decode("utf-8", "replace")
        text = self._pending + data
        self._pending = ""
        i, n = 0, len(text)
        while i < n:
            ch = text[i]
            if ch == "\x1b":
                m = _CSI.match(text, i)
                if m:
                    self._csi(m.group(1), m.group(3))
                    i = m.end()
                    continue
                if i + 1 >= n or _PARTIAL.match(text, i):
                    self._pending = text[i:]       # the rest arrives in the next feed
                    return
                if text[i + 1] in "()" and i + 2 < n:
                    i += 3
                else:
                    i += 2
                continue
            i += 1
            if ch == "\r":
                self.x = 0
            elif ch == "\n":
                self._newline()
            elif ch in ("\b", "\x7f"):
                self.x = max(0, self.x - 1)
            elif ch == "\t":
                self.x = min(self.cols - 1, (self.x // 8 + 1) * 8)
            elif ch == "\x07" or ord(ch) < 32:
                continue
            else:
                if self.x >= self.cols:
                    self.x = 0
                    self._newline()
                self.lines[self.y][self.x] = ch
                self.x += 1

    def _csi(self, params, final):
        nums = [int(p) if p.isdigit() else 0 for p in params.lstrip("?").split(";")] if params else []
        a = nums[0] if nums else 0
        if final == "m" or params.startswith("?"):
            return
        if final == "K":
            row = self.lines[self.y]
            if a == 0:
                for c in range(self.x, self.cols):
                    row[c] = " "
            elif a == 1:
                for c in range(0, min(self.x + 1, self.cols)):
                    row[c] = " "
            else:
                self.lines[self.y] = [" "] * self.cols
        elif final == "J":
            if a == 2 or a == 3:
                self.lines = [[" "] * self.cols for _ in range(self.rows)]
            elif a == 0:
                self._csi("", "K")
                for r in range(self.y + 1, self.rows):
                    self.lines[r] = [" "] * self.cols
        elif final in ("H", "f"):
            r = (nums[0] if nums and nums[0] else 1) - 1
            c = (nums[1] if len(nums) > 1 and nums[1] else 1) - 1
            self.y = max(0, min(self.rows - 1, r))
            self.x = max(0, min(self.cols - 1, c))
        elif final == "A":
            self.y = max(0, self.y - max(1, a))
        elif final == "B":
            self.y = min(self.rows - 1, self.y + max(1, a))
        elif final == "C":
            self.x = min(self.cols - 1, self.x + max(1, a))
        elif final == "D":
            self.x = max(0, self.x - max(1, a))
        elif final == "G":
            self.x = max(0, min(self.cols - 1, max(1, a) - 1))

    def text_lines(self):
        return ["".join(ln) for ln in self.lines]

    def text(self):
        return "\n".join(ln.rstrip() for ln in self.text_lines()).rstrip()


# Keys as the app names them (see ui.KEYS), as a terminal would send them.
KEY_BYTES = {
    "up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D",
    "home": b"\x1b[H", "end": b"\x1b[F", "delete": b"\x1b[3~", "insert": b"\x1b[2~",
    "pgup": b"\x1b[5~", "pgdn": b"\x1b[6~", "enter": b"\r", "tab": b"\t",
    "backspace": b"\x7f", "esc": b"\x1b", "btab": b"\x1b[Z",
    "f1": b"\x1bOP", "f2": b"\x1bOQ", "f3": b"\x1bOR", "f4": b"\x1bOS",
}


def key_bytes(key):
    """The bytes to send for one key, b"" for one with no meaning there."""
    if key in KEY_BYTES:
        return KEY_BYTES[key]
    if len(key) == 2 and key[0] == "^" and "@" <= key[1] <= "_":
        return bytes([ord(key[1]) - 64])          # ^C is 0x03, ^] is 0x1D
    if len(key) == 1:
        return key.encode("utf-8")
    return b""
