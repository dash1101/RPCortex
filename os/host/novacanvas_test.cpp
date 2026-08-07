// The Nova D1 drawing surface, on the host.
//
// This is the file that makes the port checkable. novacanvas.cpp has no fw_*
// call in it, so the SAME code that draws the device's screens compiles and runs
// here — which means a screen can be rendered without hardware, compared against
// what the MicroPython suite drew, and any drift caught on a commit rather than
// on a bench.
//
// The checks below are about the primitives. Rendering whole screens comes with
// the screens.
#include "../apps/novad1/novacanvas.cpp"

#include <stdio.h>
#include <string.h>

static int checks, failures;

static void ok(bool cond, const char *what) {
    checks++;
    if (!cond) { failures++; printf("    FAIL %s\n", what); }
}

static void eq(int got, int want, const char *what) {
    checks++;
    if (got != want) { failures++; printf("    FAIL %s: got %d want %d\n", what, got, want); }
}

// A panel the size of the real one, so the arithmetic under test is the
// arithmetic the device does.
static uint8_t g_buf[128 * 8];
static nova::Canvas C;

static void fresh(void) {
    memset(g_buf, 0, sizeof(g_buf));
    C.attach(g_buf, 128, 64);
}

// How many pixels are lit. The cheapest way to assert that a fill covered what
// it should and nothing else.
static int lit(void) {
    int n = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++)
            n += C.get(x, y);
    return n;
}

// --- layout ------------------------------------------------------------------

static void test_layout(void) {
    fresh();
    eq(C.width(), 128, "width");
    eq(C.height(), 64, "height");
    eq(C.bytes(), 1024, "a 128x64 mono panel is 1 KB");
    eq(C.cols(), 21, "21 characters across at a 6-pixel advance");
}

// --- the byte layout ---------------------------------------------------------
//
// MONO_VLSB is the whole reason a finished frame can go to the panel as one
// write. If the bit for (x, y) is not where the controller expects it, every
// screen is subtly wrong in a way that looks like a font problem.

static void test_vlsb(void) {
    fresh();
    C.pixel(0, 0, 1);
    eq(g_buf[0], 0x01, "(0,0) is bit 0 of byte 0");

    fresh();
    C.pixel(0, 7, 1);
    eq(g_buf[0], 0x80, "(0,7) is bit 7 of byte 0");

    fresh();
    C.pixel(0, 8, 1);
    eq(g_buf[0], 0x00, "(0,8) has left the first page");
    eq(g_buf[128], 0x01, "(0,8) is bit 0 of the second page");

    fresh();
    C.pixel(127, 63, 1);
    eq(g_buf[1023], 0x80, "the last pixel is the top bit of the last byte");
}

// --- clipping ----------------------------------------------------------------
//
// Every one of these was a real class of bug on the MicroPython side: a row that
// wrapped onto the next line looks like a glitch rather than an out-of-range
// write, and can survive a long time before anyone works out what it is.

static void test_clipping(void) {
    fresh();
    C.pixel(-1, 0, 1); C.pixel(0, -1, 1); C.pixel(128, 0, 1); C.pixel(0, 64, 1);
    eq(lit(), 0, "pixels off every edge draw nothing");

    fresh();
    C.hline(-10, 5, 20, 1);
    eq(lit(), 10, "a line starting off the left is clipped, not wrapped");
    ok(C.get(0, 5) == 1 && C.get(9, 5) == 1 && C.get(10, 5) == 0, "and lands where it should");

    fresh();
    C.hline(120, 5, 20, 1);
    eq(lit(), 8, "a line running off the right stops at the edge");

    fresh();
    C.vline(5, -4, 10, 1);
    eq(lit(), 6, "a vertical line above the top is clipped");

    fresh();
    C.vline(5, 60, 10, 1);
    eq(lit(), 4, "and below the bottom");

    // A negative length is a line drawn backwards, which is what a caller doing
    // (x1 - x0) produces when the two are the other way round.
    fresh();
    C.hline(20, 5, -10, 1);
    eq(lit(), 10, "a negative length draws backwards rather than nothing");
    ok(C.get(10, 5) == 1 && C.get(19, 5) == 1, "starting where it should");
}

// --- runs --------------------------------------------------------------------
//
// hline and vline are written as byte and page runs rather than loops over
// pixel(), so they need checking against pixel() rather than against themselves.

static void test_runs(void) {
    fresh();
    C.vline(3, 2, 20, 1);
    bool same = true;
    for (int y = 0; y < 64; y++)
        if (C.get(3, y) != (y >= 2 && y < 22)) same = false;
    ok(same, "a vertical run spanning three pages sets exactly its own rows");
    eq(lit(), 20, "and nothing else");

    // Clearing has to leave the pixels either side alone: the page-mask
    // arithmetic is where an off-by-one takes out a neighbouring row.
    C.vline(3, 5, 4, 0);
    eq(lit(), 16, "clearing part of a run takes out exactly that part");
    ok(C.get(3, 4) == 1 && C.get(3, 5) == 0 && C.get(3, 8) == 0 && C.get(3, 9) == 1,
       "at both ends");
}

// --- boxes -------------------------------------------------------------------

static void test_boxes(void) {
    fresh();
    C.fill_rect(10, 10, 20, 10, 1);
    eq(lit(), 200, "a filled box is w * h pixels");
    ok(C.get(10, 10) && C.get(29, 19) && !C.get(30, 20), "with the right corners");

    fresh();
    C.rect(10, 10, 20, 10, 1);
    eq(lit(), 2 * 20 + 2 * 10 - 4, "an outline is its perimeter, corners not doubled");

    fresh();
    C.fill_rect(0, 0, 128, 64, 1);
    eq(lit(), 128 * 64, "a full-screen fill covers the panel");

    fresh();
    C.clear(1);
    eq(lit(), 128 * 64, "and so does clear(1)");
    C.clear(0);
    eq(lit(), 0, "clear(0) empties it");

    // Zero and negative sizes come from a list with nothing in it, and must draw
    // nothing rather than one stray pixel or a run the wrong way.
    fresh();
    C.fill_rect(10, 10, 0, 10, 1);
    C.fill_rect(10, 10, 10, 0, 1);
    C.rect(10, 10, -5, 10, 1);
    eq(lit(), 0, "an empty box draws nothing");
}

static void test_invert(void) {
    fresh();
    C.fill_rect(0, 0, 20, 10, 1);
    C.invert_rect(0, 0, 20, 10);
    eq(lit(), 0, "inverting a filled box empties it");
    C.invert_rect(0, 0, 20, 10);
    eq(lit(), 200, "and again fills it");

    // This is how a selected row is drawn, so it has to leave its neighbours
    // untouched — one row of bleed is instantly visible on a list.
    fresh();
    C.invert_rect(0, 9, 128, 9);
    ok(C.get(0, 8) == 0 && C.get(0, 9) == 1 && C.get(0, 17) == 1 && C.get(0, 18) == 0,
       "a selection bar covers its own rows only");
}

// --- text --------------------------------------------------------------------

static void test_text(void) {
    fresh();
    C.text(0, 0, "A", 1);
    ok(lit() > 0, "a letter draws something");

    // 6 pixels per cell, minus the trailing gap the last character does not
    // need. Centring against the full advance puts text a pixel left of centre,
    // which is visible at this size.
    eq(C.text_width("A", 1, false), 5, "one character is 5 wide, not 6");
    eq(C.text_width("AB", 1, false), 11, "two are 11");
    eq(C.text_width("", 1, false), 0, "and nothing is 0");
    eq(C.text_width("ABCDEFGHIJKLMNOPQRSTU", 1, false), 125, "21 characters fit in 128");

    eq(C.text_width("A", 2, false), 10, "scale doubles it");

    // Narrow drops each glyph's blank columns, so it depends on which
    // characters they are — an 'i' is much thinner than an 'm'.
    ok(C.text_width("iiii", 1, true) < C.text_width("iiii", 1, false),
       "narrow mode is narrower than fixed");
    ok(C.text_width("mmmm", 1, true) >= C.text_width("iiii", 1, true),
       "and proportional, not just uniformly smaller");

    // A space has no lit pixels at all, so its width cannot be measured from its
    // glyph — without a special case, narrow mode runs every word together.
    ok(C.text_width("a a", 1, true) > C.text_width("aa", 1, true),
       "a space still separates words in narrow mode");

    fresh();
    C.text(0, 0, "?", 1);
    int q = lit();
    fresh();
    C.text(0, 0, "\x01", 1);
    eq(lit(), q, "a character outside the face draws as '?'");
}

static void test_text_placement(void) {
    fresh();
    C.text_centred(0, "AB", 1);
    // 11 wide on a 128 panel: (128 - 11) / 2 = 58.
    ok(C.get(58, 0) || C.get(58, 1) || C.get(58, 2), "centred text starts where the maths says");

    fresh();
    C.text(126, 0, "ABCDEF", 1);
    ok(lit() > 0, "text starting near the edge draws its first glyph");
    for (int y = 0; y < 8; y++) ok(C.get(0, y) == 0, "and does not wrap to the left edge");

    // Fitting is what every list row does, and the two dots have to come out of
    // the budget BEFORE deciding how much fits or the result is a pixel too wide.
    fresh();
    int w = C.text_fit(0, 0, "a very long name indeed", 1, 40, false);
    ok(w <= 40, "text_fit stays inside its budget");
    ok(w > 0, "and draws something");

    fresh();
    w = C.text_fit(0, 0, "short", 1, 100, false);
    eq(w, C.text_width("short", 1, false), "a string that fits is drawn whole");
}

// --- affordances -------------------------------------------------------------

static void test_affordances(void) {
    fresh();
    C.scroll_tri(120, 10, 50, false, false);
    eq(lit(), 0, "no triangles when nothing is off-screen");

    fresh();
    C.scroll_tri(120, 10, 50, true, false);
    ok(lit() > 0, "an up triangle when there is more above");
    ok(C.get(120, 10) == 1, "pointing up");

    fresh();
    C.scrollbar(126, 10, 40, 0, 5, 5);
    eq(lit(), 0, "no scrollbar when everything is shown");

    fresh();
    C.scrollbar(126, 10, 40, 0, 5, 20);
    ok(lit() > 0, "a scrollbar when it is not");

    // The thumb has to move down as the list does, and stay inside the track at
    // both ends — an off-by-one here draws a pixel outside the panel.
    fresh(); C.scrollbar(126, 10, 40, 0, 5, 20);
    int top_first = -1;
    for (int y = 10; y < 50; y++) if (C.get(125, y)) { top_first = y; break; }
    fresh(); C.scrollbar(126, 10, 40, 15, 5, 20);
    int bot_first = -1;
    for (int y = 10; y < 50; y++) if (C.get(125, y)) { bot_first = y; break; }
    ok(top_first >= 10 && bot_first > top_first, "the thumb moves down with the list");
    ok(bot_first < 50, "and stays inside the track");
}

// --- circles -----------------------------------------------------------------

static void test_circles(void) {
    fresh();
    C.circle(64, 32, 10, 1);
    ok(lit() > 20, "a circle draws an outline");
    ok(C.get(64, 32) == 0, "which is hollow");

    fresh();
    C.fill_circle(64, 32, 10, 1);
    ok(C.get(64, 32) == 1, "a filled circle is not");
    // pi r^2 is 314; a raster disc is close but not exact, and the point of the
    // check is that it is a disc rather than that it is any particular size.
    int n = lit();
    ok(n > 250 && n < 400, "and covers about the right area");

    // Drawn at the edge it must clip rather than wrap. A radar sweep does this
    // constantly.
    fresh();
    C.fill_circle(0, 0, 10, 1);
    ok(lit() > 0, "a circle at the corner draws its visible quarter");
    ok(C.get(127, 0) == 0, "without wrapping to the far side");
}

int main(void) {
    test_layout();
    test_vlsb();
    test_clipping();
    test_runs();
    test_boxes();
    test_invert();
    test_text();
    test_text_placement();
    test_affordances();
    test_circles();

    printf("  %d checks", checks);
    if (failures) printf(", %d FAILED", failures);
    printf("\n");
    return failures ? 1 : 0;
}
