// Desc: The UI leaf — the Screen protocol, the layout tokens, and the shared widgets.
// File: novaui.cpp
#include "novaui.h"

#include <string.h>
#include <stdio.h>

namespace nova {
namespace ui {

// --- Menu ---------------------------------------------------------------------

void Menu::set(const char *title, const MenuItem *items, int count) {
    title_ = title;
    items_ = items;
    count_ = count;
    sel_   = 0;
    top_   = 0;
}

void Menu::select(int i) {
    if (count_ <= 0) { sel_ = 0; return; }
    if (i < 0) i = 0;
    if (i >= count_) i = count_ - 1;
    sel_ = i;
}

void Menu::draw(Canvas &c) {
    if (!items_ || count_ <= 0) {
        c.text_centred(TOP + ROWH, "Nothing here", 1);
        return;
    }

    const int rows = rows_for(c);

    // Keep the selection in view. Done at draw time rather than on the event so
    // a menu whose selection was set from outside — restoring where somebody
    // was — scrolls to it rather than showing the top of the list.
    if (sel_ < top_)              top_ = sel_;
    else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
    if (top_ < 0) top_ = 0;

    const bool scrolls = count_ > rows;
    // The scrollbar lane is only taken when there is something to scroll, so a
    // short list gets the full width for its labels.
    const int right = scrolls ? c.width() - (SB_W + 1) : c.width();

    for (int i = 0; i < rows; i++) {
        const int idx = top_ + i;
        if (idx >= count_) break;
        const MenuItem &it = items_[idx];
        const int y = TOP + i * ROWH;
        // 12 pixels held back on the right: the '>' or 'x' marker, and the gap
        // before it. Without the gap a long label runs straight into the arrow
        // and reads as one word.
        const int avail = right - 12;

        if (idx == sel_) {
            // The selection is a filled bar with the corners knocked out and the
            // text inverted inside it. One selection indicator, everywhere.
            c.rounded_rect(0, y - 1, right, ROWH, 1, true);
            c.text_fit(4, y, it.label, 0, avail, false);
            if (it.fn) c.text(right - ADV - 2, y, ">", 0);
        } else {
            c.text_fit(4, y, it.label, 1, avail, false);
            // An inert row is marked when it is NOT selected, which is the
            // opposite way round from the arrow on purpose: the arrow says
            // "this goes somewhere" about the row you are on, and the cross says
            // "that one does not" about the rows you are not.
            if (!it.fn) c.text(right - ADV - 2, y, "x", 1);
        }
    }

    if (scrolls) c.scrollbar(c.width() - SB_W + 1, TOP, c.height() - TOP, top_, rows, count_);
}

Action Menu::on_event(Event e) {
    if (count_ <= 0) return Screen::on_event(e);
    switch (e) {
        case EV_ROT_CW:
            // Wraps. On a list of six with the cursor at the bottom, one more
            // click reaching the top is faster than five clicks back and reads
            // as a ring rather than a wall.
            sel_ = (sel_ + 1) % count_;
            return ACT_STAY;
        case EV_ROT_CCW:
            sel_ = (sel_ + count_ - 1) % count_;
            return ACT_STAY;
        case EV_SELECT:
            if (items_[sel_].fn) return items_[sel_].fn(items_[sel_].ctx, sel_);
            return ACT_STAY;
        default:
            return Screen::on_event(e);
    }
}

// --- the slider ----------------------------------------------------------------

void Slider::set(const char *title, int value, int lo, int hi, int step,
                 SliderFmt fmt, SliderFn on_change, void *ctx, SliderFn on_commit) {
    title_ = title; cb_ = on_change; commit_ = on_commit; ctx_ = ctx; fmt_ = fmt;
    stops_ = nullptr; n_ = 0; idx_ = 0; moved_ = false;
    lo_ = lo; hi_ = hi; step_ = step > 0 ? step : 1;
    val_ = value < lo ? lo : value > hi ? hi : value;
}

void Slider::set_stops(const char *title, int value, const int *stops, int n,
                       SliderFmt fmt, SliderFn on_change, void *ctx, SliderFn on_commit) {
    title_ = title; cb_ = on_change; commit_ = on_commit; ctx_ = ctx; fmt_ = fmt; moved_ = false;
    stops_ = stops; n_ = n > 0 ? n : 1; step_ = 1;
    lo_ = stops ? stops[0] : 0;
    hi_ = stops ? stops[n_ - 1] : 0;
    // Land on the stop nearest the current value, so opening the slider does not
    // silently move the setting.
    idx_ = 0;
    for (int i = 1; i < n_; i++)
        if (stops_[i] <= value) idx_ = i;
    val_ = stops_ ? stops_[idx_] : value;
}

void Slider::move(int dir) {
    if (stops_) {
        idx_ += dir;
        if (idx_ < 0) idx_ = 0;
        if (idx_ >= n_) idx_ = n_ - 1;
        val_ = stops_[idx_];
    } else {
        val_ += dir * step_;
        if (val_ < lo_) val_ = lo_;
        if (val_ > hi_) val_ = hi_;
    }
    moved_ = true;
    if (cb_) cb_(ctx_, val_);
}

// The one flush, on the way out. on_change has already put every step in the
// registry in RAM through the owner's callback; on_commit is the owner's
// flush-to-flash, called once and only if anything moved — so opening a slider
// and backing straight out costs nothing, and the widget never touches the
// registry itself.
void Slider::leave(void) {
    if (moved_ && commit_) commit_(ctx_, val_);
}

void Slider::fmt_value(char *out, unsigned cap) const {
    switch (fmt_) {
        case SL_PERCENT255:
            snprintf(out, cap, "%d%%", val_ * 100 / 255);
            break;
        case SL_SECONDS:
            if (!val_)               snprintf(out, cap, "never");
            else if (val_ % 60 == 0) snprintf(out, cap, "%dm", val_ / 60);
            else if (val_ < 60)      snprintf(out, cap, "%ds", val_);
            else                     snprintf(out, cap, "%dm%ds", val_ / 60, val_ % 60);
            break;
        default:
            snprintf(out, cap, "%d", val_);
            break;
    }
}

void Slider::draw(Canvas &c) {
    // The reading, big and centred, above the bar.
    char v[16];
    fmt_value(v, sizeof(v));
    c.text_centred(ui::TOP + 6, v, 1, 2, false);

    // The track, and the fill. The fraction is by STOP position when there are
    // stops — the gaps between timeouts are not equal in seconds and a bar that
    // tried to be would bunch the useful values into a corner — and by value
    // over the range otherwise.
    const int bx = 8, bw = c.width() - 16, bh = 9;
    const int by = c.height() - bh - 6;
    c.rounded_rect(bx, by, bw, bh, 1, false);

    int num, den;
    if (stops_) { num = idx_; den = n_ > 1 ? n_ - 1 : 1; }
    else        { num = val_ - lo_; den = hi_ > lo_ ? hi_ - lo_ : 1; }
    int fill = (bw - 4) * num / den;
    if (fill < 0) fill = 0;
    if (fill > bw - 4) fill = bw - 4;
    if (fill > 0) c.fill_rect(bx + 2, by + 2, fill, bh - 4, 1);
}

Action Slider::on_event(Event e) {
    if (e == EV_ROT_CW)  { move(+1); return ACT_STAY; }
    if (e == EV_ROT_CCW) { move(-1); return ACT_STAY; }
    return Screen::on_event(e);
}

// --- helpers -------------------------------------------------------------------

int wrap(const char *s, int cols, char *store, unsigned store_cap,
         const char **lines, int max_lines) {
    if (!s || !store || !lines || cols <= 0 || max_lines <= 0) return 0;

    unsigned at = 0;
    int n = 0;
    const char *p = s;

    while (*p && n < max_lines && at + 1 < store_cap) {
        // How much of what is left fits on a line.
        int take = 0;
        while (p[take] && p[take] != '\n' && take < cols) take++;

        if (p[take] && p[take] != '\n' && take == cols) {
            // Break at the last space rather than mid-word — but only if there
            // IS one on this line. A single word longer than the panel has to be
            // cut somewhere, and cutting it is better than an empty line
            // followed by the same problem.
            int sp = take;
            while (sp > 0 && p[sp] != ' ') sp--;
            if (sp > 0) take = sp;
        }

        unsigned len = (unsigned)take;
        if (at + len + 1 > store_cap) len = store_cap - at - 1;
        memcpy(store + at, p, len);
        store[at + len] = 0;
        lines[n++] = store + at;
        at += len + 1;

        p += take;
        while (*p == ' ') p++;          // the space we broke at is not shown
        if (*p == '\n') p++;
    }
    return n;
}

void heading(Canvas &c, const char *text) {
    c.text(0, TOP, text, 1);
    c.hline(0, TOP + FH + 1, c.width(), 1);
}

// --- the power indicator --------------------------------------------------------

// Digits at three by five, because the panel font does not fit inside a battery.
//
// A cell tall enough to hold a 5x7 glyph and its border would be nine rows and
// the status bar has eight before the rule, so the number inside had to be its
// own alphabet. Only the ten digits exist: this draws percentages and nothing
// else, and a general small font is a thing to add when something needs one.
//
// One byte a row, bit 2 leftmost, written in binary so each literal reads as the
// pixels it draws.
static const uint8_t kDigit[10][5] = {
    { 0b111, 0b101, 0b101, 0b101, 0b111 },   // 0
    { 0b010, 0b110, 0b010, 0b010, 0b111 },   // 1
    { 0b111, 0b001, 0b111, 0b100, 0b111 },   // 2
    { 0b111, 0b001, 0b111, 0b001, 0b111 },   // 3
    { 0b101, 0b101, 0b111, 0b001, 0b001 },   // 4
    { 0b111, 0b100, 0b111, 0b001, 0b111 },   // 5
    { 0b111, 0b100, 0b111, 0b101, 0b111 },   // 6
    { 0b111, 0b001, 0b001, 0b001, 0b001 },   // 7
    { 0b111, 0b101, 0b111, 0b101, 0b111 },   // 8
    { 0b111, 0b101, 0b111, 0b001, 0b111 },   // 9
};

constexpr int DIGIT_W = 3;
constexpr int DIGIT_H = 5;

// The cell, sized from the slot so the two cannot disagree: POWER_W is the shell
// plus its terminal plus the air before the clock.
constexpr int CELL_W = POWER_W - 3;
// Eight rows, which is one more than everything else in the bar and is the
// tallest a shell can be without touching the rule. The interior has to hold a
// five-row digit and a row of air, or the top of an 8 merges into the border and
// the number stops being a number.
constexpr int CELL_H = 8;

void power_badge(Canvas &c, int x, bool usb, int pct) {
    if (usb) {
        // A USB trident: the stem, the round tip, the square tip, the fork.
        // Centred in the slot the cell would have taken, so the bar does not
        // shift when a cable goes in.
        int sx = x + (CELL_W + 1 - 10) / 2;
        int y  = 4;
        c.hline(sx, y, 10, 1);
        c.fill_rect(sx, y - 1, 2, 3, 1);
        c.line(sx + 4, y, sx + 6, y - 3, 1);
        c.fill_rect(sx + 6, y - 4, 2, 2, 1);
        c.line(sx + 4, y, sx + 6, y + 3, 1);
        c.rect(sx + 6, y + 2, 2, 2, 1);
        return;
    }

    // The shell, corners knocked off, with its terminal on the right. A hard
    // rectangle in a bar of drawn shapes reads as a debug overlay.
    c.rounded_rect(x, 0, CELL_W, CELL_H, 1, false);
    c.fill_rect(x + CELL_W, 3, 1, 2, 1);

    if (pct < 0) return;        // powered, and nothing honest to say about how much
    if (pct > 100) pct = 100;

    // Inside the border: the interior the fill and the number share.
    const int ix = x + 1;
    const int iy = 1;
    const int iw = CELL_W - 2;
    const int ih = CELL_H - 2;

    const int fill_w = pct * iw / 100;
    if (fill_w > 0) c.fill_rect(ix, iy, fill_w, ih, 1);

    // The number, in the background colour where the fill is behind it and the
    // foreground colour where it is not — the way a phone does it, and the only
    // way it stays readable at both ends of the range on a panel with no greys.
    int d[3], n = 0;
    if      (pct >= 100) { d[n++] = 1; d[n++] = 0; d[n++] = 0; }
    else if (pct >= 10)  { d[n++] = pct / 10; d[n++] = pct % 10; }
    else                 { d[n++] = pct; }

    const int num_w = n * DIGIT_W + (n - 1);
    // Sat on the bottom of the interior rather than centred in it: the spare row
    // goes above, so the digits share a baseline with the border under them the
    // way text sits on a line. At 100% the number fills the width exactly, which
    // is the one case where every pixel of it is dark against a full fill and
    // has the border to lean on.
    int dx = ix + (iw - num_w) / 2;
    const int dy = iy + ih - DIGIT_H;

    // ONE COLOUR PER DIGIT, decided by where the digit's middle falls.
    //
    // Inverting per COLUMN is what a phone does and it is wrong here. A digit is
    // three pixels wide, so a fill edge landing inside one leaves it two pixels
    // knocked out and one solid — which at this size is not a damaged digit, it
    // is not a digit. Every level between about 35 and 75 per cent put the edge
    // through one.
    //
    // Choosing by the middle means the boundary does not cut a glyph, at the
    // cost of a digit that is very slightly the wrong colour for a pixel or two
    // of its width. That trade is only worth making because there are three
    // pixels: at any size where per-column inversion reads, it is the better
    // answer, and this comment is here so nobody 'fixes' it back on a bigger
    // panel without knowing why.
    const int fill_edge = ix + fill_w;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = kDigit[d[i]];
        const bool over_fill = (dx + DIGIT_W / 2) < fill_edge;
        const int colour = over_fill ? 0 : 1;
        for (int row = 0; row < DIGIT_H; row++) {
            for (int col = 0; col < DIGIT_W; col++) {
                if (!((g[row] >> (DIGIT_W - 1 - col)) & 1u)) continue;
                c.pixel(dx + col, dy + row, colour);
            }
        }
        dx += DIGIT_W + 1;
    }
}

}  // namespace ui
}  // namespace nova
