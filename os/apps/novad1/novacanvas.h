// Desc: The 1-bit drawing surface — every pixel the device shows goes through here.
// File: novacanvas.h
//
// MONO_VLSB, which is what the SH1106, SSD1306 and SSD1309 all want: one byte is
// eight VERTICAL pixels, page-major, so a finished buffer goes to the panel as
// one write with no repacking.
//
//     byte = (y >> 3) * w + x        bit = 1 << (y & 7)
//
// WHY THIS IS NOT fw_fb_*. The firmware exports a framebuffer with the same
// layout and it is a perfectly good one. This is a separate implementation on
// purpose, for two reasons that both matter more than the duplication costs:
//
//   * A supervisor call is 296 cycles. Drawing straight into memory the package
//     already owns costs none, and a screen is hundreds of primitives.
//   * The host renderer has no firmware. The pixel-diff suite that proves this
//     UI matches the MicroPython one it replaces has to run the SAME drawing
//     code the device runs, and fw_fb_* cannot be that code.
//
// So this file is pure logic over a byte array, with no fw_* call anywhere in
// it. That is what makes it testable on the host and identical on both.
#ifndef NOVA_CANVAS_H
#define NOVA_CANVAS_H

#include <stdint.h>

namespace nova {

// The font, as the layout tokens need it. Held here rather than in a separate
// module because every one of them is a property of the drawing surface, and a
// screen that positions text is asking the canvas, not a font file.
constexpr int FONT_W   = 5;      // glyph width in pixels
constexpr int FONT_H   = 7;      // glyph height
constexpr int FONT_ADV = 6;      // one character cell: the glyph plus its gap

class Canvas {
public:
    // The buffer belongs to the caller and must be at least w * ((h + 7) / 8)
    // bytes. Not allocated here: on the device it is a static in the GUI task's
    // own memory, and on the host it is a local. Keeping the lifetime out of
    // this class is what lets both be true.
    void attach(uint8_t *buf, int w, int h);

    uint8_t *buffer() const { return buf_; }
    int      width()  const { return w_; }
    int      height() const { return h_; }
    int      bytes()  const { return w_ * ((h_ + 7) / 8); }

    // Characters that fit across the panel, at the normal advance. Screens ask
    // this rather than assuming 21, so a wider panel needs no code change.
    int cols() const { return w_ / FONT_ADV; }

    // --- primitives ---------------------------------------------------------
    //
    // Colour is 1 (lit) or 0 (dark) throughout. Everything clips: a screen that
    // draws off the edge gets a shorter line, never a corrupted buffer or a
    // wrapped row, because on a 1-bit panel a wrapped row is not obviously
    // wrong at a glance and can survive a long time.

    void clear(int colour = 0);
    void pixel(int x, int y, int colour);
    int  get(int x, int y) const;

    void hline(int x, int y, int len, int colour);
    void vline(int x, int y, int len, int colour);
    void line(int x0, int y0, int x1, int y1, int colour);

    void rect(int x, int y, int w, int h, int colour);        // outline
    void fill_rect(int x, int y, int w, int h, int colour);
    // Rounded by one pixel at each corner. The whole visual difference between
    // a panel that looks designed and one that looks like a debug overlay, for
    // four pixels.
    void rounded_rect(int x, int y, int w, int h, int colour, bool filled = false);

    void circle(int cx, int cy, int r, int colour);
    void fill_circle(int cx, int cy, int r, int colour);

    // Flip every pixel in a box. How a selected row is drawn: a filled bar with
    // the text inverted inside it, rather than a second text colour.
    void invert_rect(int x, int y, int w, int h);

    // --- text ---------------------------------------------------------------
    //
    // `scale` repeats each pixel, so scale 2 is a 10x14 glyph on a 12-pixel
    // cell — the "one big value" archetype. `narrow` drops each glyph's blank
    // leading and trailing columns, which fits about a quarter more characters
    // on a line and is what long text screens use.

    void ch(int x, int y, char c, int colour, int scale = 1);
    void text(int x, int y, const char *s, int colour, int scale = 1, bool narrow = false);

    // How wide that string would be, in pixels. Always ask before centring:
    // with narrow set the answer depends on which characters they are.
    int text_width(const char *s, int scale = 1, bool narrow = false) const;

    // Centred horizontally within the panel, or within [x, x+w).
    void text_centred(int y, const char *s, int colour, int scale = 1, bool narrow = false);
    void text_centred_in(int x, int w, int y, const char *s, int colour,
                         int scale = 1, bool narrow = false);

    // Truncate to fit `max_px`, ending in ".." when something was cut. Returns
    // the width actually drawn.
    int text_fit(int x, int y, const char *s, int colour, int max_px, bool narrow = false);

    // --- affordances --------------------------------------------------------
    //
    // Shared so every list in the suite grows the same furniture, which is most
    // of what makes fifty screens feel like one device.

    // The little up/down triangles that say a list continues. Draws neither
    // when nothing is off-screen — an affordance that is always there stops
    // meaning anything.
    void scroll_tri(int x, int top, int bottom, bool more_up, bool more_down);

    // A proportional scrollbar down the right edge, for screens long enough
    // that "there is more" is not enough and "how much more" is the question.
    void scrollbar(int x, int y, int h, int first, int shown, int total);

    // A four-frame spinner, for work that has no percentage. `phase` is any
    // increasing number; it takes the remainder itself.
    void spinner(int x, int y, unsigned phase, int colour);

    // No default member initialisers — see the note in display.h. A Canvas is
    // usually a static, zero is exactly the unattached state, and a non-trivial
    // constructor on a static in this package would never run.
private:
    uint8_t *buf_;
    int      w_;
    int      h_;
    int      pages_;

    // The one place the glyph is fetched, so an out-of-range character produces
    // the same substitute everywhere.
    const uint8_t *glyph(char c) const;
    // Columns actually used by this glyph, for narrow mode: {first, last+1}.
    void glyph_span(char c, int *from, int *to) const;
};

}  // namespace nova

#endif  // NOVA_CANVAS_H
