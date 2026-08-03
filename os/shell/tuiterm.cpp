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
#include <stdlib.h>

#include "pico/stdlib.h"

// The grid showing on the terminal right now, kept so the next frame can be
// sent as a difference.
//
// ALLOCATED, not static. A TuiScreen is ~12 KB and there are two of them — this
// one and the one apps draw into — which is 23 KB permanently unavailable for a
// feature most sessions never use. They live only while a full-screen app is
// running, which is also the only time anything can afford them.
static TuiScreen *g_shown;
static bool      g_active;
static uint16_t  g_term_w = 80, g_term_h = 24;
static TuiKeyParser g_keys;

// --- batched output ---------------------------------------------------------
//
// Everything a frame sends is gathered here and written once.
//
// This is not a micro-optimisation. out_write is an fwrite, and the diff below
// naturally produces ONE CHARACTER AT A TIME — a full first frame is around two
// thousand separate one-byte writes plus escape sequences, and the per-call
// cost dwarfs the bytes. It made the first paint take long enough that keys
// pressed during it were not read until it finished, which felt like an
// unresponsive terminal rather than a slow one.
//
// A frame that changes a few cells now costs a single write of a few dozen
// bytes, which is what makes the thing feel immediate.
#define TXBUF 1024
static char     g_tx[TXBUF];
static uint32_t g_tx_len;

static void tx_flush(void) {
    if (!g_tx_len) return;
    out_write(g_tx, g_tx_len);
    g_tx_len = 0;
    // fflush, and it is not optional.
    //
    // stdout is LINE buffered, and a frame of escape sequences contains no
    // newline at all — so without this a small update simply sat in the C
    // library's buffer until something later happened to fill it. Scrolling
    // produced enough output to keep forcing flushes and looked smooth, while a
    // single keypress produced a couple of hundred bytes that never left, which
    // read as the keypress being ignored.
    //
    // That is the whole "sometimes it does not respond" symptom.
    fflush(stdout);
}

static void tx(const char *data, uint32_t len) {
    if (len >= TXBUF) { tx_flush(); out_write(data, len); return; }
    if (g_tx_len + len > TXBUF) tx_flush();
    memcpy(g_tx + g_tx_len, data, len);
    g_tx_len += len;
}

static void tx_ch(char c) {
    if (g_tx_len + 1 > TXBUF) tx_flush();
    g_tx[g_tx_len++] = c;
}

// --- escape sequences -------------------------------------------------------

// Unbatched: used at begin and end, where the terminal must see the change
// before anything else happens.
static void esc(const char *s) { tx(s, (uint32_t)strlen(s)); tx_flush(); }

static void move_to(int x, int y) {
    char b[24];
    int n = snprintf(b, sizeof(b), "\033[%d;%dH", y + 1, x + 1);   // 1-based
    tx(b, (uint32_t)n);
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
    tx(b, (uint32_t)n);
    *cur_attr = attr;
    *cur_fg = fg;
}

// Ask the terminal how big it is.
//
// There is no other way to know: a serial line carries no window size, and
// assuming 80x24 wastes most of a maximised window and overdraws a small one.
// The trick is universal — park the cursor far beyond any real screen, ask
// where it ended up, and the answer is the bottom-right corner.
//
// Falls back to 80x24 on any terminal that does not answer, which costs one
// short wait at startup and never breaks anything.
static void query_size(uint16_t *w, uint16_t *h) {
    *w = 80; *h = 24;

    esc("\033[s\033[999;999H\033[6n");     // save, go far, report position

    // "\033[<rows>;<cols>R". Bounded by a deadline rather than a byte count,
    // since a terminal that does not implement this sends nothing at all.
    char buf[24];
    uint32_t n = 0;
    absolute_time_t deadline = make_timeout_time_ms(150);
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        int c = getchar_timeout_us(1000);
        if (c == PICO_ERROR_TIMEOUT) continue;
        if (n + 1 < sizeof(buf)) buf[n++] = (char)c;
        if (c == 'R') break;
    }
    esc("\033[u");                           // put the cursor back
    buf[n] = 0;

    const char *p = strchr(buf, '[');
    if (!p) return;
    int rows = atoi(p + 1);
    const char *semi = strchr(p, ';');
    int cols = semi ? atoi(semi + 1) : 0;

    // Believe it only if it is plausible. A garbled reply that parsed as 3x2
    // would be worse than the default.
    if (rows >= 5 && cols >= 20 && rows <= TUI_MAX_H && cols <= TUI_MAX_W) {
        *w = (uint16_t)cols;
        *h = (uint16_t)rows;
    }
}

// Ask again, for a window that changed size.
//
// A serial line carries no resize notification — there is no equivalent of
// SIGWINCH, so the only way to notice is to ask. Doing that on a timer would
// race with whatever the user is typing, since the reply arrives on the same
// input stream. So it happens on request: Ctrl+L, the same key every terminal
// program uses for "redraw, I do not trust what is on screen".
bool tuiterm_refresh(void) {
    if (!g_active) return false;
    uint16_t w = g_term_w, h = g_term_h;
    query_size(&w, &h);
    bool changed = (w != g_term_w || h != g_term_h);
    g_term_w = w; g_term_h = h;
    // Force the next present to send everything: after a resize the terminal's
    // contents bear no relation to what was drawn.
    if (g_shown) { g_shown->w = 0; g_shown->h = 0; }
    esc("\033[2J");
    return changed;
}

void tuiterm_size(uint16_t *w, uint16_t *h) {
    if (w) *w = g_term_w;
    if (h) *h = g_term_h;
}

void tuiterm_begin(void) {
    if (g_active) return;
    g_shown = (TuiScreen *)malloc(sizeof(TuiScreen));
    if (!g_shown) {
        // Refuse rather than half-start. A TUI with no previous frame to diff
        // against would repaint everything every time, which at 115200 baud is
        // slower than useless.
        out_err("Not enough memory for a full-screen app (%u KB needed).",
                (unsigned)(sizeof(TuiScreen) / 1024));
        return;
    }
    g_active = true;
    tuikey_init(&g_keys);

    esc("\033[?1049h");     // the alternate screen, so the shell scrollback survives
    esc("\033[?25l");       // hide the cursor; an app that wants one places it
    esc(TUI_MOUSE_ON);
    esc("\033[2J");

    // After the alternate screen is up, so the answer describes the screen the
    // app will actually draw on.
    query_size(&g_term_w, &g_term_h);

    // Force the first frame to send everything, by making the remembered screen
    // impossible to match.
    memset(g_shown, 0, sizeof(*g_shown));
    g_shown->w = 0; g_shown->h = 0;
}

void tuiterm_end(void) {
    if (!g_active) return;
    g_active = false;
    free(g_shown);
    g_shown = nullptr;
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

    bool full = (g_shown->w != s->w || g_shown->h != s->h);
    if (full) {
        tx("\033[2J", 4);
        g_shown->w = s->w;
        g_shown->h = s->h;
        for (uint32_t i = 0; i < (uint32_t)s->w * s->h; i++) {
            g_shown->cells[i].ch = 0;      // nothing can match, so everything sends
        }
    }

    uint8_t cur_attr = 0xff, cur_fg = 0xff;   // unknown, so the first cell sets it
    int last_x = -99, last_y = -99;

    for (int y = 0; y < (int)s->h; y++) {
        for (int x = 0; x < (int)s->w; x++) {
            uint32_t i = (uint32_t)y * s->w + x;
            const TuiCell &now = s->cells[i];
            TuiCell &was = g_shown->cells[i];
            if (now.ch == was.ch && now.attr == was.attr && now.fg == was.fg) continue;

            // Only move when the cursor is not already where it needs to be:
            // consecutive changed cells cost one byte each rather than eight.
            if (!(last_y == y && last_x == x)) { move_to(x, y); last_x = x; last_y = y; }
            apply_attr(now.attr, now.fg, &cur_attr, &cur_fg);
            tx_ch(now.ch >= 32 && now.ch < 127 ? now.ch : ' ');
            last_x++;
            was = now;
        }
    }
    // One write for the whole frame. Nothing above reached the wire until here.
    tx("\033[0m", 4);
    tx_flush();
}

void tuiterm_cursor(int x, int y, bool visible) {
    if (!g_active) return;
    if (visible) { move_to(x, y); esc("\033[?25h"); }
    else         esc("\033[?25l");
}

bool tuiterm_poll(TuiEvent *out) {
    if (!g_active || !out) return false;

    // Consume bytes until an event is COMPLETE, or the input runs dry.
    //
    // Reading one byte per call and returning false on a partial sequence was
    // wrong in a way that looked like flaky hardware. An arrow key is three
    // bytes and a mouse report is around eleven, so a caller's
    // `while (poll(&e))` loop exited part-way through one, drew a frame, and
    // came back later. If that took longer than the lone-Escape timeout, the
    // parser gave up mid-sequence and emitted an Escape, and the remaining
    // bytes — '[', '<', digits — arrived as literal keystrokes.
    //
    // That is why scrolling jumbled the screen and why keys sometimes did
    // nothing: the sequence was being torn in half by the frame in between.
    //
    // Bounded so a stream of bytes cannot hold the caller here forever; the
    // limit is far above any real sequence.
    for (int guard = 0; guard < 64; guard++) {
        uint32_t now = task_now_ms();
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            // A lone Escape only becomes an Escape once enough time has passed
            // without a following byte. Checking here is what makes the key
            // work rather than appearing dead until some later keypress.
            TuiEvent e = tuikey_timeout(&g_keys, now);
            if (e.kind != TUI_EV_NONE) { *out = e; return true; }
            return false;
        }
        TuiEvent e = tuikey_feed(&g_keys, (uint8_t)c, now);
        if (e.kind != TUI_EV_NONE) { *out = e; return true; }
    }
    return false;
}

bool tuiterm_active(void) { return g_active; }
