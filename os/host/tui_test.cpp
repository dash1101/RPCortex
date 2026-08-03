// The character grid.
//
// Every check here reads a rendered row back as a string. That is the reason
// the whole thing draws into memory: "does the box close", "is the long name
// visibly truncated", "is the selected row highlighted" have exact answers
// here, and no answer at all over a serial line someone has to squint at.
#include "tui.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { printf("    FAIL %s\n", what); fails++; }
}
static void eq_row(const TuiScreen *s, int y, const char *want, const char *what) {
    char got[TUI_MAX_W + 1];
    tui_row_text(s, y, got, sizeof(got));
    checks++;
    if (strcmp(got, want) != 0) {
        printf("    FAIL %s\n      want [%s]\n      got  [%s]\n", what, want, got);
        fails++;
    }
}

static TuiScreen g_s;

int main(void) {
    printf("  tui\n");
    TuiScreen *s = &g_s;

    // --- size ---------------------------------------------------------------
    tui_resize(s, 80, 24);
    ok(s->w == 80 && s->h == 24, "an ordinary size is kept");
    tui_resize(s, 5, 2);
    ok(s->w >= 20 && s->h >= 5, "a size too small to lay out is raised to a floor");
    tui_resize(s, 9999, 9999);
    ok(s->w == TUI_MAX_W && s->h == TUI_MAX_H, "an enormous size is clamped");

    tui_resize(s, 40, 10);

    // --- text ---------------------------------------------------------------
    tui_clear(s);
    tui_text(s, 0, 0, "hello", TUI_NORMAL, TUI_DEFAULT);
    eq_row(s, 0, "hello", "plain text");

    tui_clear(s);
    tui_text(s, 3, 1, "indented", TUI_NORMAL, TUI_DEFAULT);
    eq_row(s, 1, "   indented", "text at an offset keeps its leading space");

    {
        // Writing past the right edge must CLIP, not wrap. A wrap puts text on
        // the next line and reads as a layout bug somewhere else entirely.
        tui_clear(s);
        int n = tui_text(s, 36, 2, "abcdefgh", TUI_NORMAL, TUI_DEFAULT);
        ok(n == 4, "text past the edge reports what it wrote");
        eq_row(s, 2, "                                    abcd", "and clips rather than wrapping");
        eq_row(s, 3, "", "the next row is untouched");
    }
    {
        tui_clear(s);
        tui_text(s, -5, 0, "off", TUI_NORMAL, TUI_DEFAULT);
        tui_text(s, 0, -1, "off", TUI_NORMAL, TUI_DEFAULT);
        tui_text(s, 0, 99, "off", TUI_NORMAL, TUI_DEFAULT);
        eq_row(s, 0, "", "negative and out-of-range coordinates draw nothing");
    }

    // --- truncation ---------------------------------------------------------
    {
        tui_clear(s);
        tui_text_clip(s, 0, 0, "short", 10, TUI_NORMAL, TUI_DEFAULT);
        eq_row(s, 0, "short", "text that fits is untouched");

        tui_clear(s);
        tui_text_clip(s, 0, 0, "averylongfilename.txt", 10, TUI_NORMAL, TUI_DEFAULT);
        // A name that silently loses its end looks like a DIFFERENT name, which
        // is worse than one visibly shortened.
        eq_row(s, 0, "averylo...", "an over-long name is visibly marked as cut");
        {   // 10 columns total: seven characters and three dots.
            char got[TUI_MAX_W + 1]; tui_row_text(s, 0, got, sizeof(got));
            ok(strlen(got) == 10, "and occupies exactly the width it was given");
        }

        tui_clear(s);
        tui_text_clip(s, 0, 0, "abcdefgh", 2, TUI_NORMAL, TUI_DEFAULT);
        eq_row(s, 0, "..", "with no room for an ellipsis it degrades to dots");

        tui_clear(s);
        tui_text_clip(s, 0, 0, "abc", 0, TUI_NORMAL, TUI_DEFAULT);
        eq_row(s, 0, "", "zero width draws nothing");
    }

    // --- boxes --------------------------------------------------------------
    {
        tui_clear(s);
        tui_box(s, 0, 0, 10, 4, nullptr, TUI_NORMAL, TUI_DEFAULT);
        eq_row(s, 0, "+--------+", "box top");
        eq_row(s, 1, "|        |", "box side");
        eq_row(s, 3, "+--------+", "box bottom");
        eq_row(s, 4, "", "nothing below the box");
    }
    {
        tui_clear(s);
        tui_box(s, 0, 0, 20, 3, "Settings", TUI_NORMAL, TUI_DEFAULT);
        eq_row(s, 0, "+- Settings -------+", "a title is inlaid into the top edge");
    }
    {
        // A title longer than the edge must not escape the box.
        tui_clear(s);
        tui_box(s, 0, 0, 14, 3, "AVeryLongTitleIndeed", TUI_NORMAL, TUI_DEFAULT);
        char got[TUI_MAX_W + 1];
        tui_row_text(s, 0, got, sizeof(got));
        ok(strlen(got) == 14, "an over-long title stays inside the box");
        ok(got[13] == '+', "and the corner survives");
    }
    {
        tui_clear(s);
        tui_box(s, 0, 0, 1, 1, nullptr, TUI_NORMAL, TUI_DEFAULT);
        eq_row(s, 0, "", "a box too small to draw draws nothing");
    }

    // --- fill and attributes ------------------------------------------------
    {
        tui_clear(s);
        tui_text(s, 0, 0, "row one", TUI_NORMAL, TUI_DEFAULT);
        tui_text(s, 0, 1, "row two", TUI_NORMAL, TUI_DEFAULT);
        // Selection is drawn by filling the row in reverse, then writing over
        // it — so the highlight spans the full width, not just the text.
        tui_fill(s, 0, 1, s->w, 1, ' ', TUI_REVERSE, TUI_DEFAULT);
        tui_text(s, 0, 1, "row two", TUI_REVERSE, TUI_DEFAULT);

        ok(tui_at(s, 0, 1).attr & TUI_REVERSE, "the selected row is highlighted");
        ok(tui_at(s, 39, 1).attr & TUI_REVERSE, "the highlight reaches the right edge");
        ok(!(tui_at(s, 0, 0).attr & TUI_REVERSE), "the row above is not");
        eq_row(s, 1, "row two", "and the text is still readable");
    }
    {
        tui_clear(s);
        tui_text(s, 0, 0, "warn", TUI_BOLD, TUI_YELLOW);
        ok(tui_at(s, 0, 0).fg == TUI_YELLOW, "colour is recorded");
        ok(tui_at(s, 0, 0).attr & TUI_BOLD, "so is bold");
        ok(tui_at(s, 4, 0).fg == TUI_DEFAULT, "and stops where the text does");
    }

    // --- clear --------------------------------------------------------------
    {
        tui_text(s, 0, 0, "leftover", TUI_BOLD, TUI_RED);
        tui_clear(s);
        eq_row(s, 0, "", "clear empties the text");
        ok(tui_at(s, 0, 0).attr == 0 && tui_at(s, 0, 0).fg == 0,
           "and the attributes with it, so nothing bleeds into the next screen");
    }

    // --- a realistic panel --------------------------------------------------
    {
        // What `settings` will actually look like, asserted as text.
        tui_resize(s, 40, 10);
        tui_box(s, 0, 0, 40, 8, "Settings", TUI_NORMAL, TUI_CYAN);
        tui_text(s, 2, 2, "Device name", TUI_NORMAL, TUI_DEFAULT);
        tui_text(s, 20, 2, "vela", TUI_BOLD, TUI_DEFAULT);
        tui_fill(s, 1, 3, 38, 1, ' ', TUI_REVERSE, TUI_DEFAULT);
        tui_text(s, 2, 3, "Timezone", TUI_REVERSE, TUI_DEFAULT);
        tui_text(s, 20, 3, "UTC+0", TUI_REVERSE, TUI_DEFAULT);
        tui_text(s, 2, 9, "arrows move  enter edits  q quits", TUI_DIM, TUI_DEFAULT);

        eq_row(s, 0, "+- Settings ---------------------------+", "panel: titled frame");
        eq_row(s, 2, "| Device name       vela               |", "panel: an ordinary row");
        eq_row(s, 3, "| Timezone          UTC+0              |", "panel: the selected row");
        eq_row(s, 9, "  arrows move  enter edits  q quits", "panel: the hint line");
        ok(tui_at(s, 1, 3).attr & TUI_REVERSE, "panel: selection starts inside the border");
        ok(tui_at(s, 38, 3).attr & TUI_REVERSE, "panel: and runs to the far side");
        ok(tui_at(s, 0, 3).ch == '|', "panel: the border survives the highlight");
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
