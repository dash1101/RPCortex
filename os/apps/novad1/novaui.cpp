// Desc: The UI leaf — the Screen protocol, the layout tokens, and the shared widgets.
// File: novaui.cpp
#include "novaui.h"

#include <string.h>

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

}  // namespace ui
}  // namespace nova
