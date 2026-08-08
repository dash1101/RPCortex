// Render the Nova D1 screen on the host, to a page you can look at.
//
// This is the replacement for the Pyodide simulator, and it is better in the way
// that matters: it runs the SAME code the device runs. novacanvas and novaicons
// have no fw_* call in them by design, so what comes out here is what comes out
// of the panel, pixel for pixel — not an approximation drawn by a second
// implementation that can drift.
//
// It exists because "the icons look different from the Python version" is a
// judgement nobody can settle from a photograph of a 1.3 inch panel. Now it can
// be settled by putting the two side by side.
//
//     os/host/novasim > /tmp/novasim.html
//
// Writes a self-contained page: every icon at the size the home screen draws it,
// and a set of composed screens. No network, no dependencies.
#include "../apps/novad1/novacanvas.cpp"
#include "../apps/novad1/novaicons.cpp"
// The layout tokens and the shared widgets, so the frames below are POSITIONED
// by the same numbers the device uses instead of by a second set that agreed
// with them on the day it was written. novaui has no fw_* call in it either,
// which is what makes that possible.
#include "../apps/novad1/novaui.cpp"

#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

using nova::Canvas;

static uint8_t g_buf[128 * 8];
static Canvas C;

static void fresh(int w = 128, int h = 64) {
    memset(g_buf, 0, sizeof(g_buf));
    C.attach(g_buf, w, h);
}

// --- the page ------------------------------------------------------------------

struct Frame {
    std::string name;
    std::string bits;      // one character per pixel, '1' or '0', row-major
    int w, h;
};
static std::vector<Frame> g_frames;

static void capture(const char *name) {
    Frame f;
    f.name = name;
    f.w = C.width();
    f.h = C.height();
    f.bits.reserve((size_t)f.w * f.h);
    for (int y = 0; y < f.h; y++)
        for (int x = 0; x < f.w; x++)
            f.bits.push_back(C.get(x, y) ? '1' : '0');
    g_frames.push_back(f);
}

// --- what to render --------------------------------------------------------------

// Every key the icon map knows, plus a few that fall through to the initial
// fallback so that path is visible too rather than only being described.
static const char *kIconKeys[] = {
    "wifi", "bt", "cc1101", "sx1276", "msg", "pn532", "ir_rx", "gps",
    "dht11", "battery", "clock", "files", "shell", "radar", "presence",
    "wardrive", "notes", "power", "diag", "check", "store", "fix", "cmds",
    "res", "logs", "scripts", "settings", "set_display", "set_home",
    "set_network", "set_security", "set_system", "set_device",
    "Wireless", "Sensors", "Tools", "System", "Testing",
    "kbd", "sdcard", "ibutton",
};

static void render_icons(void) {
    // The pair the icons were DRAWN against, side by side, on one strip per icon
    // — because an icon that reads at twelve pixels and turns to mush at six is
    // a real problem and is invisible until they are next to each other.
    //
    // Fixed at 12 and 6 rather than following ui::ICON_SMALL, because this strip
    // is also what tools/icondiff.py compares against the MicroPython original
    // and both sides have to be asked for the same size. The gallery's own
    // choice of neighbour size shows in the home frames below.
    for (unsigned i = 0; i < sizeof(kIconKeys) / sizeof(kIconKeys[0]); i++) {
        fresh(44, 32);
        nova::icons::draw(C, kIconKeys[i], 15, 16, 12, kIconKeys[i]);
        nova::icons::draw(C, kIconKeys[i], 36, 16, 6, kIconKeys[i]);
        capture(kIconKeys[i]);
    }
}

// The gallery, laid out from nova::ui rather than from a private copy of the
// numbers. This is Gallery::draw with the animation state passed in — the ring,
// the label under it and the position pips under that.
static void ring(const char *const *keys, int count, int sel, int slide, int dir) {
    using namespace nova::ui;
    const int cx = C.width() / 2;
    const int off = dir * slide * ICON_SPACING / 256;

    for (int pass = 1; pass >= 0; pass--) {
        for (int d = -1; d <= 1; d++) {
            if ((d < 0 ? -d : d) != pass) continue;
            int idx = ((sel + d) % count + count) % count;
            int x = cx + d * ICON_SPACING + off;
            if (x < -ICON_BIG || x > C.width() + ICON_BIG) continue;
            int dist = x > cx ? x - cx : cx - x;
            int r = ICON_BIG - dist * (ICON_BIG - ICON_SMALL) / ICON_SPACING;
            if (r < ICON_SMALL) r = ICON_SMALL;
            if (r > ICON_BIG)   r = ICON_BIG;
            nova::icons::draw(C, keys[idx], x, RING_Y, r, keys[idx]);
        }
    }

    int at = ((sel + ((off > ICON_SPACING / 2) ? -1 :
                      (off < -ICON_SPACING / 2) ? 1 : 0)) % count + count) % count;
    C.text_centred(label_y(C), keys[at], 1, 1, false);

    if (count > 1 && count <= 16) {
        const int y = pip_y(C);
        const int base = y + PIP_H - 1;
        int x0 = cx - (count * PIP_PITCH - 1) / 2;
        for (int i = 0; i < count; i++) {
            int x = x0 + i * PIP_PITCH;
            if (i == sel) C.fill_rect(x, y, PIP_H, PIP_H, 1);
            else          C.pixel(x, base, 1);
        }
    }
}

// The status bar, so a home frame is the whole thing the panel shows and not
// just the part that changed.
static void status_bar(const char *title, bool usb, int pct, int bars);

// The home ring, at three positions through a step, so the slide can be checked
// as a sequence rather than as one still — and then the flat gallery style,
// which shows app icons rather than the five folders and is where the neighbour
// size has to earn its keep.
static void render_home(void) {
    static const char *folders[] = { "Wireless", "Sensors", "Tools", "System", "Testing" };
    for (int phase = 0; phase < 3; phase++) {
        fresh();
        ring(folders, 5, 2, 256 - phase * 128, 1);     // slide 256, 128, 0
        status_bar("Nova D1", false, -1, 0);
        char nm[24];
        snprintf(nm, sizeof(nm), "home ring %d of 3", phase + 1);
        capture(nm);
    }

    static const char *apps[] = { "wifi", "bt", "radar", "gps", "clock",
                                  "files", "shell", "res", "diag", "store" };
    for (int sel = 0; sel < 3; sel++) {
        fresh();
        ring(apps, 10, sel * 4 + 1, 0, 1);
        status_bar("Nova D1", false, -1, 0);
        char nm[32];
        snprintf(nm, sizeof(nm), "home gallery %d of 3", sel + 1);
        capture(nm);
    }
}

// draw_status, minus the parts that need a live screen: the clock on the right,
// the power badge, the link bars, and the title in what is left.
static void status_bar(const char *title, bool usb, int pct, int bars) {
    int right = C.width() - C.text_width("14:32", 1, false);
    C.text(right, 1, "14:32", 1);

    right -= nova::ui::POWER_W;
    nova::ui::power_badge(C, right, usb, pct);

    right -= 11;
    for (int b = 0; b < 3; b++) {
        int h = 2 + b * 2;
        int x = right + b * 3;
        if (b < bars) C.fill_rect(x, 1 + (6 - h), 2, h, 1);
        else          C.hline(x, 7, 2, 1);
    }
    if (!bars) { C.pixel(right + 2, 2, 1); C.pixel(right + 4, 2, 1); }

    C.text_fit(0, 1, title, 1, right - 2, false);
    C.hline(0, nova::ui::BARH - 1, C.width(), 1);
}

// The status bar on its own, in each of the states it can be in — this is where
// the signal bars were wrong for a week without anybody being able to see it.
//
// The power states are here in full because there are three of them now and the
// difference between two is the ABSENCE of a level: an outline that means "no
// reading" and a filled cell that means 4% are a keystroke apart in the source
// and must not be a keystroke apart on the glass. The percentages are the width
// edges of the number inside — one digit, two, and the three that fill the cell.
static void render_status(void) {
    struct S { const char *name; int bars; int batt; bool usb; };
    static const S kStates[] = {
        { "status: no link, source not known", 0, -1, false },
        { "status: weak link, on USB",         1, -1, true  },
        { "status: on the cell, level unknown",3, -1, false },
        { "status: battery 4%",                3,  4, false },
        { "status: battery 42%",               3, 42, false },
        { "status: battery 80%",               3, 80, false },
        { "status: battery 100%",              3,100, false },
    };
    for (unsigned i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) {
        const S &s = kStates[i];
        fresh(128, 16);
        status_bar("Nova D1", s.usb, s.batt, s.bars);
        capture(s.name);
    }
}

static void render_boot(void) {
    // The three beats of the opening, at the moments they are most different.
    const int cx = 64, cy = 32;
    fresh();
    C.circle(cx, cy, 18, 1);
    C.circle(cx, cy, 11, 1);
    capture("boot: ring");

    fresh();
    int full = C.text_width("Nova D1", 2, false);
    int x0 = cx - full / 2;
    C.text(x0, cy - 13, "Nova D1", 1, 2, false);
    C.fill_rect(x0 + full * 2 / 3, cy - 15, full / 3 + 2, 19, 0);
    capture("boot: name wiping in");

    fresh();
    C.text(x0, cy - 13, "Nova D1", 1, 2, false);
    C.text_centred(cy + 7, "RPCortex", 1);
    C.hline(cx - 22, cy + 7 + nova::FONT_H + 2, 44, 1);
    capture("boot: signed");

    // And the check list, part way through.
    fresh();
    static const char *rows[]  = { "Display", "Storage", "Memory", "Controls", "Clock" };
    static const char *vals[]  = { "ssd1309 0x3c", "/nova", "241 KB, max 198",
                                   "EC11 on 14/15", "not set" };
    static const char *marks[] = { "+", "+", "+", "+", "-" };
    int y = 2;
    for (int i = 0; i < 5; i++) {
        C.text(0, y, marks[i], 1);
        C.text(8, y, rows[i], 1);
        int w = C.text_width(vals[i], 1, true);
        C.text(128 - w, y, vals[i], 1, 1, true);
        y += 9;
    }
    C.rounded_rect(0, 56, 128, 7, 1, false);
    C.fill_rect(2, 58, 5 * 124 / 9, 3, 1);
    capture("boot: checks running");
}

static void render_menu(void) {
    fresh();
    C.text(0, 1, "Power", 1);
    C.text(128 - 29, 1, "14:32", 1);
    C.hline(0, 8, 128, 1);
    static const char *items[] = { "Lock", "Incognito", "Controls", "Reload", "Reboot", "Shutdown" };
    for (int i = 0; i < 6; i++) {
        int y = 10 + i * 9;
        if (i == 1) {
            C.rounded_rect(0, y - 1, 124, 9, 1, true);
            C.text(4, y, items[i], 0);
            C.text(124 - 6 - 2, y, ">", 0);
        } else {
            C.text(4, y, items[i], 1);
        }
    }
    C.scrollbar(128 - 3 + 1, 10, 54, 0, 6, 6);
    capture("menu: power");
}

// --- output ----------------------------------------------------------------------

// `novasim --dump` prints one line per frame: name, then a character per pixel.
// That is what makes the comparison against the MicroPython original mechanical
// rather than a matter of squinting at two screenshots — tools/icondiff.py
// renders the same keys through v1's own novacanvas and diffs the two.
static int dump(void) {
    render_icons();
    for (const Frame &f : g_frames)
        printf("%s %d %d %s\n", f.name.c_str(), f.w, f.h, f.bits.c_str());
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) return dump();
    render_boot();
    render_home();
    render_status();
    render_menu();
    render_icons();

    printf("<!doctype html><meta charset=utf-8><title>Nova D1 &mdash; rendered</title>\n");
    printf("<style>"
           "body{background:#111;color:#ccc;font:13px/1.5 system-ui,sans-serif;margin:24px}"
           "h1{font-size:16px;font-weight:600;color:#eee}"
           "p{max-width:60ch;color:#999}"
           "ul{display:flex;flex-wrap:wrap;gap:18px;list-style:none;padding:0}"
           "li{text-align:center}"
           "canvas{background:#0a1a12;image-rendering:pixelated;border:1px solid #333;"
           "border-radius:3px;display:block}"
           "span{display:block;margin-top:4px;font-size:11px;color:#888}"
           "</style>\n");
    printf("<h1>Nova D1 &mdash; rendered from the device's own drawing code</h1>\n");
    printf("<p>Every pixel here came from novacanvas and novaicons, the same "
           "source the panel runs. Nothing is redrawn or approximated, so an "
           "icon that looks wrong here looks wrong on the device.</p>\n");
    printf("<ul>\n");
    for (const Frame &f : g_frames) {
        printf("<li><canvas width=%d height=%d data-b=\"%s\"></canvas><span>%s</span></li>\n",
               f.w, f.h, f.bits.c_str(), f.name.c_str());
    }
    printf("</ul>\n");
    // Scale is applied in CSS rather than by drawing large, so the canvas holds
    // exactly the device's pixels and a browser zoom shows the real thing.
    printf("<script>\n"
           "for (const cv of document.querySelectorAll('canvas')) {\n"
           "  const b = cv.dataset.b, w = cv.width, h = cv.height;\n"
           "  const ctx = cv.getContext('2d');\n"
           "  const img = ctx.createImageData(w, h);\n"
           "  for (let i = 0; i < w * h; i++) {\n"
           "    const on = b[i] === '1';\n"
           "    img.data[i*4+0] = on ? 0x6f : 0x0a;\n"
           "    img.data[i*4+1] = on ? 0xf7 : 0x1a;\n"
           "    img.data[i*4+2] = on ? 0xc3 : 0x12;\n"
           "    img.data[i*4+3] = 255;\n"
           "  }\n"
           "  ctx.putImageData(img, 0, 0);\n"
           "  cv.style.width = (w * 3) + 'px';\n"
           "  cv.style.height = (h * 3) + 'px';\n"
           "}\n"
           "</script>\n");
    fprintf(stderr, "novasim: %d frame(s)\n", (int)g_frames.size());
    return 0;
}
