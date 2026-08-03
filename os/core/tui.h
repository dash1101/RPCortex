// A character grid, and the drawing that goes onto it.
//
// v1 had five full-screen apps and each one drew its own boxes, tracked its own
// cursor and worked out its own redraws. They did not look alike, they did not
// behave alike, and each new one cost the same effort as the last. The point of
// this is that the fifth app is nearly free.
//
// **Everything renders into memory first.** Nothing here writes to a terminal.
// An app draws onto a grid of characters, and a separate step turns the
// difference between the last grid and this one into escape sequences. Two
// things follow, and both matter more than they sound:
//
//   * a whole screen can be ASSERTED ON in a test. "Is the selected row
//     highlighted, is the long filename truncated, does the box close" are
//     questions with exact answers here, and no answer at all over a serial
//     line someone has to look at.
//   * only what changed gets sent. A full 80x24 repaint is nearly 2 KB down a
//     115200 baud line — about 170 ms, which is visible as a flicker. v1's
//     settings panel cleared and redrew the entire screen on every keystroke
//     for exactly this reason, and it showed.
//
// The grid is fixed-size and lives wherever the caller puts it. No allocation
// happens in here.
#ifndef RPC_TUI_H
#define RPC_TUI_H

#include <stdint.h>
#include <stdbool.h>

// 80x24 is the assumption a serial terminal can always meet. Larger terminals
// get a centred region rather than a stretched layout, because a form that
// spreads to 200 columns is harder to read, not easier.
#define TUI_MAX_W  100
#define TUI_MAX_H  40

// Attributes, kept to what a serial terminal reliably has. Colour is an index
// rather than RGB for the same reason.
enum TuiAttr {
    TUI_NORMAL  = 0,
    TUI_BOLD    = 1 << 0,
    TUI_REVERSE = 1 << 1,   // how selection is shown; works on every terminal
    TUI_DIM     = 1 << 2,
    TUI_UNDER   = 1 << 3,
};

enum TuiColor {
    TUI_DEFAULT = 0, TUI_RED, TUI_GREEN, TUI_YELLOW,
    TUI_BLUE, TUI_MAGENTA, TUI_CYAN, TUI_WHITE,
};

struct TuiCell {
    char    ch;
    uint8_t attr;
    uint8_t fg;
};

struct TuiScreen {
    uint16_t w, h;
    TuiCell  cells[TUI_MAX_W * TUI_MAX_H];
};

// Clear to spaces with default attributes.
void tui_clear(TuiScreen *s);

// Set the working size. Clamped to the maxima above, and to at least 20x5 —
// below that no layout is readable and a caller is better told than left to
// draw into nothing.
void tui_resize(TuiScreen *s, uint16_t w, uint16_t h);

// One cell. Out-of-range coordinates are ignored rather than wrapping: a wrap
// puts text on the wrong line, which looks like a layout bug somewhere else
// entirely.
void tui_put(TuiScreen *s, int x, int y, char ch, uint8_t attr, uint8_t fg);

// Text from x,y, clipped at the right edge. Returns the columns written.
int tui_text(TuiScreen *s, int x, int y, const char *str, uint8_t attr, uint8_t fg);

// Text clipped to `width`, with an ellipsis when it does not fit. A filename
// that silently loses its end is worse than one visibly shortened.
int tui_text_clip(TuiScreen *s, int x, int y, const char *str, int width,
                  uint8_t attr, uint8_t fg);

// Fill a rectangle. Used for selection bars and for clearing a pane.
void tui_fill(TuiScreen *s, int x, int y, int w, int h, char ch,
              uint8_t attr, uint8_t fg);

// A box. `title` may be null; when present it is inlaid into the top edge.
// ASCII by default — box-drawing characters need the terminal to agree about
// the character set, and getting that wrong turns a panel into line noise.
void tui_box(TuiScreen *s, int x, int y, int w, int h, const char *title,
             uint8_t attr, uint8_t fg);

// Read a cell, for tests and for anything that needs to know what is there.
// Out of range reads as a space.
TuiCell tui_at(const TuiScreen *s, int x, int y);

// A whole row as a NUL-terminated string, trailing spaces trimmed. The most
// useful thing a test can ask for.
void tui_row_text(const TuiScreen *s, int y, char *out, uint32_t cap);

#endif  // RPC_TUI_H
