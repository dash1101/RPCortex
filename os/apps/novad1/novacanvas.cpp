// Desc: The 1-bit drawing surface — every pixel the device shows goes through here.
// File: novacanvas.cpp
#include "novacanvas.h"

#include <string.h>

namespace nova {

#include "novafont_data.inc"

#define FONT_FIRST 0x20
#define FONT_LAST  0x7e

// --- the surface ------------------------------------------------------------

void Canvas::attach(uint8_t *buf, int w, int h) {
    buf_   = buf;
    w_     = w;
    h_     = h;
    pages_ = (h + 7) / 8;
}

void Canvas::clear(int colour) {
    if (buf_) memset(buf_, colour ? 0xff : 0x00, (unsigned)bytes());
}

void Canvas::pixel(int x, int y, int colour) {
    if (!buf_ || x < 0 || y < 0 || x >= w_ || y >= h_) return;
    uint8_t *p = &buf_[(y >> 3) * w_ + x];
    uint8_t  m = (uint8_t)(1u << (y & 7));
    if (colour) *p = (uint8_t)(*p | m);
    else        *p = (uint8_t)(*p & ~m);
}

int Canvas::get(int x, int y) const {
    if (!buf_ || x < 0 || y < 0 || x >= w_ || y >= h_) return 0;
    return (buf_[(y >> 3) * w_ + x] >> (y & 7)) & 1;
}

// --- lines ------------------------------------------------------------------

void Canvas::hline(int x, int y, int len, int colour) {
    if (!buf_ || y < 0 || y >= h_) return;
    if (len < 0) { x += len; len = -len; }
    if (x < 0) { len += x; x = 0; }
    if (x + len > w_) len = w_ - x;
    if (len <= 0) return;
    // One row is one bit in each of `len` consecutive bytes, so this is a
    // straight run rather than a loop over pixel(). It is the single most
    // common primitive on the device — every rule, bar and selection uses it.
    uint8_t *p = &buf_[(y >> 3) * w_ + x];
    uint8_t  m = (uint8_t)(1u << (y & 7));
    if (colour) for (int i = 0; i < len; i++) p[i] = (uint8_t)(p[i] | m);
    else        for (int i = 0; i < len; i++) p[i] = (uint8_t)(p[i] & ~m);
}

void Canvas::vline(int x, int y, int len, int colour) {
    if (!buf_ || x < 0 || x >= w_) return;
    if (len < 0) { y += len; len = -len; }
    if (y < 0) { len += y; y = 0; }
    if (y + len > h_) len = h_ - y;
    if (len <= 0) return;

    // A vertical run is contiguous BITS within a byte, so whole pages can be
    // set at once instead of eight separate reads and writes.
    int y1 = y + len;
    while (y < y1) {
        int page   = y >> 3;
        int bottom = (page + 1) * 8;
        int upto   = y1 < bottom ? y1 : bottom;
        // Bits [y & 7, upto - page*8) within this page.
        uint8_t m = (uint8_t)(((1u << (upto - page * 8)) - 1u) & ~((1u << (y & 7)) - 1u));
        uint8_t *p = &buf_[page * w_ + x];
        if (colour) *p = (uint8_t)(*p | m);
        else        *p = (uint8_t)(*p & ~m);
        y = upto;
    }
}

void Canvas::line(int x0, int y0, int x1, int y1, int colour) {
    if (y0 == y1) { hline(x0, y0, x1 - x0 + (x1 >= x0 ? 1 : -1), colour); return; }
    if (x0 == x1) { vline(x0, y0, y1 - y0 + (y1 >= y0 ? 1 : -1), colour); return; }

    // Bresenham. Integer only — the ABI carries no floating point and this is
    // the reason it does not need to.
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        pixel(x0, y0, colour);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// --- boxes ------------------------------------------------------------------

void Canvas::rect(int x, int y, int w, int h, int colour) {
    if (w <= 0 || h <= 0) return;
    hline(x, y, w, colour);
    hline(x, y + h - 1, w, colour);
    vline(x, y, h, colour);
    vline(x + w - 1, y, h, colour);
}

void Canvas::fill_rect(int x, int y, int w, int h, int colour) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < w; i++) vline(x + i, y, h, colour);
}

void Canvas::rounded_rect(int x, int y, int w, int h, int colour, bool filled) {
    if (w <= 2 || h <= 2) { filled ? fill_rect(x, y, w, h, colour) : rect(x, y, w, h, colour); return; }
    if (filled) {
        fill_rect(x + 1, y, w - 2, h, colour);
        vline(x, y + 1, h - 2, colour);
        vline(x + w - 1, y + 1, h - 2, colour);
    } else {
        hline(x + 1, y, w - 2, colour);
        hline(x + 1, y + h - 1, w - 2, colour);
        vline(x, y + 1, h - 2, colour);
        vline(x + w - 1, y + 1, h - 2, colour);
    }
}

void Canvas::invert_rect(int x, int y, int w, int h) {
    if (!buf_) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > w_) w = w_ - x;
    if (y + h > h_) h = h_ - y;
    for (int j = 0; j < h; j++) {
        int row = y + j;
        if (row >= h_) break;
        uint8_t *p = &buf_[(row >> 3) * w_ + x];
        uint8_t  m = (uint8_t)(1u << (row & 7));
        for (int i = 0; i < w; i++) p[i] = (uint8_t)(p[i] ^ m);
    }
}

// --- circles ----------------------------------------------------------------

void Canvas::circle(int cx, int cy, int r, int colour) {
    if (r <= 0) { pixel(cx, cy, colour); return; }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        pixel(cx + x, cy + y, colour); pixel(cx + y, cy + x, colour);
        pixel(cx - y, cy + x, colour); pixel(cx - x, cy + y, colour);
        pixel(cx - x, cy - y, colour); pixel(cx - y, cy - x, colour);
        pixel(cx + y, cy - x, colour); pixel(cx + x, cy - y, colour);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void Canvas::fill_circle(int cx, int cy, int r, int colour) {
    if (r <= 0) { pixel(cx, cy, colour); return; }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        // Spans rather than pixels: two horizontal runs per octant pair covers
        // the disc without drawing any row twice, which on an inverting surface
        // would matter and here just costs time.
        hline(cx - x, cy + y, 2 * x + 1, colour);
        hline(cx - x, cy - y, 2 * x + 1, colour);
        hline(cx - y, cy + x, 2 * y + 1, colour);
        hline(cx - y, cy - x, 2 * y + 1, colour);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

// --- text -------------------------------------------------------------------

const uint8_t *Canvas::glyph(char c) const {
    unsigned u = (unsigned char)c;
    // Anything outside the face becomes '?'. Drawing nothing would silently
    // shorten a string and leave the reader wondering what was there.
    if (u < FONT_FIRST || u > FONT_LAST) u = '?';
    return &kFont5x7[(u - FONT_FIRST) * FONT_W];
}

void Canvas::glyph_span(char c, int *from, int *to) const {
    const uint8_t *g = glyph(c);
    int a = 0, b = FONT_W;
    while (a < FONT_W && g[a] == 0) a++;
    while (b > a && g[b - 1] == 0) b--;
    if (a == b) {           // a blank glyph, i.e. a space
        a = 0;
        b = 2;              // two columns, so words stay apart
    }
    *from = a;
    *to   = b;
}

void Canvas::ch(int x, int y, char c, int colour, int scale) {
    if (scale < 1) scale = 1;
    const uint8_t *g = glyph(c);
    for (int col = 0; col < FONT_W; col++) {
        uint8_t bits = g[col];
        if (!bits) continue;
        for (int row = 0; row < FONT_H; row++) {
            if (!((bits >> row) & 1)) continue;
            if (scale == 1) { pixel(x + col, y + row, colour); continue; }
            fill_rect(x + col * scale, y + row * scale, scale, scale, colour);
        }
    }
}

void Canvas::text(int x, int y, const char *s, int colour, int scale, bool narrow) {
    if (!s) return;
    if (scale < 1) scale = 1;
    for (; *s; s++) {
        if (x >= w_) break;          // nothing further can land on the panel
        if (!narrow) {
            ch(x, y, *s, colour, scale);
            x += FONT_ADV * scale;
            continue;
        }
        int from, to;
        glyph_span(*s, &from, &to);
        const uint8_t *g = glyph(*s);
        for (int col = from; col < to; col++) {
            uint8_t bits = g[col];
            if (!bits) continue;
            for (int row = 0; row < FONT_H; row++) {
                if (!((bits >> row) & 1)) continue;
                if (scale == 1) pixel(x + (col - from), y + row, colour);
                else fill_rect(x + (col - from) * scale, y + row * scale, scale, scale, colour);
            }
        }
        x += (to - from + 1) * scale;     // the glyph, plus one column of gap
    }
}

int Canvas::text_width(const char *s, int scale, bool narrow) const {
    if (!s || !*s) return 0;
    if (scale < 1) scale = 1;
    if (!narrow) {
        int n = (int)strlen(s);
        // The last character needs no trailing gap, so a run of n cells is
        // n * ADV - 1 wide. Centring against the full advance leaves text a
        // pixel left of where it should be, which is visible at this size.
        return (n * FONT_ADV - 1) * scale;
    }
    int px = 0;
    for (const char *p = s; *p; p++) {
        int from, to;
        glyph_span(*p, &from, &to);
        px += (to - from + 1) * scale;
    }
    return px > 0 ? px - scale : 0;
}

void Canvas::text_centred(int y, const char *s, int colour, int scale, bool narrow) {
    text_centred_in(0, w_, y, s, colour, scale, narrow);
}

void Canvas::text_centred_in(int x, int w, int y, const char *s, int colour,
                             int scale, bool narrow) {
    int tw = text_width(s, scale, narrow);
    int at = x + (w - tw) / 2;
    if (at < x) at = x;
    text(at, y, s, colour, scale, narrow);
}

int Canvas::text_fit(int x, int y, const char *s, int colour, int max_px, bool narrow) {
    if (!s) return 0;
    int full = text_width(s, 1, narrow);
    if (full <= max_px) { text(x, y, s, colour, 1, narrow); return full; }

    // Room for the two dots has to come out of the budget before deciding how
    // much of the string fits, or the result is one character too long.
    int dots = text_width("..", 1, narrow);
    int budget = max_px - dots;
    if (budget <= 0) return 0;

    char cut[48];
    unsigned n = 0;
    int px = 0;
    for (const char *p = s; *p && n + 3 < sizeof(cut); p++) {
        int adv;
        if (narrow) { int a, b; glyph_span(*p, &a, &b); adv = b - a + 1; }
        else        { adv = FONT_ADV; }
        if (px + adv > budget) break;
        cut[n++] = *p;
        px += adv;
    }
    cut[n++] = '.';
    cut[n++] = '.';
    cut[n]   = 0;
    text(x, y, cut, colour, 1, narrow);
    return text_width(cut, 1, narrow);
}

// --- affordances ------------------------------------------------------------

void Canvas::scroll_tri(int x, int top, int bottom, bool more_up, bool more_down) {
    // Three rows, narrowing. Small enough to sit in the margin beside a list
    // without stealing a character cell from it.
    if (more_up) {
        hline(x - 2, top + 2, 5, 1);
        hline(x - 1, top + 1, 3, 1);
        pixel(x, top, 1);
    }
    if (more_down) {
        hline(x - 2, bottom - 2, 5, 1);
        hline(x - 1, bottom - 1, 3, 1);
        pixel(x, bottom, 1);
    }
}

void Canvas::scrollbar(int x, int y, int h, int first, int shown, int total) {
    if (total <= shown || h <= 0) return;
    vline(x, y, h, 1);
    int thumb = shown * h / total;
    if (thumb < 3) thumb = 3;                  // always grabbable-looking
    if (thumb > h) thumb = h;
    int span = total - shown;
    int at   = span > 0 ? first * (h - thumb) / span : 0;
    fill_rect(x - 1, y + at, 3, thumb, 1);
}

void Canvas::spinner(int x, int y, unsigned phase, int colour) {
    // Four frames: | / - \, drawn as two-pixel strokes so they read at this size.
    switch (phase & 3) {
        case 0: vline(x + 2, y, 5, colour); break;
        case 1: line(x, y + 4, x + 4, y, colour); break;
        case 2: hline(x, y + 2, 5, colour); break;
        default: line(x, y, x + 4, y + 4, colour); break;
    }
}

}  // namespace nova
