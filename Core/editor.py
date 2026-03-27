# Desc: Terminal text editor (nano-style) for RPCortex - Nebula OS
# File: /Core/editor.py
# Last Updated: 3/24/2026
# Lang: MicroPython, English
# Version: v1.0.0
# Author: dash1101
#
# Invoked via the shell: edit <filename>
# Requires a serial terminal with ANSI support (PuTTY, minicom, screen).
# Thonny's built-in REPL does NOT work — use a real terminal emulator.
#
# Controls:
#   Arrow keys      Move cursor
#   Ctrl+S          Save
#   Ctrl+Q          Quit (prompts if unsaved)
#   Ctrl+X          Save and quit
#   Ctrl+K          Cut current line
#   Ctrl+U          Paste cut line
#   Ctrl+F          Find text
#   Ctrl+G          Go to line number
#   Home / Ctrl+A   Start of line
#   End  / Ctrl+E   End of line
#   PgUp / PgDn     Page up / down
#   Backspace       Delete char before cursor
#   Delete          Delete char at cursor

import sys
import os

# ---------------------------------------------------------------------------
# Terminal / ANSI helpers
# ---------------------------------------------------------------------------

ESC = "\x1b"

def _w(s):
    sys.stdout.write(s)

def clear_screen():
    _w(ESC + "[2J")

def move_to(row, col):
    # Terminal rows/cols are 1-based
    _w(ESC + "[{};{}H".format(row + 1, col + 1))

def hide_cursor():
    _w(ESC + "[?25l")

def show_cursor():
    _w(ESC + "[?25h")

def reverse_video():
    _w(ESC + "[7m")

def normal_video():
    _w(ESC + "[0m")

def clear_line():
    _w(ESC + "[2K")

def get_terminal_size():
    """Try to read terminal size; fall back to 80x24."""
    try:
        # VT100 query: save cursor, move to 999,999, query position, restore
        _w(ESC + "[s" + ESC + "[999;999H" + ESC + "[6n" + ESC + "[u")
        resp = ""
        while True:
            ch = sys.stdin.read(1)
            resp += ch
            if ch == "R":
                break
        # resp is like ESC[rows;colsR
        inner = resp[2:-1]  # strip ESC[ and R
        rows, cols = inner.split(";")
        return int(rows), int(cols)
    except Exception:
        return 24, 80

# ---------------------------------------------------------------------------
# Raw input reading
# ---------------------------------------------------------------------------

def set_raw_mode():
    """Switch stdin to raw (no echo, no line buffering) if possible."""
    try:
        import uos
        uos.dupterm(None, 1)  # disable REPL DUPTERM if active
    except Exception:
        pass
    # On Pico, sys.stdin is already character-by-character in raw USB mode.
    # Nothing else to do at the MicroPython level.

KEY_UP    = "UP"
KEY_DOWN  = "DOWN"
KEY_LEFT  = "LEFT"
KEY_RIGHT = "RIGHT"
KEY_HOME  = "HOME"
KEY_END   = "END"
KEY_PGUP  = "PGUP"
KEY_PGDN  = "PGDN"
KEY_DEL   = "DELETE"
KEY_BKSP  = "BACKSPACE"
KEY_ENTER = "ENTER"
KEY_CTRL_S = "CTRL_S"
KEY_CTRL_Q = "CTRL_Q"
KEY_CTRL_X = "CTRL_X"
KEY_CTRL_K = "CTRL_K"
KEY_CTRL_U = "CTRL_U"
KEY_CTRL_G = "CTRL_G"
KEY_CTRL_F = "CTRL_F"
KEY_CTRL_A = "CTRL_A"
KEY_CTRL_E = "CTRL_E"

def read_key():
    """Read one keypress and return a key token or a character string."""
    ch = sys.stdin.read(1)

    # Control characters
    if ch == "\x01": return KEY_CTRL_A   # Ctrl+A
    if ch == "\x05": return KEY_CTRL_E   # Ctrl+E
    if ch == "\x06": return KEY_CTRL_F   # Ctrl+F
    if ch == "\x0b": return KEY_CTRL_K   # Ctrl+K
    if ch == "\x11": return KEY_CTRL_Q   # Ctrl+Q
    if ch == "\x13": return KEY_CTRL_S   # Ctrl+S
    if ch == "\x15": return KEY_CTRL_U   # Ctrl+U
    if ch == "\x18": return KEY_CTRL_X   # Ctrl+X
    if ch == "\x07": return KEY_CTRL_G   # Ctrl+G
    if ch in ("\r", "\n"): return KEY_ENTER
    if ch in ("\x7f", "\x08"): return KEY_BKSP

    # Escape sequences
    if ch == "\x1b":
        next1 = sys.stdin.read(1)
        if next1 == "[":
            seq = ""
            while True:
                c = sys.stdin.read(1)
                seq += c
                if c.isalpha() or c == "~":
                    break
            if seq == "A": return KEY_UP
            if seq == "B": return KEY_DOWN
            if seq == "C": return KEY_RIGHT
            if seq == "D": return KEY_LEFT
            if seq == "H": return KEY_HOME
            if seq == "F": return KEY_END
            if seq == "3~": return KEY_DEL
            if seq == "5~": return KEY_PGUP
            if seq == "6~": return KEY_PGDN
            if seq == "1~": return KEY_HOME
            if seq == "4~": return KEY_END
            return None  # unknown escape seq
        elif next1 == "O":
            c = sys.stdin.read(1)
            if c == "H": return KEY_HOME
            if c == "F": return KEY_END
            return None
        return None

    return ch  # printable character

# ---------------------------------------------------------------------------
# Editor state
# ---------------------------------------------------------------------------

class Editor:
    def __init__(self, filename=None):
        self.filename = filename
        self.lines = [""]
        self.cx = 0        # cursor col (0-based index into line)
        self.cy = 0        # cursor row (0-based index into self.lines)
        self.scroll_row = 0
        self.scroll_col = 0
        self.dirty = False
        self.clipboard = ""
        self.message = ""
        self.rows, self.cols = get_terminal_size()
        self.edit_rows = self.rows - 2  # reserve 2 rows: status + message

        if filename:
            self._load()

    # -----------------------------------------------------------------------
    # File I/O
    # -----------------------------------------------------------------------

    def _load(self):
        try:
            with open(self.filename, "r") as f:
                content = f.read()
            self.lines = content.split("\n")
            if not self.lines:
                self.lines = [""]
            self.message = "Loaded: {}  ({} lines)".format(
                self.filename, len(self.lines))
        except OSError:
            self.lines = [""]
            self.message = "New file: {}".format(self.filename)

    def save(self):
        if not self.filename:
            self.message = "No filename — use Ctrl+S after setting one."
            return False
        try:
            with open(self.filename, "w") as f:
                f.write("\n".join(self.lines))
            self.dirty = False
            self.message = "Saved: {}  ({} lines)".format(
                self.filename, len(self.lines))
            return True
        except OSError as e:
            self.message = "Save error: {}".format(e)
            return False

    # -----------------------------------------------------------------------
    # Rendering
    # -----------------------------------------------------------------------

    def render(self):
        hide_cursor()
        self._adjust_scroll()
        self._draw_rows()
        self._draw_status()
        self._draw_message()
        # Position terminal cursor
        screen_row = self.cy - self.scroll_row
        screen_col = self.cx - self.scroll_col
        move_to(screen_row, screen_col)
        show_cursor()

    def _adjust_scroll(self):
        if self.cy < self.scroll_row:
            self.scroll_row = self.cy
        if self.cy >= self.scroll_row + self.edit_rows:
            self.scroll_row = self.cy - self.edit_rows + 1
        if self.cx < self.scroll_col:
            self.scroll_col = self.cx
        if self.cx >= self.scroll_col + self.cols:
            self.scroll_col = self.cx - self.cols + 1

    def _draw_rows(self):
        for screen_row in range(self.edit_rows):
            file_row = screen_row + self.scroll_row
            move_to(screen_row, 0)
            clear_line()
            if file_row < len(self.lines):
                line = self.lines[file_row]
                visible = line[self.scroll_col:self.scroll_col + self.cols]
                _w(visible)

    def _draw_status(self):
        move_to(self.edit_rows, 0)
        reverse_video()
        fname = self.filename if self.filename else "[scratch]"
        modified = " [+]" if self.dirty else ""
        pos = "Ln {}/{}  Col {}".format(
            self.cy + 1, len(self.lines), self.cx + 1)
        help_hint = " ^S Save  ^Q Quit  ^X Save+Quit  ^K Cut  ^F Find"
        left = " {}{}".format(fname, modified)
        right = pos + help_hint + " "
        # Pad the middle
        space = self.cols - len(left) - len(right)
        if space < 0:
            space = 0
        status = left + " " * space + right
        status = status[:self.cols]
        _w(status)
        normal_video()

    def _draw_message(self):
        move_to(self.rows - 1, 0)
        clear_line()
        if self.message:
            _w(self.message[:self.cols])

    # -----------------------------------------------------------------------
    # Cursor movement
    # -----------------------------------------------------------------------

    def _clamp_cx(self):
        line_len = len(self.lines[self.cy])
        if self.cx > line_len:
            self.cx = line_len

    def move_up(self):
        if self.cy > 0:
            self.cy -= 1
            self._clamp_cx()

    def move_down(self):
        if self.cy < len(self.lines) - 1:
            self.cy += 1
            self._clamp_cx()

    def move_left(self):
        if self.cx > 0:
            self.cx -= 1
        elif self.cy > 0:
            self.cy -= 1
            self.cx = len(self.lines[self.cy])

    def move_right(self):
        line = self.lines[self.cy]
        if self.cx < len(line):
            self.cx += 1
        elif self.cy < len(self.lines) - 1:
            self.cy += 1
            self.cx = 0

    def move_home(self):
        self.cx = 0

    def move_end(self):
        self.cx = len(self.lines[self.cy])

    def page_up(self):
        self.cy = max(0, self.cy - self.edit_rows)
        self._clamp_cx()

    def page_down(self):
        self.cy = min(len(self.lines) - 1, self.cy + self.edit_rows)
        self._clamp_cx()

    # -----------------------------------------------------------------------
    # Editing
    # -----------------------------------------------------------------------

    def insert_char(self, ch):
        line = self.lines[self.cy]
        self.lines[self.cy] = line[:self.cx] + ch + line[self.cx:]
        self.cx += 1
        self.dirty = True

    def insert_newline(self):
        line = self.lines[self.cy]
        self.lines[self.cy] = line[:self.cx]
        self.lines.insert(self.cy + 1, line[self.cx:])
        self.cy += 1
        self.cx = 0
        self.dirty = True

    def backspace(self):
        if self.cx > 0:
            line = self.lines[self.cy]
            self.lines[self.cy] = line[:self.cx - 1] + line[self.cx:]
            self.cx -= 1
            self.dirty = True
        elif self.cy > 0:
            # Merge with previous line
            prev = self.lines[self.cy - 1]
            curr = self.lines[self.cy]
            self.cx = len(prev)
            self.lines[self.cy - 1] = prev + curr
            del self.lines[self.cy]
            self.cy -= 1
            self.dirty = True

    def delete_char(self):
        line = self.lines[self.cy]
        if self.cx < len(line):
            self.lines[self.cy] = line[:self.cx] + line[self.cx + 1:]
            self.dirty = True
        elif self.cy < len(self.lines) - 1:
            # Merge with next line
            next_line = self.lines[self.cy + 1]
            self.lines[self.cy] = line + next_line
            del self.lines[self.cy + 1]
            self.dirty = True

    def cut_line(self):
        """Ctrl+K: cut (delete) current line, store in clipboard."""
        if len(self.lines) == 1:
            self.clipboard = self.lines[0]
            self.lines[0] = ""
            self.cx = 0
        else:
            self.clipboard = self.lines[self.cy]
            del self.lines[self.cy]
            if self.cy >= len(self.lines):
                self.cy = len(self.lines) - 1
            self._clamp_cx()
        self.dirty = True
        self.message = "Line cut to clipboard."

    def paste_line(self):
        """Ctrl+U: paste clipboard above current line."""
        if self.clipboard == "":
            self.message = "Clipboard is empty."
            return
        self.lines.insert(self.cy, self.clipboard)
        self.dirty = True
        self.message = "Pasted from clipboard."

    # -----------------------------------------------------------------------
    # Search
    # -----------------------------------------------------------------------

    def find(self):
        self._draw_message()
        move_to(self.rows - 1, 0)
        clear_line()
        _w("Find: ")
        term = self._read_line()
        if not term:
            self.message = "Search cancelled."
            return
        # Search forward from current position
        start_cy = self.cy
        start_cx = self.cx + 1
        for offset in range(len(self.lines)):
            row = (start_cy + offset) % len(self.lines)
            col_start = start_cx if offset == 0 else 0
            idx = self.lines[row].find(term, col_start)
            if idx != -1:
                self.cy = row
                self.cx = idx
                self.message = "Found at line {}.".format(row + 1)
                return
        self.message = "'{}' not found.".format(term)

    def goto_line(self):
        move_to(self.rows - 1, 0)
        clear_line()
        _w("Go to line: ")
        s = self._read_line()
        if s:
            try:
                n = int(s) - 1
                n = max(0, min(n, len(self.lines) - 1))
                self.cy = n
                self.cx = 0
                self.message = "Jumped to line {}.".format(n + 1)
            except ValueError:
                self.message = "Invalid line number."

    def _read_line(self):
        """Read a line of text from stdin (mini prompt, no REPL)."""
        buf = ""
        while True:
            ch = sys.stdin.read(1)
            if ch in ("\r", "\n"):
                _w("\r\n")
                return buf
            elif ch in ("\x7f", "\x08"):
                if buf:
                    buf = buf[:-1]
                    _w("\x08 \x08")
            elif ch == "\x1b":
                return ""  # ESC cancels
            elif ch == "\x03":
                return ""  # Ctrl+C cancels
            elif ord(ch) >= 32:
                buf += ch
                _w(ch)

    # -----------------------------------------------------------------------
    # Quit handling
    # -----------------------------------------------------------------------

    def prompt_save_quit(self):
        """Return True if it's OK to quit."""
        if not self.dirty:
            return True
        move_to(self.rows - 1, 0)
        clear_line()
        _w("Unsaved changes. Save before quitting? (y/n/cancel): ")
        while True:
            ch = sys.stdin.read(1).lower()
            if ch == "y":
                self.save()
                return True
            if ch == "n":
                return True
            if ch in ("c", "\x1b", "\x03"):
                self.message = "Quit cancelled."
                return False

    # -----------------------------------------------------------------------
    # Main loop
    # -----------------------------------------------------------------------

    def run(self):
        clear_screen()
        self.message = (self.message or
                        "^S Save  ^Q Quit  ^X Save+Quit  ^K Cut  ^U Paste  ^F Find  ^G GoTo")
        while True:
            self.render()
            key = read_key()
            if key is None:
                continue

            if key == KEY_UP:       self.move_up()
            elif key == KEY_DOWN:   self.move_down()
            elif key == KEY_LEFT:   self.move_left()
            elif key == KEY_RIGHT:  self.move_right()
            elif key == KEY_HOME or key == KEY_CTRL_A: self.move_home()
            elif key == KEY_END  or key == KEY_CTRL_E: self.move_end()
            elif key == KEY_PGUP:   self.page_up()
            elif key == KEY_PGDN:   self.page_down()
            elif key == KEY_ENTER:  self.insert_newline()
            elif key == KEY_BKSP:   self.backspace()
            elif key == KEY_DEL:    self.delete_char()
            elif key == KEY_CTRL_K: self.cut_line()
            elif key == KEY_CTRL_U: self.paste_line()
            elif key == KEY_CTRL_F: self.find()
            elif key == KEY_CTRL_G: self.goto_line()
            elif key == KEY_CTRL_S:
                self.save()
            elif key == KEY_CTRL_X:
                self.save()
                break
            elif key == KEY_CTRL_Q:
                if self.prompt_save_quit():
                    break
            elif isinstance(key, str) and len(key) == 1 and ord(key) >= 32:
                self.insert_char(key)
            else:
                self.message = ""  # clear stale message on unknown key

        # Cleanup
        clear_screen()
        move_to(0, 0)
        show_cursor()
        normal_video()
        print("Editor closed.")
        if self.filename and not self.dirty:
            print("File saved: {}".format(self.filename))


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------

def edit(filename=None):
    """
    Open the editor.
    Usage:
        from editor import edit
        edit("myfile.txt")   # edit a file
        edit()               # scratch buffer
    """
    e = Editor(filename)
    e.run()


# Run directly if executed as main script (MicroPython has no sys.argv)
if __name__ == "__main__":
    edit()