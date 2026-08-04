// The framebuffer, on the host.
//
// This is the piece of the Nova D1 port that is genuinely new code rather than
// a door onto something the OS already did, and it is pure memory work — no
// pin, no bus, no clock. So it is fully testable here, which matters more than
// usual: a blit with the wrong stride or a scroll that reads a pixel it has
// already written draws something that looks *almost* right, and almost right
// is the hardest kind of wrong to notice on a 128x64 panel.
#include "../core/framebuf.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

// Count the lit pixels, which is the cheapest way to assert that an operation
// touched what it should and nothing else.
static int lit(const FrameBuf *f) {
    int n = 0;
    for (int y = 0; y < f->h; y++)
        for (int x = 0; x < f->w; x++)
            if (fb_get(f, x, y)) n++;
    return n;
}

int main(void) {
    printf("framebuf_test - the 1-bit drawing layer\n");

    // A panel-shaped buffer, and one whose height is not a whole page — the
    // second is where a rounding mistake in fb_bytes would show up.
    ck(fb_bytes(128, 64) == 1024, "a 128x64 mono buffer is 1024 bytes");
    ck(fb_bytes(128, 60) == 1024, "and a 60-row one still needs eight whole pages");
    ck(fb_bytes(64, 8)   == 64,   "a single page is one byte per column");

    static uint8_t mem[1024];
    FrameBuf f{ mem, 128, 64 };

    // --- pixels -------------------------------------------------------------
    fb_fill(&f, 0);
    ck(lit(&f) == 0, "fill 0 clears everything");
    fb_fill(&f, 1);
    ck(lit(&f) == 128 * 64, "fill 1 lights everything");

    fb_fill(&f, 0);
    fb_pixel(&f, 0, 0, 1);
    ck(fb_get(&f, 0, 0) == 1, "the first pixel sets");
    ck(lit(&f) == 1, "and only that one");

    fb_pixel(&f, 127, 63, 1);
    ck(fb_get(&f, 127, 63) == 1, "so does the last");

    // MONO_VLSB packing: (0,0) and (0,8) are different PAGES of the same
    // column, so getting the shift wrong puts them in the same byte.
    fb_fill(&f, 0);
    fb_pixel(&f, 5, 0, 1);
    fb_pixel(&f, 5, 8, 1);
    ck(mem[5] == 0x01, "row 0 is bit 0 of page 0");
    ck(mem[128 + 5] == 0x01, "row 8 is bit 0 of page 1, not the same byte");

    fb_fill(&f, 0);
    fb_pixel(&f, 3, 7, 1);
    ck(mem[3] == 0x80, "row 7 is the top bit of page 0");

    // Off the edge is CLIPPED, never wrapped. A wrap would draw the right-hand
    // edge of a shape onto the left of the row below, which looks like a bug in
    // whatever drew it rather than in here.
    fb_fill(&f, 0);
    fb_pixel(&f, -1, 0, 1);
    fb_pixel(&f, 128, 0, 1);
    fb_pixel(&f, 0, -1, 1);
    fb_pixel(&f, 0, 64, 1);
    ck(lit(&f) == 0, "pixels outside the buffer are dropped, not wrapped");
    ck(fb_get(&f, -1, 0) == 0, "and reading outside gives 0 rather than reading memory");

    // --- lines --------------------------------------------------------------
    fb_fill(&f, 0);
    fb_hline(&f, 10, 20, 30, 1);
    ck(lit(&f) == 30, "a horizontal line is exactly its length");
    ck(fb_get(&f, 10, 20) && fb_get(&f, 39, 20), "spanning the ends given");
    ck(!fb_get(&f, 40, 20), "and stopping there");

    fb_fill(&f, 0);
    fb_vline(&f, 5, 5, 10, 1);
    ck(lit(&f) == 10, "a vertical line is exactly its length");
    // A vertical line crossing a page boundary is the case that catches a
    // packing mistake, since it spans two bytes of the same column.
    ck(fb_get(&f, 5, 7) && fb_get(&f, 5, 8), "and crosses a page boundary intact");

    fb_fill(&f, 0);
    fb_line(&f, 0, 0, 9, 9, 1);
    ck(lit(&f) == 10, "a 45-degree line is one pixel per step");
    ck(fb_get(&f, 0, 0) && fb_get(&f, 9, 9), "from end to end");

    fb_fill(&f, 0);
    fb_line(&f, 5, 5, 5, 5, 1);
    ck(lit(&f) == 1, "a line of no length is a single pixel, not an empty loop");

    // --- rectangles ---------------------------------------------------------
    fb_fill(&f, 0);
    fb_rect(&f, 10, 10, 20, 10, 1, /*filled*/0);
    ck(lit(&f) == 2 * 20 + 2 * 10 - 4, "an outline is its perimeter, corners not doubled");

    fb_fill(&f, 0);
    fb_rect(&f, 10, 10, 20, 10, 1, /*filled*/1);
    ck(lit(&f) == 200, "a filled rectangle is its area");

    fb_fill(&f, 0);
    fb_rect(&f, 0, 0, 0, 10, 1, 1);
    fb_rect(&f, 0, 0, 10, 0, 1, 1);
    ck(lit(&f) == 0, "a rectangle with no width or height draws nothing");

    // Partly off-screen: clipped, and the visible part still correct.
    fb_fill(&f, 0);
    fb_rect(&f, 120, 0, 20, 4, 1, 1);
    ck(lit(&f) == 8 * 4, "a rectangle over the edge draws only what fits");

    // --- text ---------------------------------------------------------------
    fb_fill(&f, 0);
    fb_text(&f, "A", 0, 0, 1);
    ck(lit(&f) > 0, "text draws something");
    int a_pixels = lit(&f);

    fb_fill(&f, 0);
    fb_text(&f, " ", 0, 0, 1);
    ck(lit(&f) == 0, "a space draws nothing");

    fb_fill(&f, 0);
    fb_text(&f, "AA", 0, 0, 1);
    ck(lit(&f) == a_pixels * 2, "two of the same character are twice the pixels");

    ck(fb_text_width("") == 0, "an empty string has no width");
    ck(fb_text_width("A") == 5, "one character is the glyph, with no trailing gap");
    ck(fb_text_width("AB") == 11, "two are both glyphs plus the gap between them");

    // A character the font does not have becomes '?' rather than reading past
    // the end of the table, which is the bug that would only show up as a
    // strange glyph or a crash much later.
    fb_fill(&f, 0);
    fb_text(&f, "\x01", 0, 0, 1);
    int fallback = lit(&f);
    fb_fill(&f, 0);
    fb_text(&f, "?", 0, 0, 1);
    ck(fallback == lit(&f), "an unknown character draws '?' rather than reading past the font");

    // --- blit ---------------------------------------------------------------
    static uint8_t small_mem[8];
    FrameBuf small{ small_mem, 8, 8 };
    fb_fill(&small, 1);

    fb_fill(&f, 0);
    fb_blit(&f, &small, 4, 4, /*transparent*/-1);
    ck(lit(&f) == 64, "a blit copies the whole source");
    ck(fb_get(&f, 4, 4) && fb_get(&f, 11, 11), "at the offset asked for");
    ck(!fb_get(&f, 3, 4) && !fb_get(&f, 12, 11), "and nowhere else");

    // Transparent 0: the lit pixels land, the clear ones leave what was there.
    fb_fill(&f, 1);
    fb_fill(&small, 0);
    fb_pixel(&small, 0, 0, 1);
    fb_blit(&f, &small, 0, 0, /*transparent*/0);
    ck(lit(&f) == 128 * 64, "a transparent blit leaves the background alone");

    // And without transparency the same blit punches a hole.
    fb_fill(&f, 1);
    fb_blit(&f, &small, 0, 0, /*transparent*/-1);
    ck(lit(&f) == 128 * 64 - 63, "an opaque blit replaces what it covers");

    // Off the edge, clipped rather than wrapped or overrunning.
    fb_fill(&f, 0);
    fb_fill(&small, 1);
    fb_blit(&f, &small, 124, 60, -1);
    ck(lit(&f) == 4 * 4, "a blit over the corner draws only the part that fits");

    // --- scroll -------------------------------------------------------------
    //
    // The one where reading a pixel already overwritten gives a smear rather
    // than an error, so both directions are checked.
    fb_fill(&f, 0);
    fb_hline(&f, 0, 0, 128, 1);
    fb_scroll(&f, 0, 1);
    ck(!fb_get(&f, 0, 0), "scrolling down clears the row it left");
    ck(fb_get(&f, 0, 1), "and the line moved");
    ck(lit(&f) == 128, "with nothing duplicated");

    fb_fill(&f, 0);
    fb_hline(&f, 0, 10, 128, 1);
    fb_scroll(&f, 0, -1);
    ck(fb_get(&f, 0, 9) && !fb_get(&f, 0, 10), "scrolling up moves it the other way");
    ck(lit(&f) == 128, "still nothing duplicated");

    fb_fill(&f, 0);
    fb_vline(&f, 0, 0, 64, 1);
    fb_scroll(&f, 1, 0);
    ck(fb_get(&f, 1, 0) && !fb_get(&f, 0, 0), "a sideways scroll moves and clears");
    ck(lit(&f) == 64, "and does not smear across the row");

    // Scrolling everything off leaves nothing behind.
    fb_fill(&f, 1);
    fb_scroll(&f, 0, 64);
    ck(lit(&f) == 0, "scrolling further than the buffer clears it");

    fb_fill(&f, 1);
    fb_scroll(&f, 0, 0);
    ck(lit(&f) == 128 * 64, "a scroll of nothing changes nothing");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
