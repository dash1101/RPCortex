// Getting a rendered grid onto a real terminal, and reading one back.
//
// This is the only part of the TUI that touches a serial line. Everything it
// sends is decided by comparing the grid an app just drew against the one
// showing now, so a keystroke that changes one row sends one row.
//
// That is not an optimisation, it is the difference between usable and not. A
// full 80x24 repaint is about 2 KB of escape sequences; at 115200 baud that is
// ~170 ms, which reads as a flicker on every keypress. v1's settings panel
// cleared and redrew the whole screen each time and it showed.

#include "tui.h"
#include "tuikey.h"
#include "tuiterm.h"
#include "out.h"
#include "task.h"
#include "interrupt.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

// The grid showing on the terminal right now. Kept so the next frame can be
// sent as a difference. One screen of TuiCell is ~12 KB, which is the price of
// not repainting — worth it against 520 KB of RAM.
static TuiScreen g_shown;
static bool      g_active;
static TuiKeyParser g_keys;

// --- escape sequences -------------------------------------------------------

static void esc(const char *s) { out_write(s, (uint32_t)strlen(s)); }

static void move_to(int x, int y) {
    char b[24];
    int n = snprintf(b, sizeof(b), "\033[%d;%dH", y + 1, x + 1);   // 1-based
    out_write(b, (uint32_t)n);
}

// Emit the SGR for an attribute set, but only when it differs from what the
// terminal is already in — a colour code before every character would triple
// the bytes sent and undo the point of diffing.
static void apply_attr(uint8_t attr, uint8_t fg, uint8_t *cur_attr, uint8_t *cur_fg) {
    if (attr == *cur_attr && fg == *cur_fg) return;
    char b[48];
    int n = snprintf(b, sizeof(b), "\033[0");
    if (attr & TUI_BOLD)    n += snprintf(b + n, sizeof(b) - n, ";1");
    if (attr & TUI_DIM)     n += snprintf(b + n, sizeof(b) - n, ";2");
    if (attr & TUI_UNDER)   n += snprintf(b + n, sizeof(b) - n, ";4");
    if (attr & TUI_REVERSE) n += snprintf(b + n, sizeof(b) - n, ";7");
    if (fg != TUI_DEFAULT)  n += snprintf(b + n, sizeof(b) - n, ";%d", 30 + (fg - 1));
    n += snprintf(b + n, sizeof(b) - n, "m");
    out_write(b, (uint32_t)n);
    *cur_attr = attr;
    *cur_fg = fg;
}

void tuiterm_begin(void) {
    if (g_active) return;
    g_active = true;
    tuikey_init(&g_keys);

    esc("\033[?1049h");     // the alternate screen, so the shell scrollback survives
    esc("\033[?25l");       // hide the cursor; an app that wants one places it
    esc(TUI_MOUSE_ON);
    esc("\033[2J");

    // Force the first frame to send everything, by making the remembered screen
    // impossible to match.
    memset(&g_shown, 0, sizeof(g_shown));
    g_shown.w = 0; g_shown.h = 0;
}

void tuiterm_end(void) {
    if (!g_active) return;
    g_active = false;
    // Order matters on the way out. Leaving mouse reporting on means the
    // terminal sends escape sequences to the SHELL for every click afterwards,
    // which looks like the device has started typing by itself.
    esc(TUI_MOUSE_OFF);
    esc("\033[0m");
    esc("\033[?25h");
    esc("\033[?1049l");     // back to the shell's screen, scrollback intact
}

void tuiterm_present(const TuiScreen *s) {
    if (!g_active) return;

    bool full = (g_shown.w != s->w || g_shown.h != s->h);
    if (full) {
        esc("\033[2J");
        g_shown.w = s->w;
        g_shown.h = s->h;
        for (uint32_t i = 0; i < (uint32_t)s->w * s->h; i++) {
            g_shown.cells[i].ch = 0;      // nothing can match, so everything sends
        }
    }

    uint8_t cur_attr = 0xff, cur_fg = 0xff;   // unknown, so the first cell sets it
    int last_x = -99, last_y = -99;

    for (int y = 0; y < (int)s->h; y++) {
        for (int x = 0; x < (int)s->w; x++) {
            uint32_t i = (uint32_t)y * s->w + x;
            const TuiCell &now = s->cells[i];
            TuiCell &was = g_shown.cells[i];
            if (now.ch == was.ch && now.attr == was.attr && now.fg == was.fg) continue;

            // Only move when the cursor is not already where it needs to be:
            // consecutive changed cells cost one byte each rather than eight.
            if (!(last_y == y && last_x == x)) { move_to(x, y); last_x = x; last_y = y; }
            apply_attr(now.attr, now.fg, &cur_attr, &cur_fg);
            char c = now.ch >= 32 && now.ch < 127 ? now.ch : ' ';
            out_write(&c, 1);
            last_x++;
            was = now;
        }
    }
    esc("\033[0m");
}

void tuiterm_cursor(int x, int y, bool visible) {
    if (!g_active) return;
    if (visible) { move_to(x, y); esc("\033[?25h"); }
    else         esc("\033[?25l");
}

bool tuiterm_poll(TuiEvent *out) {
    if (!g_active || !out) return false;

    uint32_t now = task_now_ms();
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) {
        // A lone Escape only becomes an Escape once enough time has passed
        // without a following byte. Polling for it here is what makes the key
        // work at all rather than appearing dead until the next keypress.
        TuiEvent e = tuikey_timeout(&g_keys, now);
        if (e.kind != TUI_EV_NONE) { *out = e; return true; }
        return false;
    }

    TuiEvent e = tuikey_feed(&g_keys, (uint8_t)c, now);
    if (e.kind == TUI_EV_NONE) return false;
    *out = e;
    return true;
}

bool tuiterm_active(void) { return g_active; }
