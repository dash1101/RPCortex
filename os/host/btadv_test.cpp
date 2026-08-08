// BLE advertising, on the host: the payloads byte for byte, and the Ping screen
// rendered so it cannot go blank unnoticed.
//
// The radio cannot be exercised here — no chip, no phone — but the two things
// that fail SILENTLY on the device can. A wrong AD byte transmits fine and
// raises no card; a screen that draws nothing looks exactly like a hang. So this
// checks the advertisement builders field by field against their sourced formats
// (Apple Continuity proximity pairing, Google Fast Pair, and the name/raw
// primitives), and then drives the runner far enough to photograph the Ping
// screen the same way novashots photographs the rest.
//
// The transmit path itself and the scan/advertise serialisation live behind the
// radio and are DEVICE-UNCONFIRMED; they are reviewed, not run.

#include <stdio.h>
#include <string.h>
#include <stdint.h>

// The pure builders, straight out of the firmware command, with nothing else
// from bt.cpp pulled in (no btstack, no command table).
#define BT_ADV_BUILDER_ONLY
#include "../shell/bt.cpp"
#undef BT_ADV_BUILDER_ONLY

// The Nova D1 runner with the hardware faked, so the Ping screen can be opened
// and drawn. The same set and order novagui_test and novashots compile.
#include "fakefw_d1.inc"
#include "../apps/novad1/novacore.cpp"
#include "../apps/novad1/novacanvas.cpp"
#include "../apps/novad1/novaicons.cpp"
#include "../apps/novad1/novaboard.cpp"
#include "../apps/novad1/novamodtab.cpp"
#include "../apps/novad1/novalog.cpp"
#include "../apps/novad1/novanotify.cpp"
#include "../apps/novad1/novapower.cpp"
#include "../apps/novad1/display.cpp"
#include "../apps/novad1/novainput.cpp"
#include "../apps/novad1/novaui.cpp"
#include "../apps/novad1/novabootcheck.cpp"
#include "../apps/novad1/novagui_tools.cpp"
#include "../apps/novad1/novagui_system.cpp"
#include "../apps/novad1/novakeys.cpp"
#include "../apps/novad1/novagui_wifi.cpp"
#include "../apps/novad1/novagui_files.cpp"
#include "../apps/novad1/novagui_settings.cpp"
#include "../apps/novad1/novagui_ops.cpp"
#include "../apps/novad1/novagui_apps.cpp"
#include "../apps/novad1/novagui_ble.cpp"
#include "../apps/novad1/novagui_tasks.cpp"
#include "../apps/novad1/novagui.cpp"

static int checks, failures;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { failures++; printf("    FAIL %s\n", what); }
}
static void eqi(int got, int want, const char *what) {
    checks++;
    if (got != want) { failures++; printf("    FAIL %s: got %d want %d\n", what, got, want); }
}

// --- the payloads --------------------------------------------------------------

static void test_apple_ping(void) {
    uint8_t ad[31];
    size_t n = bt_ad_apple_ping(ad, sizeof(ad), 0x0E20, 0xDEADBEEF);
    eqi((int)n, 31, "apple ping is a full 31-octet advertisement");
    // 1E FF 4C 00 07 19: element length, Manufacturer Specific Data, Apple's
    // company id little-endian, Continuity Proximity Pairing type, 25-byte len.
    eqi(ad[0], 0x1E, "apple element length");
    eqi(ad[1], 0xFF, "apple is manufacturer specific data");
    eqi(ad[2], 0x4C, "apple company id low byte");
    eqi(ad[3], 0x00, "apple company id high byte");
    eqi(ad[4], 0x07, "apple continuity type is proximity pairing");
    eqi(ad[5], 0x19, "apple message length is 25");
    eqi(ad[7], 0x0E, "model id high byte, big-endian");
    eqi(ad[8], 0x20, "model id low byte");
    eqi(ad[9], 0x55, "status byte");

    // The opaque tail differs per seed, which is what makes each burst read as a
    // fresh device instead of a repeat the phone dismisses.
    uint8_t a2[31];
    bt_ad_apple_ping(a2, sizeof(a2), 0x0E20, 0xDEADBEEF);
    ok(memcmp(ad, a2, 31) == 0, "same seed gives the same payload");
    bt_ad_apple_ping(a2, sizeof(a2), 0x0E20, 0x12345678);
    ok(memcmp(ad + 15, a2 + 15, 16) != 0, "a different seed moves the opaque tail");
    // The structural bytes never move, whatever the seed.
    ok(memcmp(ad, a2, 10) == 0, "the structural bytes are seed-independent");
}

static void test_fastpair_ping(void) {
    uint8_t ad[31];
    size_t n = bt_ad_fastpair_ping(ad, sizeof(ad), 0xCD8256, 0xABCDEF01);
    eqi((int)n, 14, "fast pair advertisement is 14 octets");
    // 03 03 2C FE: a 16-bit Service UUID list carrying Fast Pair's 0xFE2C.
    eqi(ad[0], 0x03, "uuid list element length");
    eqi(ad[1], 0x03, "incomplete 16-bit service uuid list");
    eqi(ad[2], 0x2C, "fast pair uuid low byte");
    eqi(ad[3], 0xFE, "fast pair uuid high byte");
    // 06 16 2C FE <model24,BE>: Service Data for 0xFE2C then the 24-bit model id.
    eqi(ad[4], 0x06, "service data element length");
    eqi(ad[5], 0x16, "service data ad type");
    eqi(ad[6], 0x2C, "service data uuid low byte");
    eqi(ad[7], 0xFE, "service data uuid high byte");
    eqi(ad[8],  0xCD, "model id byte 0, big-endian");
    eqi(ad[9],  0x82, "model id byte 1");
    eqi(ad[10], 0x56, "model id byte 2");
    eqi(ad[11], 0x02, "tx power element length");
    eqi(ad[12], 0x0A, "tx power ad type");
}

static void test_name(void) {
    uint8_t ad[31];
    bool trunc = true;
    size_t n = bt_ad_build_name(ad, sizeof(ad), "RPCortex", &trunc);
    // Flags(3) + length + type + 8 chars = 13.
    eqi((int)n, 13, "a short name is flags plus a name element");
    eqi(ad[0], 0x02, "flags element length");
    eqi(ad[1], 0x01, "flags ad type");
    eqi(ad[2], 0x06, "LE general discoverable, no BR/EDR");
    eqi(ad[3], 0x09, "name element length is name+1");
    eqi(ad[4], 0x09, "complete local name type");
    ok(memcmp(ad + 5, "RPCortex", 8) == 0, "the name is on the air verbatim");
    ok(!trunc, "a short name is not marked truncated");

    // A long name is cut to fit 31 octets and switches to Shortened Local Name.
    n = bt_ad_build_name(ad, sizeof(ad), "abcdefghijklmnopqrstuvwxyz0123456789", &trunc);
    eqi((int)n, 31, "a long name fills the advertisement exactly");
    eqi(ad[4], 0x08, "and is marked as a shortened local name");
    ok(trunc, "and reports that it was truncated");
}

static void test_hex(void) {
    uint8_t ad[31];
    eqi((int)bt_ad_from_hex(ad, sizeof(ad), "1eff4c00"), 4, "plain hex parses");
    ok(ad[0] == 0x1E && ad[1] == 0xFF && ad[2] == 0x4C && ad[3] == 0x00, "the bytes are right");
    eqi((int)bt_ad_from_hex(ad, sizeof(ad), "1e:ff 4c-00"), 4, "colons, spaces and dashes are ignored");
    ok(ad[0] == 0x1E && ad[3] == 0x00, "and give the same bytes");
    eqi((int)bt_ad_from_hex(ad, sizeof(ad), "1ef"), 0, "an odd number of digits is rejected");
    eqi((int)bt_ad_from_hex(ad, sizeof(ad), "12xy"), 0, "a non-hex digit is rejected");
    // 32 bytes of hex is one past the 31-octet ceiling.
    eqi((int)bt_ad_from_hex(ad, sizeof(ad),
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"), 0,
        "more than 31 bytes is rejected");
}

static void test_rand_addr(void) {
    uint8_t a[6], b[6];
    bt_adv_rand_addr(a, 1);
    eqi(a[0] & 0xC0, 0xC0, "a ping address is a static random address");
    bt_adv_rand_addr(b, 1);
    ok(memcmp(a, b, 6) == 0, "the same seed gives the same address");
    bt_adv_rand_addr(b, 2);
    ok(memcmp(a, b, 6) != 0, "a different seed gives a different address");
}

// --- the screen ----------------------------------------------------------------
//
// The novashots discipline, aimed at the one screen novashots cannot reach on
// its own: it is opened by a gesture from BLE, not by a catalogue row, so the
// tool that walks the catalogue never photographs it. Here it is walked to and
// checked for a non-empty panel, entered and after a ping fired, so a Ping
// screen that draws nothing fails the build the same way a blank catalogue
// screen does.

static int lit_pixels(void) {
    nova::Canvas &c = nova::gui::canvas();
    int lit = 0;
    for (int y = 0; y < c.height(); y++)
        for (int x = 0; x < c.width(); x++)
            if (c.get(x, y)) lit++;
    return lit;
}

static void settle(int frames = 4, uint32_t dt = 33) {
    for (int i = 0; i < frames; i++) {
        nova::ui::Screen *s = nova::gui::top();
        if (!s) return;
        s->tick(dt);
        g_ms += dt;
    }
}

static void redraw(void) {
    nova::ui::Screen *s = nova::gui::top();
    if (!s) return;
    nova::Canvas &c = nova::gui::canvas();
    c.clear(0);
    s->draw(c);
}

static void test_ping_screen(void) {
    using namespace nova;

    ok(gui::begin(), "the runner starts");
    settle(40, 60);                 // let the boot check pop itself
    gui::go_home();

    // Open the BLE app the way home would.
    const gui::App *apps = gui::apps();
    const unsigned n = gui::app_count();
    bool opened = false;
    for (unsigned i = 0; i < n; i++)
        if (!strcmp(apps[i].key, "bt") && apps[i].open) { apps[i].open(); opened = true; break; }
    ok(opened, "the BLE app is in the catalogue and opens");

    ui::Screen *ble = gui::top();
    ok(ble && !strcmp(ble->title(), "BLE"), "the BLE screen is up");

    // Row zero is the pinned Ping entry. Select it.
    if (ble) ble->on_event(EV_SELECT);
    ui::Screen *ping = gui::top();
    ok(ping && ping != ble, "the top row opens another screen");
    ok(ping && !strcmp(ping->title(), "Ping"), "and it is the Ping screen");

    // Idle: it must draw its target selector and hint, not a blank panel.
    settle();
    redraw();
    ok(lit_pixels() >= 12, "the Ping screen draws a non-empty panel");

    // It must not paint into the status bar's rows, which the runner overwrites.
    nova::Canvas &c = gui::canvas();
    int intruding = 0;
    if (ping && !ping->fullscreen())
        for (int y = 0; y < ui::BARH; y++)
            for (int x = 0; x < c.width(); x++)
                if (c.get(x, y)) intruding++;
    eqi(intruding, 0, "and stays out of the status bar");

    // Fire a ping and confirm the advertising view still draws (a countdown and
    // a spinner). fw_task_spawn is counted, not run, in the fake, so the burst
    // stays "in flight" — which is exactly the state a real one would render.
    if (ping) ping->on_event(EV_SELECT);
    settle();
    redraw();
    ok(lit_pixels() >= 12, "after SELECT the advertising view still draws");

    // Rotating and pressing again must not trap or blank it.
    if (ping) { ping->on_event(EV_ROT_CW); ping->on_event(EV_SELECT); }
    settle();
    redraw();
    ok(lit_pixels() >= 12, "it survives rotation and a second press");
    ok(gui::top() == ping, "and is still the screen on top");
}

#define STAGE(f) do { fprintf(stderr, "  .. %s\n", #f); f(); } while (0)

int main(void) {
    STAGE(test_apple_ping);
    STAGE(test_fastpair_ping);
    STAGE(test_name);
    STAGE(test_hex);
    STAGE(test_rand_addr);
    STAGE(test_ping_screen);

    printf("  %d checks", checks);
    if (failures) printf(", %d FAILED", failures);
    printf("\n");
    return failures ? 1 : 0;
}
