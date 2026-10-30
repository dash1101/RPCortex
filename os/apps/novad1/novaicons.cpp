// Desc: The app icons — drawn, not stored.
// File: novaicons.cpp
//
// Ported from the MicroPython suite's novaicons.py, shape for shape. These were
// not designed once: several went through two or three attempts on real hardware
// before they read at six pixels, and the notes on why each is the shape it is
// came with them. Redrawing them from scratch would have thrown that away, and
// "the icons look different" is exactly what happened the first time.
//
// Everything derives from `r`, because the home screen draws the centre icon at
// twelve and its neighbours at six. An icon with a fixed offset in it looks fine
// at one size and runs out through the side of its own box at the other.
#include "novaicons.h"

#include <string.h>

namespace nova {
namespace icons {

// --- helpers ------------------------------------------------------------------

// A rectangle with its corners knocked off. Everything on this panel is drawn
// from straight lines, so a screen full of hard-cornered boxes reads as
// unfinished; a single pixel off each corner softens it without costing
// legibility at neighbour size.
static void rbox(Canvas &c, int x, int y, int w, int h, int col = 1) {
    if (w < 4 || h < 4) { c.rect(x, y, w, h, col); return; }
    c.hline(x + 1, y, w - 2, col);
    c.hline(x + 1, y + h - 1, w - 2, col);
    c.vline(x, y + 1, h - 2, col);
    c.vline(x + w - 1, y + 1, h - 2, col);
}

// Upper semicircle, walked with Bresenham. Sampling a circle equation per column
// left gaps that turned to dust at neighbour size, which is what the WiFi and
// LoRa arcs used to do.
static void uarc(Canvas &c, int cx, int cy, int rr) {
    int x = 0, y = rr, d = 1 - rr;
    while (x <= y) {
        c.pixel(cx + x, cy - y, 1); c.pixel(cx - x, cy - y, 1);
        c.pixel(cx + y, cy - x, 1); c.pixel(cx - y, cy - x, 1);
        if (d < 0) d += 2 * x + 3;
        else { d += 2 * (x - y) + 5; y--; }
        x++;
    }
}

static int imax(int a, int b) { return a > b ? a : b; }

// An arc of a circle, only the part within about 50 degrees of the way it points. A full
// half circle is what a signal does not look like: it is a beam, and what leaves an aerial
// is a short curved wave on the side it faces. (dx, dy) is the facing: up is (0,-1).
static void arc_dir(Canvas &c, int cx, int cy, int rr, int dx, int dy) {
    int x = 0, y = rr, d = 1 - rr;
    while (x <= y) {
        const int pts[8][2] = { { x, y }, { -x, y }, { x, -y }, { -x, -y }, { y, x }, { -y, x }, { y, -x }, { -y, -x } };
        for (int i = 0; i < 8; i++) {
            const int px = pts[i][0], py = pts[i][1];
            const int ax = px < 0 ? -px : px, ay = py < 0 ? -py : py;
            bool in = false;
            if (dy < 0)      in = py < 0 && ax * 4 <= ay * 5;
            else if (dx > 0) in = px > 0 && ay * 4 <= ax * 5;
            else if (dx < 0) in = px < 0 && ay * 4 <= ax * 5;
            else if (dy > 0) in = py > 0 && ax * 4 <= ay * 5;
            if (in) c.pixel(cx + px, cy + py, 1);
        }
        if (d < 0) d += 2 * x + 3;
        else { d += 2 * (x - y) + 5; y--; }
        x++;
    }
}

// --- the one bitmap ------------------------------------------------------------
//
// A gear is the clearest case for pixel art over primitives: built from circles
// and blocks it kept reading as a revolver cylinder, because what makes a gear
// legible is the TEETH interrupting the rim silhouette at an exact pitch — a
// pixel-grid decision rather than a geometric one.
//
// pixelarticons' settings-cog (MIT, (c) 2019 Gerrit Halfmann), 24x24, one word
// per row, bit 0 leftmost.
#define GEAR_W 24
static const uint32_t kGear[24] = {
    0x007E00, 0x007E00, 0x3E667C, 0x3E667C,
    0x31E78C, 0x31E78C, 0x30000C, 0x0C0030,
    0x0C3C30, 0xFC3C3F, 0xFCC33F, 0xC0C303,
    0xC0C303, 0xFCC33F, 0xFC3C3F, 0x0C3C30,
    0x0C0030, 0x30000C, 0x31E78C, 0x31E78C,
    0x3E667C, 0x3E667C, 0x007E00, 0x007E00,
};

// Nearest-neighbour on purpose. The source is pixel art on a 24-grid, so
// smoothing it would undo the thing that makes it legible at this size.
static void blit(Canvas &c, const uint32_t *bits, int w, int cx, int cy, int side) {
    if (side < 3) side = 3;
    int x0 = cx - side / 2, y0 = cy - side / 2;
    for (int py = 0; py < side; py++) {
        uint32_t row = bits[(py * w) / side];
        if (!row) continue;
        for (int px = 0; px < side; px++)
            if ((row >> ((px * w) / side)) & 1u) c.pixel(x0 + px, y0 + py, 1);
    }
}

// --- sensors --------------------------------------------------------------------

static void ic_thermo(Canvas &c, int cx, int cy, int r) {       // DHT11
    int bulb = imax(2, r / 2);
    int by = cy + r - bulb;
    c.fill_circle(cx, by, bulb, 1);
    int tw = imax(2, bulb - 1);                 // tube width tracks the bulb
    int ty = cy - r;
    c.rect(cx - tw / 2, ty, tw + 1, by - ty, 1);
    for (int i = 0; i < 3; i++) {               // graduations on the right
        int yy = ty + 3 + i * imax(2, (by - ty - 5) / 3);
        if (yy < by - bulb) c.hline(cx + tw / 2 + 2, yy, imax(2, r / 3), 1);
    }
}

// A ring on a teardrop whose point reaches the bottom of the cell, so it reads
// as "planted" rather than as a balloon.
static void ic_pin(Canvas &c, int cx, int cy, int r) {          // GPS
    int hr = imax(2, (2 * r) / 5);
    int hy = cy - r + hr + 1;
    c.circle(cx, hy, hr, 1);
    c.circle(cx, hy, imax(1, hr / 2), 1);
    int tip = cy + r;
    c.line(cx - hr, hy + hr / 2, cx, tip, 1);
    c.line(cx + hr, hy + hr / 2, cx, tip, 1);
}

static void ic_battery(Canvas &c, int cx, int cy, int r) {
    c.rect(cx - r, cy - r / 2, 2 * r - 1, r, 1);
    c.fill_rect(cx + r - 1, cy - r / 4, 2, r / 2, 1);
    c.fill_rect(cx - r + 2, cy - r / 2 + 2, r, r - 4, 1);
}

static void ic_clock(Canvas &c, int cx, int cy, int r) {
    c.circle(cx, cy, r - 1, 1);
    c.line(cx, cy, cx, cy - (r - 4), 1);        // hour hand, up
    c.line(cx, cy, cx + (r - 3), cy, 1);        // minute hand, right
    c.pixel(cx, cy, 1);
}

// --- radios ----------------------------------------------------------------------

// Three solid arcs over a dot.
static void ic_wifi(Canvas &c, int cx, int cy, int r) {
    int base = cy + r - 1;
    c.fill_circle(cx, base, imax(1, r / 5), 1);
    int step = imax(2, r / 3);
    for (int i = 1; i <= 3; i++) uarc(c, cx, base, i * step);
}

static void ic_bt(Canvas &c, int cx, int cy, int r) {
    c.vline(cx, cy - r, 2 * r, 1);
    c.line(cx, cy - r, cx + r / 2, cy - r / 2, 1);
    c.line(cx + r / 2, cy - r / 2, cx, cy, 1);
    c.line(cx, cy, cx + r / 2, cy + r / 2, 1);
    c.line(cx + r / 2, cy + r / 2, cx, cy + r, 1);
    c.line(cx - r / 2, cy - r / 2, cx + r / 2, cy + r / 2, 1);
    c.line(cx - r / 2, cy + r / 2, cx + r / 2, cy - r / 2, 1);
}

// A key fob with its signal. Sub-GHz is garage doors, gates and fobs, so the icon is the thing
// people actually point at them; the LoRa aerial beside it is a different silhouette.
static void ic_radio(Canvas &c, int cx, int cy, int r) {        // sub-GHz
    if (r >= 9) {
        rbox(c, cx - 8, cy - 4, 9, 15);                          // the body
        c.circle(cx - 4, cy - 6, 2, 1);                          // the ring it hangs from
        c.fill_rect(cx - 6, cy - 1, 5, 3, 1);                    // two buttons
        c.fill_rect(cx - 6, cy + 4, 5, 3, 1);
        arc_dir(c, cx - 2, cy + 3, 5, 1, 0);                     // and what comes off it
        arc_dir(c, cx - 2, cy + 3, 9, 1, 0);
    } else {
        rbox(c, cx - 5, cy - 2, 5, 8);
        c.pixel(cx - 3, cy - 3, 1);
        c.fill_rect(cx - 4, cy, 3, 1, 1);
        c.fill_rect(cx - 4, cy + 3, 3, 1, 1);
        arc_dir(c, cx - 1, cy + 2, 3, 1, 0);
        arc_dir(c, cx - 1, cy + 2, 5, 1, 0);
    }
}

// A straight rod on a foot, radiating upward. The tower shape is the sub-GHz
// one; the two have to be told apart at neighbour size, so they do not share a
// silhouette.
static void ic_antenna(Canvas &c, int cx, int cy, int r) {      // LoRa
    int base = cy + r - 1, tip = cy - r / 3;
    c.vline(cx, tip, base - tip, 1);
    c.hline(cx - imax(2, r / 3), base, 2 * imax(2, r / 3) + 1, 1);
    c.fill_circle(cx, tip, 1, 1);
    for (int i = 1; i <= 2; i++) uarc(c, cx, tip, i * imax(2, r / 3));
}

static void ic_nfc(Canvas &c, int cx, int cy, int r) {
    int w = r;                                  // card occupies the left half
    rbox(c, cx - r, cy - r / 2, w, r);
    c.hline(cx - r + 2, cy - r / 4, imax(2, w / 2), 1);         // chip
    for (int i = 1; i <= 3; i++) {                              // waves off the right
        int rr = (r * i) / 3;
        c.line(cx + rr / 2, cy - rr, cx + rr, cy, 1);
        c.line(cx + rr, cy, cx + rr / 2, cy + rr, 1);
    }
}

// A TV remote with its signal leaving the top. The receiver and the transmitter used to be two
// drawings (bars arriving, a fan leaving) and neither read as anything; the Hardware screen
// lists them as text, and the IR app is one icon.
static void ic_ir(Canvas &c, int cx, int cy, int r) {
    if (r >= 9) {
        rbox(c, cx - 5, cy - 2, 10, 14);
        c.fill_rect(cx - 2, cy - 1, 4, 1, 1);                    // the window the light leaves by
        c.fill_rect(cx - 1, cy + 2, 2, 2, 1);                    // a round button
        for (int row = 0; row < 2; row++) {
            c.fill_rect(cx - 3, cy + 5 + row * 3, 2, 2, 1);
            c.fill_rect(cx + 1, cy + 5 + row * 3, 2, 2, 1);
        }
        arc_dir(c, cx, cy - 2, 4, 0, -1);
        arc_dir(c, cx, cy - 2, 8, 0, -1);
    } else {
        rbox(c, cx - 3, cy - 1, 6, 7);
        c.pixel(cx, cy + 1, 1);
        c.pixel(cx - 1, cy + 3, 1);
        c.pixel(cx + 1, cy + 3, 1);
        arc_dir(c, cx, cy - 1, 2, 0, -1);
        arc_dir(c, cx, cy - 1, 4, 0, -1);
    }
}
static void ic_ir_rx(Canvas &c, int cx, int cy, int r) { ic_ir(c, cx, cy, r); }
static void ic_ir_tx(Canvas &c, int cx, int cy, int r) { ic_ir(c, cx, cy, r); }

// A rounded speech bubble.
static void ic_chat(Canvas &c, int cx, int cy, int r) {         // Messages
    int w = 2 * r, h = (13 * r) / 10;
    int x = cx - r, y = cy - r;
    c.hline(x + 1, y, w - 2, 1);
    c.hline(x + 1, y + h, w - 2, 1);
    c.vline(x, y + 1, h - 1, 1);
    c.vline(x + w - 1, y + 1, h - 1, 1);
    int tail = imax(2, r / 3);
    c.line(x + tail, y + h, x + tail, y + h + tail, 1);
    c.line(x + tail, y + h + tail, x + tail * 2 + 1, y + h, 1);
    for (int i = 1; i <= 3; i++) c.pixel(x + (w / 4) * i, y + h / 2, 1);
}

// A round scope, not more arcs: at neighbour size an arc version is
// indistinguishable from the WiFi and LoRa icons, which sit next to it.
static void ic_radar(Canvas &c, int cx, int cy, int r) {
    c.circle(cx, cy, r - 1, 1);
    c.circle(cx, cy, imax(2, (r - 1) / 2), 1);
    c.pixel(cx, cy, 1);
    int e = r - 2;
    c.line(cx, cy, cx + e, cy - e, 1);          // the sweep arm, two lines thick
    c.line(cx, cy, cx + e - 1, cy - e, 1);
    c.fill_circle(cx - imax(2, r / 3), cy + imax(2, r / 3), imax(1, r / 4), 1);
}

static void ic_person(Canvas &c, int cx, int cy, int r) {       // Presence: somebody, and being noticed
    if (r >= 9) {
        c.fill_circle(cx, cy - 4, 3, 1);                         // head
        for (int y = cy + 1; y <= cy + 10; y++) {                // shoulders, filled
            int dy = cy + 10 - y, w = 0;
            while ((w + 1) * (w + 1) + dy * dy <= 64) w++;
            c.hline(cx - w, y, 2 * w + 1, 1);
        }
        arc_dir(c, cx, cy - 4, 7, -1, 0);                        // waves either side
        arc_dir(c, cx, cy - 4, 10, -1, 0);
        arc_dir(c, cx, cy - 4, 7, 1, 0);
        arc_dir(c, cx, cy - 4, 10, 1, 0);
    } else {
        c.fill_circle(cx, cy - 2, 2, 1);
        for (int y = cy + 1; y <= cy + 5; y++) {
            int dy = cy + 5 - y, w = 0;
            while ((w + 1) * (w + 1) + dy * dy <= 16) w++;
            c.hline(cx - w, y, 2 * w + 1, 1);
        }
        arc_dir(c, cx, cy - 2, 5, -1, 0);
        arc_dir(c, cx, cy - 2, 5, 1, 0);
    }
}

static void ic_wardrive(Canvas &c, int cx, int cy, int r) {      // a car, and what it is listening to
    if (r >= 9) {
        rbox(c, cx - 9, cy + 1, 18, 6);                          // the body
        c.line(cx - 5, cy + 1, cx - 3, cy - 3, 1);               // the cabin
        c.hline(cx - 3, cy - 3, 7, 1);
        c.line(cx + 4, cy - 3, cx + 6, cy + 1, 1);
        c.fill_circle(cx - 5, cy + 8, 2, 1);                     // wheels
        c.fill_circle(cx + 5, cy + 8, 2, 1);
        c.fill_circle(cx, cy - 6, 1, 1);                         // and the wifi it is hearing
        arc_dir(c, cx, cy - 6, 4, 0, -1);
        arc_dir(c, cx, cy - 6, 7, 0, -1);
    } else {
        rbox(c, cx - 5, cy + 0, 10, 4);
        c.hline(cx - 2, cy - 2, 5, 1);
        c.pixel(cx - 3, cy - 1, 1); c.pixel(cx + 3, cy - 1, 1);
        c.pixel(cx - 3, cy + 5, 1); c.pixel(cx + 3, cy + 5, 1);
        c.pixel(cx, cy - 4, 1);
        arc_dir(c, cx, cy - 3, 3, 0, -1);
    }
}

// --- tools ------------------------------------------------------------------------

static void ic_doc(Canvas &c, int cx, int cy, int r) {          // Logs
    int w = (14 * r) / 10, h = 2 * r;
    int x = cx - w / 2, y = cy - r;
    int fold = imax(2, r / 3);
    c.hline(x, y, w - fold, 1);
    c.line(x + w - fold, y, x + w - 1, y + fold, 1);            // folded corner
    c.vline(x + w - 1, y + fold, h - fold, 1);
    c.vline(x, y, h, 1);
    c.hline(x, y + h - 1, w, 1);
    for (int i = 0; i < 3; i++) {                               // text lines
        int yy = y + fold + 2 + i * imax(2, (h - fold - 4) / 3);
        if (yy < y + h - 2) c.hline(x + 2, yy, w - 5, 1);
    }
}

// Two thin rolls with a written sheet between. The lines of writing are short
// and RAGGED — even-length full-width rules read as slats and the whole icon
// turns into a window blind.
static void ic_scroll(Canvas &c, int cx, int cy, int r) {       // Scripts
    int w = 2 * r - 3;
    int x = cx - w / 2;
    int top = cy - r + 1, bot = cy + r - 3;
    rbox(c, x, top, w, 3);                      // top roll
    rbox(c, x, bot, w, 3);                      // bottom roll
    c.vline(x + 1, top + 3, bot - top - 3, 1);  // the sheet
    c.vline(x + w - 2, top + 3, bot - top - 3, 1);
    int inner = bot - top - 4;
    int n = inner / 5; if (n < 1) n = 1; if (n > 3) n = 3;
    for (int i = 0; i < n; i++) {
        int yy = top + 6 + i * (inner / n);
        if (yy < bot - 1) {
            int ln = (i < n - 1) ? w - 6 : (w - 6) / 2;
            c.hline(x + 3, yy, imax(2, ln), 1);
        }
    }
}

// A terminal window. The prompt is drawn as lines so it scales with r.
static void ic_terminal(Canvas &c, int cx, int cy, int r) {     // Shell
    rbox(c, cx - r, cy - r + 1, 2 * r, 2 * r - 2);
    c.hline(cx - r, cy - r + 1 + imax(2, r / 3), 2 * r, 1);     // title bar
    int k = imax(2, r / 3);
    int px = cx - r + imax(2, r / 3), py = cy;
    c.line(px, py - k, px + k, py, 1);
    c.line(px + k, py, px, py + k, 1);
    c.hline(px + k + 2, py + k, imax(2, r - k), 1);             // cursor
}

// Distinct from the shell terminal on purpose. Commands is a curated LIST you
// pick from; the shell is a place you type. Drawing both as a terminal window
// made them the same icon. A bullet marks the chosen row rather than a chevron:
// at r = 6 a chevron is three pixels of diagonal and merges into the rows beside
// it, while a filled square stays a square all the way down.
static void ic_cmdlist(Canvas &c, int cx, int cy, int r) {      // Commands
    rbox(c, cx - r, cy - r + 1, 2 * r, 2 * r - 2);
    int step = imax(2, (2 * r - 6) / 3);
    int y = cy - r + 3;
    int b = imax(1, r / 3);
    int x0 = cx - r + 3;
    for (int i = 0; i < 3; i++) {
        if (i == 1) {
            c.fill_rect(x0, y - b / 2, b, b, 1);
            c.hline(x0 + b + 2, y, imax(2, 2 * r - 8 - b), 1);
        } else {
            c.hline(x0, y, imax(3, 2 * r - 6), 1);
        }
        y += step;
    }
}

// Bars rather than a dial: a needle needs an arc, and every arc-based icon on
// this panel already competes for the same silhouette. Three rising bars read as
// "measurements" and survive losing a pixel.
// A hub with two machines hanging off it. Filled blocks rather than outlines:
// at neighbour size an outlined three-pixel box is a ring of dust, and what has
// to survive the shrink is that there are THREE of them and a line joining them.
static void ic_lan(Canvas &c, int cx, int cy, int r) {          // LAN
    const int n   = imax(2, (2 * r) / 5);       // a node is n square
    const int top = cy - r + 1;
    const int bot = cy + r - 1 - n;
    const int mid = (top + n + bot) / 2;        // the bus the drops hang off
    const int x   = cx - r + 1;
    const int w   = 2 * r - 2;
    c.fill_rect(cx - n / 2, top, n, n, 1);      // the hub
    c.vline(cx, top + n, mid - top - n + 1, 1);
    c.hline(x, mid, w, 1);                      // the bus
    c.vline(x, mid, bot - mid, 1);              // and its two drops
    c.vline(x + w - 1, mid, bot - mid, 1);
    c.fill_rect(x, bot, n, n, 1);
    c.fill_rect(x + w - n, bot, n, n, 1);
}

static void ic_bars(Canvas &c, int cx, int cy, int r) {         // Resources
    int base = cy + r - 1;
    c.hline(cx - r, base, 2 * r, 1);            // the axis they stand on
    int w = imax(1, (2 * r) / 5);
    int gap = imax(1, w / 2);
    int x = cx - r + 1;
    for (int i = 0; i < 3; i++) {
        int h = imax(2, (r * 2 * (i + 2)) / 8);
        c.fill_rect(x, base - h, w, h, 1);
        x += w + gap;
    }
}

static void ic_wrench(Canvas &c, int cx, int cy, int r) {       // Tools: a spanner, which a magnifier is not
    if (r >= 9) {
        for (int i = -1; i <= 1; i++) c.line(cx - 8 + (i < 0 ? 0 : 0), cy + 8 + i, cx + 2, cy - 2 + i, 1);
        c.line(cx - 8, cy + 9, cx + 1, cy - 2, 1);
        c.fill_circle(cx + 5, cy - 5, 5, 1);                     // the jaw
        c.fill_circle(cx + 5, cy - 5, 2, 0);                     // hollowed
        c.fill_rect(cx + 5, cy - 11, 7, 6, 0);                   // opened to the corner
        c.fill_circle(cx - 8, cy + 8, 2, 1);                     // the other end, with its hole
        c.pixel(cx - 8, cy + 8, 0);
    } else {
        c.line(cx - 4, cy + 4, cx + 1, cy - 1, 1);
        c.line(cx - 4, cy + 5, cx + 1, cy, 1);
        c.fill_circle(cx + 2, cy - 2, 2, 1);
        c.fill_rect(cx + 2, cy - 5, 4, 3, 0);
        c.pixel(cx + 2, cy - 2, 0);
    }
}

// A med kit, distinct from the Tools wrench. Every part derives from r so the
// cross stays centred and square when the icon shrinks.
static void ic_medkit(Canvas &c, int cx, int cy, int r) {       // Repair
    int w = 2 * r, h = 2 * r - 2;
    int x = cx - r, y = cy - r + 1;
    rbox(c, x, y, w, h);
    int hw = imax(2, r / 2);                                    // handle on the lid
    c.hline(cx - hw / 2, y - 1, hw, 1);
    int t = imax(1, r / 3);                                     // bar thickness
    int a = imax(3, r);                                         // arm span
    c.fill_rect(cx - t / 2, cy - a / 2, t, a, 1);
    c.fill_rect(cx - a / 2, cy - t / 2, a, t, 1);
}

static void ic_folder(Canvas &c, int cx, int cy, int r) {       // Files
    int w = 2 * r - 2, h = (13 * r) / 10;
    int x = cx - r + 1, y = cy - h / 2 + 1;
    int tab = imax(2, r / 2), th = imax(2, r / 3);
    c.hline(x, y - th, tab, 1);                 // the tab, up and across
    c.vline(x + tab, y - th, th, 1);
    rbox(c, x, y, w, h);
    c.vline(x, y - th, th + 1, 1);              // join the tab to the body
    c.hline(x + 2, y + 3, imax(2, w - 6), 1);   // a sheet inside
}

static void ic_store(Canvas &c, int cx, int cy, int r) {        // App Store: a bag, and something coming out of it
    if (r >= 9) {
        rbox(c, cx - 8, cy - 4, 16, 15);
        arc_dir(c, cx, cy - 4, 4, 0, -1);                        // the handle
        c.hline(cx - 3, cy - 7, 7, 1);
        c.vline(cx, cy + 0, 6, 1);                               // an arrow down
        c.line(cx - 3, cy + 3, cx, cy + 6, 1);
        c.line(cx + 3, cy + 3, cx, cy + 6, 1);
    } else {
        rbox(c, cx - 4, cy - 2, 8, 8);
        c.vline(cx - 2, cy - 4, 2, 1);
        c.vline(cx + 2, cy - 4, 2, 1);
        c.hline(cx - 2, cy - 4, 5, 1);
        c.line(cx - 2, cy + 1, cx, cy + 3, 1);
        c.line(cx + 2, cy + 1, cx, cy + 3, 1);
    }
}

static void ic_note(Canvas &c, int cx, int cy, int r) {         // Media
    // A quaver: the head low and left, the stem up its right side, and a flag
    // off the top.
    //
    // Drawn from the HEAD outwards, with a floor of two pixels on its radius.
    // At r = 5 the whole glyph is ten pixels tall, and a head of one pixel
    // merges into the stem and leaves a bare vertical line that reads as
    // nothing — which is the failure novaicons.h warns about, and it is only
    // visible in a dump.
    const int hr = imax(2, r / 3);
    const int hx = cx - r / 3;                  // the head's centre
    const int hy = cy + r / 2;
    const int sx = hx + hr;                     // the stem, up its right edge
    const int fx = sx + imax(3, r / 2);         // how far the flags reach
    c.fill_circle(hx, hy, hr, 1);
    c.vline(sx, cy - r, hy - (cy - r) + 1, 1);
    c.line(sx, cy - r, fx, cy - r / 2, 1);
    // The second flag only where the two can be told apart. Below that it is
    // one more line in a glyph that already has three.
    if (r >= 9) c.line(sx, cy - r / 2, fx, cy, 1);
}

static void ic_notes(Canvas &c, int cx, int cy, int r) {        // Alerts
    // Built by mirroring around cx, so it can never end up visually off-centre.
    int top = cy - r + 1;
    int rim = cy + r - imax(2, r / 3) - 1;
    int hw = r - 1;                             // half width at the rim
    int cr = imax(2, hw / 2);                   // radius of the domed crown
    c.vline(cx, top - 1, imax(1, r / 4), 1);    // the little knob
    uarc(c, cx, top + cr, cr);                  // dome
    c.line(cx - cr, top + cr, cx - hw, rim, 1); // shoulders flaring to the rim
    c.line(cx + cr, top + cr, cx + hw, rim, 1);
    c.hline(cx - hw, rim, 2 * hw + 1, 1);       // rim, two rows so it reads solid
    c.hline(cx - hw + 1, rim + 1, 2 * hw - 1, 1);
    c.fill_circle(cx, rim + imax(2, r / 3), imax(1, r / 4), 1); // clapper
}

static void ic_kbd(Canvas &c, int cx, int cy, int r) {          // Keyboard
    int w = 2 * r, h = imax(6, (13 * r) / 10);
    int x = cx - r, y = cy - h / 2;
    rbox(c, x, y, w, h);
    int step = imax(2, (w - 4) / 4);
    int rows = h < 12 ? 2 : 3;
    for (int row = 0; row < rows; row++) {
        int yy = y + 2 + row * imax(2, (h - 5) / rows);
        if (yy >= y + h - 3) break;
        for (int i = 0; i < 4; i++) {
            int xx = x + 2 + i * step;
            if (xx < x + w - 2) c.hline(xx, yy, imax(1, step - 1), 1);
        }
    }
    c.hline(x + 3, y + h - 3, w - 6, 1);        // space bar
}

// --- system -----------------------------------------------------------------------

static void ic_gear(Canvas &c, int cx, int cy, int r) {
    blit(c, kGear, GEAR_W, cx, cy, 2 * r);
}

static void ic_check(Canvas &c, int cx, int cy, int r) {        // System Check
    int w = 2 * r - 2;
    rbox(c, cx - r + 1, cy - r + 2, w, 2 * r - 3);
    int clip = imax(2, r / 2);                  // the clip scales with the board
    c.fill_rect(cx - clip / 2, cy - r, clip, imax(2, r / 3), 1);
    int t = imax(2, r / 3);
    c.line(cx - t, cy + 1, cx - 1, cy + t, 1);  // tick
    c.line(cx - 1, cy + t, cx + t + 1, cy - t, 1);
}

// A monitor showing a pulse. Every coordinate derives from r: fixed offsets did
// not shrink with the icon, so at neighbour size the trace ran straight out
// through the side of the box.
static void ic_stetho(Canvas &c, int cx, int cy, int r) {       // Hardware
    int x = cx - r, y = cy - r + 2;
    int w = 2 * r, h = 2 * r - 4;
    rbox(c, x, y, w, h);
    int mid = cy;
    int step = imax(1, w / 8);                  // one eighth of the width per leg
    int amp = imax(2, h / 3);
    int px = x + 2;
    c.hline(px, mid, step, 1);                          // flat lead-in
    px += step;
    c.line(px, mid, px + step, mid - amp, 1);           // up
    px += step;
    c.line(px, mid - amp, px + step, mid + amp, 1);     // down through the baseline
    px += step;
    c.line(px, mid + amp, px + step, mid, 1);           // back up
    px += step;
    c.hline(px, mid, imax(1, x + w - 2 - px), 1);       // flat run to the edge
}

static void ic_power(Canvas &c, int cx, int cy, int r) {
    c.circle(cx, cy, r - 1, 1);
    c.fill_rect(cx - 2, cy - r - 1, 5, 4, 0);   // the break in the ring
    c.vline(cx, cy - r - 1, r + 2, 1);
}

static void ic_monitor(Canvas &c, int cx, int cy, int r) {      // Display settings
    int w = 2 * r, h = (r * 14) / 10;
    int x = cx - r, y = cy - r + 1;
    rbox(c, x, y, w, h);
    c.hline(x + 2, y + h - 3, w - 4, 1);        // the bezel's lower edge
    int st = imax(2, r / 3);
    c.vline(cx, y + h, st, 1);                  // stand
    c.hline(cx - st, y + h + st, 2 * st + 1, 1);// foot
}

static void ic_house(Canvas &c, int cx, int cy, int r) {        // Home settings
    int b = cy + r - 2;                         // baseline
    int w = 2 * r - 2;
    int x = cx - r + 1;
    int bh = imax(3, r);
    c.rect(x, b - bh, w, bh, 1);                // body
    int peak = b - bh - imax(2, r / 2);
    c.line(x - 1, b - bh, cx, peak, 1);         // roof, left
    c.line(cx, peak, x + w, b - bh, 1);         // roof, right
    int d = imax(2, r / 3);                     // door
    c.fill_rect(cx - d / 2, b - d, imax(1, d), d, 1);
}

// Deliberately NOT the WiFi bars: those belong to the WiFi APP, and two
// identical icons a folder apart is what started the icon work.
static void ic_globe(Canvas &c, int cx, int cy, int r) {        // Network settings
    int rr = r - 1;
    c.circle(cx, cy, rr, 1);
    c.hline(cx - rr, cy, 2 * rr + 1, 1);        // equator
    c.vline(cx, cy - rr, 2 * rr + 1, 1);        // meridian
    int q = imax(1, rr / 2);                    // the curved meridians, as chords
    c.line(cx - q, cy - rr + 1, cx - q, cy + rr - 1, 1);
    c.line(cx + q, cy - rr + 1, cx + q, cy + rr - 1, 1);
}

static void ic_padlock(Canvas &c, int cx, int cy, int r) {      // Security: a padlock with its shackle
    if (r >= 9) {
        rbox(c, cx - 7, cy - 1, 14, 12);                         // the body
        c.vline(cx - 5, cy - 6, 5, 1);                           // the shackle, narrower than the body
        c.vline(cx + 4, cy - 6, 5, 1);
        c.hline(cx - 4, cy - 7, 8, 1);
        c.pixel(cx - 5, cy - 7, 1); c.pixel(cx + 4, cy - 7, 1);
        c.fill_circle(cx, cy + 4, 2, 1);                         // keyhole
        c.vline(cx, cy + 5, 3, 1);
    } else {
        rbox(c, cx - 4, cy - 1, 8, 7);
        c.vline(cx - 2, cy - 4, 3, 1);
        c.vline(cx + 1, cy - 4, 3, 1);
        c.hline(cx - 2, cy - 4, 4, 1);
        c.pixel(cx - 1, cy + 2, 1); c.pixel(cx, cy + 2, 1); c.pixel(cx - 1, cy + 3, 1); c.pixel(cx, cy + 3, 1);
    }
}

static void ic_tag(Canvas &c, int cx, int cy, int r) {          // Device / names
    int w = 2 * r - 2, h = (r * 12) / 10;
    int x = cx - r + 3, y = cy - h / 2;
    rbox(c, x, y, w - 2, h);
    c.circle(cx - r + 1, cy, imax(1, r / 4), 1);               // eyelet, off the left
    c.line(cx - r + 1 + imax(1, r / 4), cy, x, cy, 1);
    for (int i = 1; i <= 2; i++)                               // two written lines
        c.hline(x + 3, y + i * (h / 3), imax(2, w - 8), 1);
}

// --- small hardware -----------------------------------------------------------------

static void ic_key(Canvas &c, int cx, int cy, int r) {          // iButton
    c.circle(cx - r / 2, cy, r / 2, 1);
    c.hline(cx - r / 2, cy, 2 * r - r / 2, 1);
    c.vline(cx + r - 2, cy, r / 2, 1);
    c.vline(cx + r / 2, cy, r / 3, 1);
}

static void ic_sd(Canvas &c, int cx, int cy, int r) {           // SD card
    c.line(cx - r + 2, cy - r, cx + r, cy - r, 1);
    c.line(cx + r, cy - r, cx + r, cy + r, 1);
    c.line(cx + r, cy + r, cx - r, cy + r, 1);
    c.line(cx - r, cy + r, cx - r, cy - r + 3, 1);
    c.line(cx - r, cy - r + 3, cx - r + 2, cy - r, 1);
    for (int i = 0; i < 3; i++) c.vline(cx - r / 3 + i * 3, cy - r + 1, 3, 1);
}

static void ic_speaker(Canvas &c, int cx, int cy, int r) {      // buzzer
    c.fill_rect(cx - r, cy - r / 3, r / 2, 2 * (r / 3), 1);
    c.line(cx - r / 2, cy - r / 3, cx, cy - r, 1);
    c.line(cx - r / 2, cy + r / 3, cx, cy + r, 1);
    c.vline(cx, cy - r, 2 * r, 1);
    c.line(cx + r / 2, cy - r / 2, cx + r, cy - r, 1);
    c.line(cx + r / 2, cy + r / 2, cx + r, cy + r, 1);
}

static void ic_vibe(Canvas &c, int cx, int cy, int r) {         // vibration
    rbox(c, cx - r / 2, cy - r, r, 2 * r);
    for (int i = 0; i < 2; i++) {
        c.vline(cx - r + i * 2, cy - r / 2, r, 1);
        c.vline(cx + r / 2 + 1 + i * 2, cy - r / 2, r, 1);
    }
}

static void ic_led(Canvas &c, int cx, int cy, int r) {          // status LED: a bulb, not a sun
    if (r >= 9) {
        c.circle(cx, cy - 2, 6, 1);                              // the glass
        c.line(cx - 2, cy - 3, cx, cy, 1);                       // the filament
        c.line(cx, cy, cx + 2, cy - 3, 1);
        c.hline(cx - 3, cy + 5, 7, 1);                           // the cap
        c.hline(cx - 2, cy + 7, 5, 1);
        c.vline(cx, cy - 12, 2, 1);                              // light
        c.line(cx - 8, cy - 9, cx - 10, cy - 11, 1);
        c.line(cx + 8, cy - 9, cx + 10, cy - 11, 1);
    } else {
        c.circle(cx, cy - 1, 3, 1);
        c.hline(cx - 1, cy + 3, 3, 1);
        c.hline(cx - 1, cy + 5, 3, 1);
        c.pixel(cx, cy - 5, 1);
    }
}


// --- the small apps ----------------------------------------------------------------------

static void ic_dice(Canvas &c, int cx, int cy, int r) {          // a die: a box and its five pips
    rbox(c, cx - r, cy - r, 2 * r, 2 * r);
    const int o = imax(2, r / 2), sz = r >= 9 ? 2 : 1;
    const int pips[5][2] = { { -o, -o }, { o, -o }, { 0, 0 }, { -o, o }, { o, o } };
    for (int i = 0; i < 5; i++) c.fill_rect(cx + pips[i][0] - sz / 2, cy + pips[i][1] - sz / 2, sz, sz, 1);
}

static void ic_tally(Canvas &c, int cx, int cy, int r) {         // four strokes and the one across them
    const int span = 2 * r - 2, h = r - 1;
    for (int i = 0; i < 4; i++) c.vline(cx - r + 1 + i * span / 3, cy - h, 2 * h, 1);
    c.line(cx - r, cy + h / 2, cx + r, cy - h / 2, 1);
}

static void ic_hourglass(Canvas &c, int cx, int cy, int r) {
    c.hline(cx - r + 1, cy - r, 2 * r - 2, 1);
    c.hline(cx - r + 1, cy + r - 1, 2 * r - 2, 1);
    c.line(cx - r + 2, cy - r, cx, cy, 1);
    c.line(cx + r - 2, cy - r, cx, cy, 1);
    c.line(cx, cy, cx - r + 2, cy + r - 1, 1);
    c.line(cx, cy, cx + r - 2, cy + r - 1, 1);
}


static void ic_torch(Canvas &c, int cx, int cy, int r) {         // a flashlight pointing up, with its beam
    if (r >= 9) {
        c.fill_rect(cx - 6, cy - 4, 12, 2, 1);                   // the lens
        c.line(cx - 6, cy - 2, cx - 3, cy + 1, 1);               // narrowing to the handle
        c.line(cx + 5, cy - 2, cx + 2, cy + 1, 1);
        rbox(c, cx - 3, cy + 1, 6, 11);
        c.fill_rect(cx - 1, cy + 4, 2, 3, 1);                    // the switch
        c.vline(cx, cy - 11, 4, 1);                              // the beam
        c.line(cx - 6, cy - 7, cx - 9, cy - 10, 1);
        c.line(cx + 5, cy - 7, cx + 8, cy - 10, 1);
    } else {
        c.hline(cx - 3, cy - 2, 6, 1);
        rbox(c, cx - 2, cy - 1, 4, 7);
        c.vline(cx, cy - 6, 2, 1);
        c.pixel(cx - 4, cy - 4, 1);
        c.pixel(cx + 3, cy - 4, 1);
    }
}

static void ic_pie(Canvas &c, int cx, int cy, int r) {           // one-time codes: a clock face being used up
    c.circle(cx, cy, r - 1, 1);
    const int rr = r - 2;
    for (int dy = 0; dy <= rr; dy++) {
        int w = 0;
        while ((w + 1) * (w + 1) + dy * dy <= rr * rr) w++;
        c.hline(cx, cy - dy, w, 1);
    }
}

static void ic_usb(Canvas &c, int cx, int cy, int r) {            // BadUSB: a USB stick, which is what it pretends to be
    if (r >= 9) {
        c.rect(cx - 11, cy - 3, 8, 7, 1);                        // the plug
        c.fill_rect(cx - 9, cy - 1, 2, 1, 1);                    // and its two contacts
        c.fill_rect(cx - 9, cy + 1, 2, 1, 1);
        rbox(c, cx - 3, cy - 6, 14, 12);                         // the body
        c.circle(cx + 6, cy, 2, 1);                              // the loop for a lanyard
        c.hline(cx - 1, cy - 3, 4, 1);                           // and a label strip
    } else {
        c.rect(cx - 6, cy - 2, 4, 5, 1);
        c.pixel(cx - 5, cy, 1);
        rbox(c, cx - 2, cy - 3, 8, 7);
        c.pixel(cx + 3, cy, 1);
    }
}

static void ic_tasks(Canvas &c, int cx, int cy, int r) {          // what is running: rows with a mark each
    const int step = imax(3, (2 * r - 2) / 3);
    for (int i = 0; i < 3; i++) {
        const int y = cy - r + 1 + i * step;
        c.fill_rect(cx - r, y, 2, 2, 1);
        c.hline(cx - r + 4, y, 2 * r - 4, 1);
    }
}

static void ic_update(Canvas &c, int cx, int cy, int r) {         // a download arrow onto a tray
    c.vline(cx, cy - r, r + 1, 1);
    c.line(cx - r / 2, cy - 1, cx, cy + r / 2 - 1, 1);
    c.line(cx + r / 2, cy - 1, cx, cy + r / 2 - 1, 1);
    c.hline(cx - r, cy + r, 2 * r, 1);
    c.vline(cx - r, cy + r - 2, 3, 1);
    c.vline(cx + r - 1, cy + r - 2, 3, 1);
}

// Wireless: a mast radiating both ways. The folder used to borrow the Bluetooth rune, which is
// one app inside it.
static void ic_broadcast(Canvas &c, int cx, int cy, int r) {
    c.fill_circle(cx, cy, r >= 9 ? 2 : 1, 1);
    const int a = r >= 9 ? 5 : 3, b = r >= 9 ? 9 : 5;
    arc_dir(c, cx, cy, a, -1, 0); arc_dir(c, cx, cy, a, 1, 0);
    arc_dir(c, cx, cy, b, -1, 0); arc_dir(c, cx, cy, b, 1, 0);
}

// Sensors: a dial with its needle. The folder used to borrow the map pin, which is GPS.
static void ic_gauge(Canvas &c, int cx, int cy, int r) {
    const int rr = r >= 9 ? 10 : 5, base = cy + (r >= 9 ? 6 : 3);
    uarc(c, cx, base, rr);
    if (r >= 9) uarc(c, cx, base, rr - 1);
    c.hline(cx - rr, base, 2 * rr + 1, 1);
    c.line(cx, base, cx + rr / 2 + 1, base - rr / 2 - 1, 1);
    c.fill_circle(cx, base, r >= 9 ? 2 : 1, 1);
    if (r >= 9) { c.pixel(cx, base - rr + 2, 1); c.pixel(cx - 7, base - 7, 1); c.pixel(cx + 7, base - 7, 1); }
}

// System: a chip, which is what is under all of it. The gear is Settings.
static void ic_chip(Canvas &c, int cx, int cy, int r) {
    const int h = r >= 9 ? 6 : 3;
    rbox(c, cx - h, cy - h, 2 * h, 2 * h);
    c.rect(cx - h / 2, cy - h / 2, h + 1, h + 1, 1);
    const int n = r >= 9 ? 3 : 2;
    for (int i = 0; i < n; i++) {
        const int o = (2 * h * (i + 1)) / (n + 1) - h + (r >= 9 ? 0 : 0);
        c.vline(cx + o, cy - h - 3, 3, 1); c.vline(cx + o, cy + h, 3, 1);
        c.hline(cx - h - 3, cy + o, 3, 1); c.hline(cx + h, cy + o, 3, 1);
    }
}

// Base Conv: the two digits it is about.
static void ic_digits(Canvas &c, int cx, int cy, int r) {
    const int sc = r >= 9 ? 2 : 1;
    const int w = c.text_width("01", sc, false);
    c.text(cx - w / 2, cy - (FONT_H * sc) / 2, "01", 1, sc);
}

// --- the map ------------------------------------------------------------------------

typedef void (*IconFn)(Canvas &, int, int, int);
struct Entry { const char *key; IconFn fn; };

// Every key the app catalogue and the module table use. The five category
// folders borrow an app's icon each, so a folder reads as the kind of thing
// inside it rather than as one of five identical boxes.
static const Entry kMap[] = {
    { "dht11",       ic_thermo },
    { "gps",         ic_pin },
    { "pn532",       ic_nfc },
    { "cc1101",      ic_radio },
    { "sx1276",      ic_antenna },
    { "bt",          ic_bt },
    { "ibutton",     ic_key },
    { "sdcard",      ic_sd },
    { "battery",     ic_battery },
    { "buzzer",      ic_speaker },
    { "vibration",   ic_vibe },
    { "led",         ic_led },
    { "wifi",        ic_wifi },
    // Deliberately NOT the globe or the bars: the globe belongs to the network
    // SETTINGS row and the bars to WiFi itself, and this sits beside both.
    { "lan",         ic_lan },
    // 'ir' is the app; ir_rx and ir_tx are the two halves of the hardware.
    { "ir",          ic_ir },
    { "ir_rx",       ic_ir_rx },
    { "ir_tx",       ic_ir_tx },
    { "scripts",     ic_scroll },
    { "dice",        ic_dice },
    { "counter",     ic_tally },
    { "timer",       ic_hourglass },
    { "baseconv",    ic_digits },
    { "torch",       ic_torch },
    { "totp",        ic_pie },
    { "badusb",      ic_usb },
    { "tasks",       ic_tasks },
    { "update",      ic_update },
    { "settings",    ic_gear },
    { "logs",        ic_doc },
    { "check",       ic_check },
    { "msg",         ic_chat },
    { "power",       ic_power },
    { "notes",       ic_notes },
    { "clock",       ic_clock },
    { "rtc",         ic_clock },
    { "fix",         ic_medkit },
    { "cmds",        ic_cmdlist },
    { "kbd",         ic_kbd },
    { "store",       ic_store },
    { "media",       ic_note },
    { "diag",        ic_stetho },
    { "wardrive",    ic_wardrive },
    { "radar",       ic_radar },
    { "presence",    ic_person },
    // Shell gets the terminal window because that is literally what it is;
    // Commands has the list icon, so the two stopped being identical.
    { "res",         ic_bars },
    { "shell",       ic_terminal },
    { "files",       ic_folder },
    // The settings groups are apps in their own right — the System folder IS the
    // settings app — so each needs an icon that is not the gear and not a
    // duplicate of the app beside it.
    { "set_display",  ic_monitor },
    { "set_home",     ic_house },
    { "set_network",  ic_globe },
    { "set_security", ic_padlock },
    { "set_system",   ic_gear },
    { "set_device",   ic_tag },
    // The five folders, borrowing exactly what the MicroPython home borrowed:
    // Bluetooth, the map pin, the wrench, the gear and the keyboard. Wireless
    // deliberately does NOT take the WiFi bars — the WiFi app has those, and a
    // folder that looks like one of the apps inside it is the confusion the
    // whole icon pass existed to remove.
    { "Wireless",    ic_broadcast },
    { "Sensors",     ic_gauge },
    { "Tools",       ic_wrench },
    { "System",      ic_chip },
    { "Testing",     ic_kbd },
    { "tools",       ic_wrench },
};

void draw(Canvas &c, const char *key, int cx, int cy, int r, const char *label_fallback) {
    if (key) {
        for (unsigned i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++)
            if (strcmp(key, kMap[i].key) == 0) { kMap[i].fn(c, cx, cy, r); return; }
    }

    // A box with the initial in it — a full-size square, hard-cornered, exactly
    // as the MicroPython version drew it. Deliberately not a question mark: a
    // new app with no icon yet should look like an app, and a blurry custom
    // glyph is worse than a clear shared one.
    c.rect(cx - r, cy - r, 2 * r, 2 * r, 1);
    const char *s = (label_fallback && *label_fallback) ? label_fallback : (key ? key : "?");
    char one[2] = { s[0], 0 };
    if (one[0] >= 'a' && one[0] <= 'z') one[0] = (char)(one[0] - 32);
    int scale = r >= 9 ? 2 : 1;
    c.ch(cx - (FONT_W * scale) / 2, cy - (FONT_H * scale) / 2, one[0], 1, scale);
}

}  // namespace icons
}  // namespace nova
