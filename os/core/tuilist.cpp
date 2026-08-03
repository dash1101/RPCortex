#include "tuilist.h"

#include <string.h>
#include <stdio.h>

void tuilist_init(TuiList *l, int count, int rows) {
    l->count = count < 0 ? 0 : count;
    l->rows  = rows  < 1 ? 1 : rows;
    l->sel   = 0;
    l->top   = 0;
    l->wrap  = true;
    tuilist_clamp(l);
}

void tuilist_clamp(TuiList *l) {
    if (l->rows < 1) l->rows = 1;
    if (l->count <= 0) { l->count = 0; l->sel = 0; l->top = 0; return; }

    if (l->sel < 0) l->sel = 0;
    if (l->sel >= l->count) l->sel = l->count - 1;

    // Never scrolled so far that blank rows show below a list that could fill
    // them — the thing that makes a list look broken rather than short.
    int max_top = l->count - l->rows;
    if (max_top < 0) max_top = 0;
    if (l->top > max_top) l->top = max_top;
    if (l->top < 0) l->top = 0;

    // Follow the selection. Only when it has actually left the window, so a
    // wheel scroll that moved the view is not immediately undone.
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + l->rows) l->top = l->sel - l->rows + 1;
}

void tuilist_move(TuiList *l, int delta) {
    if (l->count <= 0) return;
    int n = l->sel + delta;
    if (l->wrap) {
        // Wrapping is what people expect from a short menu, and harmless on a
        // long one because reaching the end takes deliberate effort.
        while (n < 0) n += l->count;
        n %= l->count;
    }
    l->sel = n;
    tuilist_clamp(l);
}

void tuilist_page(TuiList *l, int dir) {
    if (l->count <= 0) return;
    // A page keeps one row of overlap, so there is always a line in common
    // between what was on screen and what now is. Without it a reader loses
    // their place on every press.
    int step = l->rows > 1 ? l->rows - 1 : 1;
    l->sel += dir * step;
    if (l->sel < 0) l->sel = 0;                       // pages clamp, never wrap
    if (l->sel >= l->count) l->sel = l->count - 1;
    tuilist_clamp(l);
}

void tuilist_home(TuiList *l) { l->sel = 0; l->top = 0; tuilist_clamp(l); }

void tuilist_end(TuiList *l) {
    if (l->count <= 0) return;
    l->sel = l->count - 1;
    tuilist_clamp(l);
}

void tuilist_scroll(TuiList *l, int delta) {
    if (l->count <= 0) return;
    int max_top = l->count - l->rows;
    if (max_top < 0) max_top = 0;
    l->top += delta;
    if (l->top < 0) l->top = 0;
    if (l->top > max_top) l->top = max_top;
    // Deliberately does NOT touch sel, and does not call clamp — a wheel moves
    // the view, not the cursor, and clamping here would drag the selection
    // along and undo the scroll.
}

int tuilist_item_at_row(const TuiList *l, int row) {
    if (row < 0 || row >= l->rows) return -1;
    int idx = l->top + row;
    return (idx >= 0 && idx < l->count) ? idx : -1;
}

int tuilist_row_of_item(const TuiList *l, int index) {
    if (index < l->top || index >= l->top + l->rows) return -1;
    return index - l->top;
}

bool tuilist_scrollbar(const TuiList *l, int *thumb_row, int *thumb_len) {
    if (l->count <= l->rows || l->rows <= 0) return false;

    int len = (l->rows * l->rows) / l->count;
    if (len < 1) len = 1;                              // always visible

    int span = l->rows - len;
    int max_top = l->count - l->rows;
    int row = max_top > 0 ? (l->top * span) / max_top : 0;
    if (row < 0) row = 0;
    if (row > span) row = span;

    if (thumb_row) *thumb_row = row;
    if (thumb_len) *thumb_len = len;
    return true;
}

void tuilist_draw(const TuiList *l, TuiScreen *s, int x, int y, int w,
                  TuiListLabel label, void *ctx, uint8_t fg) {
    char buf[TUI_MAX_W + 1];
    for (int r = 0; r < l->rows; r++) {
        int idx = tuilist_item_at_row(l, r);
        bool selected = (idx >= 0 && idx == l->sel);
        uint8_t attr = selected ? TUI_REVERSE : TUI_NORMAL;

        // Fill first, then write over it, so the highlight spans the full width
        // rather than stopping where the text does. A selection bar that ends
        // mid-row looks like a rendering fault.
        tui_fill(s, x, y + r, w, 1, ' ', attr, fg);
        if (idx < 0 || !label) continue;

        buf[0] = 0;
        label(ctx, idx, buf, sizeof(buf));
        tui_text_clip(s, x, y + r, buf, w, attr, fg);
    }
}
