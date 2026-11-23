"""What the screens draw on.

A Canvas is a grid of cells, each a character and a style: a role from the
theme (text, muted, accent, ok, ...), whether it sits on the raised
background of a selected row, and whether it is inverted (accent ink on the
accent). The curses layer is one Canvas; TextCanvas is another, which the
tests draw on and read back as plain text.
"""
import unicodedata

UNICODE_GLYPHS = {"tl": "┌", "tr": "┐", "bl": "└", "br": "┘",
                  "h": "─", "v": "│", "dot": "■", "bar": "▌",
                  "fill": "█", "empty": "░", "dots": "…", "sep": "·"}
ASCII_GLYPHS = {"tl": "+", "tr": "+", "bl": "+", "br": "+", "h": "-", "v": "|",
                "dot": "*", "bar": ">", "fill": "#", "empty": ".", "dots": "~", "sep": "-"}


def cell_width(ch):
    """1, or 2 for a wide (East Asian) character. Combining marks are folded
    into the character before them by the caller, so are counted as 0."""
    if unicodedata.combining(ch):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


def fit(text, width):
    """`text` cut to `width` cells, with an ellipsis where it was cut."""
    text = "".join(c if c >= " " else " " for c in text)
    w = sum(cell_width(c) for c in text)
    if w <= width:
        return text
    out, used = [], 0
    for c in text:
        cw = cell_width(c)
        if used + cw > width - 1:
            break
        out.append(c)
        used += cw
    return "".join(out) + ("…" if width >= 1 else "")


def pad(text, width):
    text = fit(text, width)
    return text + " " * max(0, width - sum(cell_width(c) for c in text))


class Canvas(object):
    """The drawing interface. Subclasses implement size, clear and _put."""

    glyphs = UNICODE_GLYPHS

    def size(self):
        raise NotImplementedError

    def clear(self):
        raise NotImplementedError

    def _put(self, y, x, text, role, hl, inv):
        raise NotImplementedError

    def show_cursor(self, y, x):
        """Where the cursor should sit after this frame, or None to hide it."""
        self.cursor = None if y is None else (y, x)

    def put(self, y, x, text, role="text", hl=False, inv=False, width=None):
        """Write text at (y, x), clipped to the canvas (and to `width`)."""
        h, w = self.size()
        if y < 0 or y >= h or x >= w or not text:
            return
        if x < 0:
            text = text[-x:]
            x = 0
        room = w - x
        if width is not None:
            room = min(room, width)
        if room <= 0:
            return
        text = fit(text, room)
        self._put(y, x, text, role, hl, inv)

    def fill(self, y, x, width, role="text", hl=False, inv=False):
        if width > 0:
            self.put(y, x, " " * width, role, hl, inv)

    def box(self, y, x, h, w, legend=None, legend_right=None, role="line", legend_role="legend"):
        """A square box with single lines, its title cut into the top edge, as
        Orbit draws its boxes."""
        if h < 2 or w < 2:
            return
        g = self.glyphs
        self.put(y, x, g["tl"] + g["h"] * (w - 2) + g["tr"], role)
        for r in range(y + 1, y + h - 1):
            self.put(r, x, g["v"], role)
            self.put(r, x + w - 1, g["v"], role)
        self.put(y + h - 1, x, g["bl"] + g["h"] * (w - 2) + g["br"], role)
        if legend and w > 6:
            self.put(y, x + 2, " " + fit(legend, w - 6) + " ", legend_role)
        if legend_right and w > 12:
            t = " " + fit(legend_right, max(1, w // 2 - 4)) + " "
            self.put(y, x + w - 2 - len(t), t, "muted")

    def hline(self, y, x, w, role="line"):
        self.put(y, x, self.glyphs["h"] * w, role)


class TextCanvas(Canvas):
    """A canvas in memory: what the tests draw on and read back."""

    def __init__(self, rows=24, cols=80, unicode=True):
        self.rows, self.cols = rows, cols
        self.glyphs = UNICODE_GLYPHS if unicode else ASCII_GLYPHS
        self.cursor = None
        self.clear()

    def resize(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.clear()

    def size(self):
        return self.rows, self.cols

    def clear(self):
        self.chars = [[" "] * self.cols for _ in range(self.rows)]
        self.styles = [[("text", False, False)] * self.cols for _ in range(self.rows)]

    def _put(self, y, x, text, role, hl, inv):
        for ch in text:
            if x >= self.cols:
                break
            self.chars[y][x] = ch
            self.styles[y][x] = (role, hl, inv)
            x += 1
            if cell_width(ch) == 2 and x < self.cols:
                self.chars[y][x] = ""
                x += 1

    def line(self, y):
        return "".join(self.chars[y])

    def text(self):
        return "\n".join(self.line(y).rstrip() for y in range(self.rows))

    def find(self, needle):
        """(y, x) of the first place `needle` is drawn, or None."""
        for y in range(self.rows):
            x = self.line(y).find(needle)
            if x >= 0:
                return y, x
        return None

    def role_at(self, y, x):
        return self.styles[y][x][0]
