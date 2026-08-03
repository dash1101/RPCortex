#include "tui.h"

#include <string.h>

#define MIN_W 20
#define MIN_H 5

static bool in(const TuiScreen *s, int x, int y) {
    return x >= 0 && y >= 0 && x < (int)s->w && y < (int)s->h;
}

void tui_resize(TuiScreen *s, uint16_t w, uint16_t h) {
    if (w > TUI_MAX_W) w = TUI_MAX_W;
    if (h > TUI_MAX_H) h = TUI_MAX_H;
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    s->w = w;
    s->h = h;
    tui_clear(s);
}

void tui_clear(TuiScreen *s) {
    uint32_t n = (uint32_t)s->w * s->h;
    for (uint32_t i = 0; i < n; i++) { s->cells[i].ch = ' '; s->cells[i].attr = 0; s->cells[i].fg = 0; }
}

void tui_put(TuiScreen *s, int x, int y, char ch, uint8_t attr, uint8_t fg) {
    // Ignore rather than wrap. A wrapped write lands on the wrong line and
    // reads as a layout bug somewhere else entirely.
    if (!in(s, x, y)) return;
    TuiCell &c = s->cells[(uint32_t)y * s->w + x];
    c.ch = ch; c.attr = attr; c.fg = fg;
}

int tui_text(TuiScreen *s, int x, int y, const char *str, uint8_t attr, uint8_t fg) {
    if (!str) return 0;
    int n = 0;
    // str[n], not *str: the pointer does not advance, so testing *str would
    // check the first character forever and read off the end of the string.
    for (; str[n] && x + n < (int)s->w; n++) tui_put(s, x + n, y, str[n], attr, fg);
    return n;
}

int tui_text_clip(TuiScreen *s, int x, int y, const char *str, int width,
                  uint8_t attr, uint8_t fg) {
    if (!str || width <= 0) return 0;
    int len = (int)strlen(str);
    if (len <= width) return tui_text(s, x, y, str, attr, fg);

    // Too long: show what fits and mark that it was cut. A name that silently
    // loses its end looks like a different name, which is worse than one
    // visibly shortened.
    if (width <= 3) {
        for (int i = 0; i < width; i++) tui_put(s, x + i, y, '.', attr, fg);
        return width;
    }
    for (int i = 0; i < width - 3; i++) tui_put(s, x + i, y, str[i], attr, fg);
    for (int i = 0; i < 3; i++) tui_put(s, x + width - 3 + i, y, '.', attr, fg);
    return width;
}

void tui_fill(TuiScreen *s, int x, int y, int w, int h, char ch,
              uint8_t attr, uint8_t fg) {
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            tui_put(s, x + c, y + r, ch, attr, fg);
}

void tui_box(TuiScreen *s, int x, int y, int w, int h, const char *title,
             uint8_t attr, uint8_t fg) {
    if (w < 2 || h < 2) return;

    // ASCII. Box-drawing characters require the terminal to agree about the
    // character set, and when it does not the panel becomes line noise — which
    // is a poor first impression from a device whose only interface is this.
    for (int c = 1; c < w - 1; c++) {
        tui_put(s, x + c, y, '-', attr, fg);
        tui_put(s, x + c, y + h - 1, '-', attr, fg);
    }
    for (int r = 1; r < h - 1; r++) {
        tui_put(s, x, y + r, '|', attr, fg);
        tui_put(s, x + w - 1, y + r, '|', attr, fg);
    }
    tui_put(s, x, y, '+', attr, fg);
    tui_put(s, x + w - 1, y, '+', attr, fg);
    tui_put(s, x, y + h - 1, '+', attr, fg);
    tui_put(s, x + w - 1, y + h - 1, '+', attr, fg);

    if (title && *title && w > 6) {
        // Inlaid with a space either side, clipped to what the edge can hold.
        int room = w - 6;
        tui_put(s, x + 2, y, ' ', attr, fg);
        int n = tui_text_clip(s, x + 3, y, title, room, (uint8_t)(attr | TUI_BOLD), fg);
        tui_put(s, x + 3 + n, y, ' ', attr, fg);
    }
}

TuiCell tui_at(const TuiScreen *s, int x, int y) {
    if (!in(s, x, y)) { TuiCell e{' ', 0, 0}; return e; }
    return s->cells[(uint32_t)y * s->w + x];
}

void tui_row_text(const TuiScreen *s, int y, char *out, uint32_t cap) {
    if (!cap) return;
    uint32_t n = 0;
    for (int x = 0; x < (int)s->w && n + 1 < cap; x++) out[n++] = tui_at(s, x, y).ch;
    // Trailing spaces carry no information and make every expected string in a
    // test padded to the screen width.
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}
