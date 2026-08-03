// The scrolling list.
//
// All of this is arithmetic that every app otherwise reimplements slightly
// differently: keeping the cursor visible, paging without losing your place,
// and turning a click at row 7 into "item 19". Each is checked against a list
// long enough to scroll, because the interesting cases only exist there.
#include "tuilist.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { printf("    FAIL %s\n", what); fails++; }
}

static void label(void *, int i, char *out, uint32_t cap) {
    snprintf(out, cap, "item-%d", i);
}

static TuiScreen g_s;

int main(void) {
    printf("  tuilist\n");

    // --- movement -----------------------------------------------------------
    {
        TuiList l; tuilist_init(&l, 100, 10);
        ok(l.sel == 0 && l.top == 0, "starts at the top");

        tuilist_move(&l, 1);
        ok(l.sel == 1 && l.top == 0, "moving down inside the window does not scroll");

        for (int i = 0; i < 8; i++) tuilist_move(&l, 1);
        ok(l.sel == 9 && l.top == 0, "the last visible row still does not scroll");

        tuilist_move(&l, 1);
        ok(l.sel == 10 && l.top == 1, "moving past the bottom scrolls by one");

        tuilist_home(&l);
        ok(l.sel == 0 && l.top == 0, "home returns to the very top");

        tuilist_move(&l, -1);
        ok(l.sel == 99 && l.top == 90, "moving up from the top wraps to the end");

        tuilist_move(&l, 1);
        ok(l.sel == 0 && l.top == 0, "and wraps forward again");
    }
    {
        TuiList l; tuilist_init(&l, 100, 10);
        l.wrap = false;
        tuilist_move(&l, -1);
        ok(l.sel == 0, "with wrap off, up from the top stays put");
        tuilist_end(&l);
        tuilist_move(&l, 1);
        ok(l.sel == 99, "and down from the end stays put");
    }

    // --- paging -------------------------------------------------------------
    {
        TuiList l; tuilist_init(&l, 100, 10);
        tuilist_page(&l, 1);
        // One row of overlap, so there is always a line in common between the
        // old screen and the new one. Without it a reader loses their place.
        ok(l.sel == 9, "a page down keeps one row of overlap");
        tuilist_page(&l, 1);
        ok(l.sel == 18, "and again");
        tuilist_page(&l, -1);
        ok(l.sel == 9, "a page up is symmetric");

        for (int i = 0; i < 50; i++) tuilist_page(&l, 1);
        ok(l.sel == 99, "paging past the end clamps rather than wrapping");
        ok(l.top == 90, "and the view sits on the last full screen");
        for (int i = 0; i < 50; i++) tuilist_page(&l, -1);
        ok(l.sel == 0 && l.top == 0, "paging past the start clamps too");
    }

    // --- the wheel ----------------------------------------------------------
    {
        TuiList l; tuilist_init(&l, 100, 10);
        tuilist_scroll(&l, 3);
        // A wheel moves the VIEW, not the cursor — matching every other list a
        // person has used. The selection stays put even out of sight.
        ok(l.top == 3, "the wheel scrolls the view");
        ok(l.sel == 0, "and leaves the selection alone, even off screen");
        ok(tuilist_row_of_item(&l, 0) == -1, "the selection is genuinely off screen");

        tuilist_scroll(&l, -10);
        ok(l.top == 0, "scrolling up past the top stops there");
        tuilist_scroll(&l, 1000);
        ok(l.top == 90, "and down past the end stops at the last full screen");
    }

    // --- clicks -------------------------------------------------------------
    {
        TuiList l; tuilist_init(&l, 100, 10);
        l.top = 15;
        ok(tuilist_item_at_row(&l, 0) == 15, "row 0 is the first visible item");
        ok(tuilist_item_at_row(&l, 4) == 19, "a click four rows down is item 19");
        ok(tuilist_item_at_row(&l, 9) == 24, "the last row");
        ok(tuilist_item_at_row(&l, 10) == -1, "a click below the list is nothing");
        ok(tuilist_item_at_row(&l, -1) == -1, "and above it too");

        ok(tuilist_row_of_item(&l, 15) == 0,  "item 15 draws on row 0");
        ok(tuilist_row_of_item(&l, 24) == 9,  "item 24 on row 9");
        ok(tuilist_row_of_item(&l, 14) == -1, "an item scrolled off has no row");
    }
    {
        // A short list: rows past the end must be nothing, not the last item.
        TuiList l; tuilist_init(&l, 3, 10);
        ok(tuilist_item_at_row(&l, 2) == 2,  "the last real row");
        ok(tuilist_item_at_row(&l, 3) == -1, "a click on empty space selects nothing");
    }

    // --- edge cases ---------------------------------------------------------
    {
        TuiList l; tuilist_init(&l, 0, 10);
        tuilist_move(&l, 1); tuilist_page(&l, 1); tuilist_end(&l); tuilist_scroll(&l, 5);
        ok(l.sel == 0 && l.top == 0, "an empty list survives every operation");
        ok(tuilist_item_at_row(&l, 0) == -1, "and has nothing at row 0");
    }
    {
        // Items removed under a selection near the end — a delete in a file
        // picker. Nothing may point past the end afterwards.
        TuiList l; tuilist_init(&l, 100, 10);
        tuilist_end(&l);
        l.count = 5;
        tuilist_clamp(&l);
        ok(l.sel == 4, "the selection follows a shrunken list");
        ok(l.top == 0, "and the view stops showing blank rows");
    }
    {
        TuiList l; tuilist_init(&l, 5, 10);
        ok(l.top == 0, "a list shorter than the window never scrolls");
        tuilist_end(&l);
        ok(l.top == 0, "not even at its end");
    }

    // --- scrollbar ----------------------------------------------------------
    {
        TuiList l; tuilist_init(&l, 5, 10);
        ok(!tuilist_scrollbar(&l, nullptr, nullptr), "no scrollbar when everything fits");

        tuilist_init(&l, 100, 10);
        int row = -1, len = -1;
        ok(tuilist_scrollbar(&l, &row, &len), "a scrollbar when it does not");
        ok(len >= 1, "the thumb is always visible");
        ok(row == 0, "at the top it sits at the top");

        tuilist_end(&l);
        tuilist_scrollbar(&l, &row, &len);
        ok(row + len == l.rows, "at the end it sits flush against the bottom");
    }

    // --- drawing ------------------------------------------------------------
    {
        TuiScreen *s = &g_s;
        tui_resize(s, 40, 12);
        tui_clear(s);

        TuiList l; tuilist_init(&l, 100, 5);
        tuilist_move(&l, 2);
        tuilist_draw(&l, s, 0, 0, 20, label, nullptr, TUI_DEFAULT);

        char r[TUI_MAX_W + 1];
        tui_row_text(s, 0, r, sizeof(r)); ok(!strcmp(r, "item-0"), "draws the first item");
        tui_row_text(s, 2, r, sizeof(r)); ok(!strcmp(r, "item-2"), "draws the selected item");
        tui_row_text(s, 4, r, sizeof(r)); ok(!strcmp(r, "item-4"), "and the last visible one");

        ok(tui_at(s, 0, 2).attr & TUI_REVERSE,  "the selected row is highlighted");
        ok(tui_at(s, 19, 2).attr & TUI_REVERSE, "across the full width it was given");
        ok(!(tui_at(s, 20, 2).attr & TUI_REVERSE), "and no further");
        ok(!(tui_at(s, 0, 1).attr & TUI_REVERSE), "other rows are not highlighted");
    }
    {
        // A short list must clear the rows past its end rather than leaving
        // whatever was there before.
        TuiScreen *s = &g_s;
        tui_resize(s, 40, 12);
        tui_clear(s);
        tui_text(s, 0, 3, "STALE", TUI_NORMAL, TUI_DEFAULT);

        TuiList l; tuilist_init(&l, 2, 5);
        tuilist_draw(&l, s, 0, 0, 20, label, nullptr, TUI_DEFAULT);

        char r[TUI_MAX_W + 1];
        tui_row_text(s, 3, r, sizeof(r));
        ok(!strcmp(r, ""), "rows past the end are cleared, not left stale");
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
