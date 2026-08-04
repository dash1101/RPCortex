// A 1-bit framebuffer, and the drawing on top of it.
//
// The Nova D1 GUI is built on MicroPython's `framebuf`, which this OS has no
// equivalent of — the TUI layer is text on a terminal, not pixels on a panel.
// So this is the one piece of the port that is genuinely new code rather than a
// door onto something already here.
//
// It lives in core/ because it is pure memory work: no pin, no bus, no clock.
// That means it is host-tested like everything else in core/, which matters
// more here than usual — a blit with the wrong stride draws something that
// looks almost right, and "almost right" is the hardest kind of wrong to see.
//
// MONO_VLSB, matching what the D1's panels want and what framebuf produced: one
// byte is EIGHT VERTICAL pixels, and the buffer is page-major. Pixel (x, y)
// lives in bit (y & 7) of byte (y >> 3) * width + x. Every operation below is
// that one line, arranged differently.
#ifndef RPC_FRAMEBUF_H
#define RPC_FRAMEBUF_H

#include <stdint.h>

struct FrameBuf {
    uint8_t *buf;
    int      w;
    int      h;
};

// Bytes a buffer of this size needs. Height rounds up to a whole page, because
// a 60-pixel-tall panel still needs the eighth row of the last page.
static inline int fb_bytes(int w, int h) { return w * ((h + 7) / 8); }

void fb_fill(FrameBuf *f, int colour);
void fb_pixel(FrameBuf *f, int x, int y, int colour);
int  fb_get(const FrameBuf *f, int x, int y);
void fb_hline(FrameBuf *f, int x, int y, int len, int colour);
void fb_vline(FrameBuf *f, int x, int y, int len, int colour);
void fb_line(FrameBuf *f, int x0, int y0, int x1, int y1, int colour);
void fb_rect(FrameBuf *f, int x, int y, int w, int h, int colour, int filled);
void fb_text(FrameBuf *f, const char *s, int x, int y, int colour);
int  fb_text_width(const char *s);
void fb_blit(FrameBuf *dst, const FrameBuf *src, int x, int y, int transparent);
void fb_scroll(FrameBuf *f, int dx, int dy);

#endif  // RPC_FRAMEBUF_H
