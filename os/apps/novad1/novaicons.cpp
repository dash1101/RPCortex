// Desc: The app icons — drawn, not stored.
// File: novaicons.cpp
#include "novaicons.h"
#include "novacore.h"

#include <string.h>

namespace nova {
namespace icons {

// Radiating arcs, for anything that transmits or listens. The shared vocabulary
// of the Wireless folder: the same three arcs appear on WiFi, BLE, sub-GHz and
// LoRa, with a different emitter under them, so the folder reads as a family.
static void arcs(Canvas &c, int cx, int cy, int r, int n) {
    for (int i = 1; i <= n; i++) {
        int rr = r * i / (n + 1);
        // Upper arc only — a full circle reads as a target, not a signal.
        for (int a = -rr; a <= rr; a++) {
            int y = 0;
            // A circle's upper half, stepped by x. Cheaper than trigonometry and
            // there is no floating point in this package by design.
            int sq = rr * rr - a * a;
            while ((y + 1) * (y + 1) <= sq) y++;
            c.pixel(cx + a, cy - y, 1);
        }
    }
}

static void antenna(Canvas &c, int cx, int cy, int r) {
    c.vline(cx, cy - r / 3, r, 1);
    c.hline(cx - r / 3, cy + r * 2 / 3, r * 2 / 3 + 1, 1);
}

// --- the icons ----------------------------------------------------------------

static void ic_wifi(Canvas &c, int cx, int cy, int r) {
    arcs(c, cx, cy + r / 2, r, 3);
    c.fill_rect(cx - 1, cy + r / 2 - 1, 3, 3, 1);
}

static void ic_ble(Canvas &c, int cx, int cy, int r) {
    // The Bluetooth rune: a vertical stroke with two triangles off it.
    c.vline(cx, cy - r, 2 * r, 1);
    c.line(cx, cy - r, cx + r / 2, cy - r / 2, 1);
    c.line(cx + r / 2, cy - r / 2, cx, cy, 1);
    c.line(cx, cy, cx + r / 2, cy + r / 2, 1);
    c.line(cx + r / 2, cy + r / 2, cx, cy + r, 1);
    c.line(cx - r / 2, cy - r / 2, cx, cy, 1);
    c.line(cx - r / 2, cy + r / 2, cx, cy, 1);
}

static void ic_subghz(Canvas &c, int cx, int cy, int r) {
    antenna(c, cx, cy, r);
    arcs(c, cx, cy - r / 3, r, 2);
}

static void ic_lora(Canvas &c, int cx, int cy, int r) {
    antenna(c, cx, cy, r);
    arcs(c, cx, cy - r / 3, r, 3);
}

static void ic_nfc(Canvas &c, int cx, int cy, int r) {
    // A card with a wave over it.
    c.rounded_rect(cx - r, cy - r * 2 / 3, 2 * r, r * 4 / 3, 1, false);
    arcs(c, cx - r / 3, cy + r / 3, r * 2 / 3, 2);
}

static void ic_ir(Canvas &c, int cx, int cy, int r) {
    // A remote: a body with a button and a beam leaving the top.
    c.rounded_rect(cx - r / 2, cy - r / 2, r, r * 3 / 2, 1, false);
    c.fill_rect(cx - 1, cy, 3, 3, 1);
    for (int i = 1; i <= 3; i++) c.pixel(cx, cy - r / 2 - i * 2, 1);
}

static void ic_gps(Canvas &c, int cx, int cy, int r) {
    // A map pin: a circle over a point.
    c.circle(cx, cy - r / 3, r * 2 / 3, 1);
    c.line(cx - r / 2, cy, cx, cy + r, 1);
    c.line(cx + r / 2, cy, cx, cy + r, 1);
}

static void ic_climate(Canvas &c, int cx, int cy, int r) {
    // A thermometer: a stem with a bulb.
    c.vline(cx, cy - r, r * 3 / 2, 1);
    c.vline(cx + 2, cy - r, r * 3 / 2, 1);
    c.fill_circle(cx + 1, cy + r * 2 / 3, r / 3 + 1, 1);
}

static void ic_battery(Canvas &c, int cx, int cy, int r) {
    c.rect(cx - r, cy - r / 2, 2 * r - 2, r, 1);
    c.fill_rect(cx + r - 2, cy - r / 4, 2, r / 2, 1);
    c.fill_rect(cx - r + 2, cy - r / 2 + 2, r, r - 4, 1);
}

static void ic_clock(Canvas &c, int cx, int cy, int r) {
    c.circle(cx, cy, r, 1);
    c.vline(cx, cy - r / 2, r / 2, 1);
    c.hline(cx, cy, r / 2, 1);
}

static void ic_files(Canvas &c, int cx, int cy, int r) {
    // A folder: a tab and a body.
    c.hline(cx - r, cy - r / 2, r, 1);
    c.rect(cx - r, cy - r / 3, 2 * r, r * 4 / 3, 1);
}

static void ic_shell(Canvas &c, int cx, int cy, int r) {
    c.rect(cx - r, cy - r * 2 / 3, 2 * r, r * 4 / 3, 1);
    // A prompt: the chevron and an underscore.
    c.line(cx - r / 2, cy - r / 4, cx - r / 4, cy, 1);
    c.line(cx - r / 4, cy, cx - r / 2, cy + r / 4, 1);
    c.hline(cx, cy + r / 4, r / 2, 1);
}

static void ic_radar(Canvas &c, int cx, int cy, int r) {
    c.circle(cx, cy, r, 1);
    c.circle(cx, cy, r / 2, 1);
    c.line(cx, cy, cx + r, cy - r, 1);
}

static void ic_msg(Canvas &c, int cx, int cy, int r) {
    // A speech bubble.
    c.rounded_rect(cx - r, cy - r * 2 / 3, 2 * r, r, 1, false);
    c.line(cx - r / 3, cy + r / 3, cx - r / 6, cy + r * 2 / 3, 1);
    c.line(cx - r / 6, cy + r * 2 / 3, cx, cy + r / 3, 1);
}

static void ic_bell(Canvas &c, int cx, int cy, int r) {
    c.circle(cx, cy, r * 2 / 3, 1);
    c.hline(cx - r, cy + r * 2 / 3, 2 * r, 1);
    c.fill_rect(cx - 1, cy + r * 2 / 3 + 1, 3, 2, 1);
}

static void ic_gear(Canvas &c, int cx, int cy, int r) {
    c.circle(cx, cy, r * 2 / 3, 1);
    c.circle(cx, cy, r / 4, 1);
    // Four teeth. Eight would be prettier and at radius 6 they merge into a
    // blur, so four it is at both sizes.
    c.fill_rect(cx - 1, cy - r, 3, r / 3 + 1, 1);
    c.fill_rect(cx - 1, cy + r - r / 3, 3, r / 3 + 1, 1);
    c.fill_rect(cx - r, cy - 1, r / 3 + 1, 3, 1);
    c.fill_rect(cx + r - r / 3, cy - 1, r / 3 + 1, 3, 1);
}

static void ic_power(Canvas &c, int cx, int cy, int r) {
    c.circle(cx, cy, r * 2 / 3, 1);
    c.fill_rect(cx - 1, cy - r, 3, r, 0);      // the gap in the ring
    c.vline(cx, cy - r, r, 1);
}

static void ic_chip(Canvas &c, int cx, int cy, int r) {
    // A package with legs: the Hardware app, and the fallback for a module.
    c.rect(cx - r * 2 / 3, cy - r * 2 / 3, r * 4 / 3, r * 4 / 3, 1);
    for (int i = -1; i <= 1; i++) {
        c.hline(cx - r, cy + i * (r / 2), r / 3, 1);
        c.hline(cx + r * 2 / 3, cy + i * (r / 2), r / 3, 1);
    }
}

static void ic_store(Canvas &c, int cx, int cy, int r) {
    // A box with an arrow into it.
    c.rect(cx - r, cy, 2 * r, r, 1);
    c.vline(cx, cy - r, r * 2 / 3, 1);
    c.line(cx - r / 3, cy - r / 2, cx, cy - r / 6, 1);
    c.line(cx + r / 3, cy - r / 2, cx, cy - r / 6, 1);
}

static void ic_wrench(Canvas &c, int cx, int cy, int r) {
    c.circle(cx - r / 2, cy - r / 2, r / 3, 1);
    c.line(cx - r / 3, cy - r / 3, cx + r * 2 / 3, cy + r * 2 / 3, 1);
    c.line(cx - r / 4, cy - r / 2, cx + r * 3 / 4, cy + r / 2, 1);
}

// --- the map ------------------------------------------------------------------

typedef void (*IconFn)(Canvas &, int, int, int);

struct Entry { const char *key; IconFn fn; };

static const Entry kMap[] = {
    { "wifi",      ic_wifi },
    { "bt",        ic_ble },
    { "cc1101",    ic_subghz },
    { "sx1276",    ic_lora },
    { "msg",       ic_msg },
    { "pn532",     ic_nfc },
    { "ir_rx",     ic_ir },
    { "gps",       ic_gps },
    { "dht11",     ic_climate },
    { "battery",   ic_battery },
    { "clock",     ic_clock },
    { "rtc",       ic_clock },
    { "files",     ic_files },
    { "shell",     ic_shell },
    { "radar",     ic_radar },
    { "presence",  ic_radar },
    { "wardrive",  ic_wifi },
    { "notes",     ic_bell },
    { "power",     ic_power },
    { "diag",      ic_chip },
    { "check",     ic_chip },
    { "store",     ic_store },
    { "fix",       ic_wrench },
    { "cmds",      ic_shell },
    { "res",       ic_chip },
    { "logs",      ic_files },
    { "scripts",   ic_files },
    { "settings",  ic_gear },
    // The five category folders borrow an app's icon, so a folder is
    // recognisable as the kind of thing inside it rather than being five
    // identical boxes.
    { "Wireless",  ic_wifi },
    { "Sensors",   ic_climate },
    { "Tools",     ic_wrench },
    { "System",    ic_gear },
    { "Testing",   ic_chip },
};

void draw(Canvas &c, const char *key, int cx, int cy, int r, const char *label_fallback) {
    if (key) {
        for (unsigned i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
            if (strcmp(key, kMap[i].key) == 0) { kMap[i].fn(c, cx, cy, r); return; }
        }
        // Settings groups all share the gear rather than each needing one.
        if (strncmp(key, "set_", 4) == 0) { ic_gear(c, cx, cy, r); return; }
    }

    // A box with the initial in it. Deliberately not a question mark: a new app
    // with no icon yet should look like an app, and a blurry custom glyph is
    // worse than a clear shared one.
    c.rounded_rect(cx - r * 2 / 3, cy - r * 2 / 3, r * 4 / 3, r * 4 / 3, 1, false);
    const char *s = label_fallback && *label_fallback ? label_fallback : "?";
    char one[2] = { s[0], 0 };
    int scale = r >= 10 ? 2 : 1;
    c.text(cx - (FONT_W * scale) / 2, cy - (FONT_H * scale) / 2, one, 1, scale);
}

}  // namespace icons
}  // namespace nova
