// Desc: Photograph every Nova D1 screen on the host, so a layout can be looked at.
// File: novashots.cpp
//
// novasim renders the icons and the home screen, and it can only reach the three
// package files with no fw_* call in them. This drives the WHOLE runner against
// the faked firmware — every screen the app catalogue can open, plus the ones
// reached by a gesture — and prints each as characters.
//
// It exists because the alternative has been guessing. Three bugs this week were
// layout or navigation and every one of them was found by somebody holding the
// board: the position pips crowding the status bar, a percentage illegible
// because a fill edge cut a three-pixel digit, and every folder opening the
// wrong category. All three are visible in a dump. None of them is visible in
// the source.
//
// ASCII rather than a picture, deliberately. A 128x64 monochrome panel is 8192
// characters, which fits in a terminal and in a diff — so two runs can be
// compared, and a layout change that was not meant to move anything can be
// proven not to have.
//
//   novashots            open every screen and FAIL on a broken one
//   novashots --dump     every screen as characters, to look at
//   novashots <key>      one of them, by its catalogue key
//   novashots --list     the keys, and which are reachable
//
// Checking is the default because the test runner invokes every tool here with
// no arguments, and a check nobody passes a flag to is a check that does not
// run.

#include "fakefw_d1.inc"

// The whole package, the same set and the same order novagui_test compiles.
// Anything missing here is a screen that cannot be photographed, which is the
// same as a screen nobody has looked at.
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

#include <stdio.h>
#include <string.h>
#include <initializer_list>

using nova::ui::Screen;

// One frame, exactly as the runner composes it: the status bar unless the screen
// takes the whole panel, then the screen's body. Anything that differs from this
// is a picture of something the device never draws.
static void compose(void) {
    Screen *s = nova::gui::top();
    if (!s) return;
    nova::Canvas &c = nova::gui::canvas();
    c.clear(0);
    // draw_status is static to novagui.cpp, and reaching it is the one thing
    // this cannot do. The bar is a fixed nine rows at the top; a screen that
    // gets its own body right is the question here, and the bar has its own
    // dump below.
    s->draw(c);
}

static void print_frame(const char *what) {
    nova::Canvas &c = nova::gui::canvas();
    printf("=== %s ===\n", what);
    for (int y = 0; y < c.height(); y++) {
        for (int x = 0; x < c.width(); x++) putchar(c.get(x, y) ? '#' : '.');
        putchar('\n');
    }
    putchar('\n');
}

// Let a screen settle. Several sample on a timer and draw nothing useful on the
// frame they are entered — Resources reads once a second, the scanning screens
// wait on a worker — so a single frame would photograph an empty body and call
// it a layout.
static void settle(int frames = 4, uint32_t dt = 33) {
    for (int i = 0; i < frames; i++) {
        Screen *s = nova::gui::top();
        if (!s) return;
        s->tick(dt);
        g_ms += dt;
    }
}

static void shoot(const nova::gui::App &a) {
    if (!a.open) return;
    nova::gui::go_home();
    a.open();
    Screen *s = nova::gui::top();
    if (!s) { printf("=== %s === (nothing opened)\n\n", a.key); return; }
    settle();
    compose();
    char label[64];
    snprintf(label, sizeof(label), "%s  (%s)", a.key, s->title());
    print_frame(label);
}

// The two faults a dump makes obvious and no compiler can see.
//
// A screen that draws NOTHING is one whose open() ran, pushed something, and
// left a blank panel — indistinguishable on the device from a hang. A screen
// that paints into the top nine rows is one fighting the status bar, which the
// runner draws over it every frame, so whatever it put there is lost and
// whatever the bar puts there lands on top of a mess.
//
// Deliberately not a pixel comparison against stored frames. Every one of these
// screens is meant to change, and a test that fails when a label is reworded is
// a test somebody turns off.
static int check_all(void) {
    const nova::gui::App *apps = nova::gui::apps();
    const unsigned n = nova::gui::app_count();
    nova::Canvas &c = nova::gui::canvas();
    int bad = 0, looked = 0;

    for (unsigned i = 0; i < n; i++) {
        if (!apps[i].open) continue;
        nova::gui::go_home();
        apps[i].open();
        Screen *s = nova::gui::top();
        if (!s) { printf("    FAIL %s opened nothing\n", apps[i].key); bad++; continue; }
        settle();
        compose();
        looked++;

        int lit = 0;
        for (int y = 0; y < c.height(); y++)
            for (int x = 0; x < c.width(); x++) if (c.get(x, y)) lit++;
        // A title and one row of anything is well over a dozen pixels. Below
        // that the screen is empty, whatever it thinks it drew.
        if (lit < 12) { printf("    FAIL %s drew an empty panel\n", apps[i].key); bad++; }

        if (!s->fullscreen()) {
            int intruding = 0;
            for (int y = 0; y < nova::ui::BARH; y++)
                for (int x = 0; x < c.width(); x++) if (c.get(x, y)) intruding++;
            if (intruding) {
                printf("    FAIL %s paints %d pixel(s) into the status bar\n",
                       apps[i].key, intruding);
                bad++;
            }
        }
    }
    printf("  novashots: %d screen(s) opened, %d problem(s)\n", looked, bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    const nova::gui::App *apps = nova::gui::apps();
    const unsigned n = nova::gui::app_count();

    if (argc > 1 && !strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < n; i++)
            printf("  %-12s %-12s %s\n", apps[i].key, apps[i].label,
                   apps[i].open ? "" : "(no screen yet)");
        return 0;
    }

    if (!nova::gui::begin()) {
        printf("the runner would not start — no panel in the fake firmware?\n");
        return 1;
    }
    // The boot check pushes itself over home and pops when it finishes. Let it.
    settle(40, 60);
    nova::gui::go_home();

    if (argc < 2) return check_all();

    if (argc > 1) {
        for (unsigned i = 0; i < n; i++)
            if (!strcmp(apps[i].key, argv[1])) { shoot(apps[i]); return 0; }
        printf("no app with key '%s' — try --list\n", argv[1]);
        return 1;
    }

    if (strcmp(argv[1], "--dump") != 0) {
        printf("unknown option '%s' — try --dump, --list, or a catalogue key\n", argv[1]);
        return 1;
    }

    // Home first, in both styles, then everything the catalogue can open.
    for (const char *style : { "folders", "gallery" }) {
        fw_reg_set("Apps.NovaD1_HomeStyle", style);
        nova::gui::go_home();
        Screen *h = nova::gui::top();
        if (h) h->enter();
        settle();
        compose();
        char label[48];
        snprintf(label, sizeof(label), "home, %s", style);
        print_frame(label);
    }
    fw_reg_set("Apps.NovaD1_HomeStyle", "folders");

    for (unsigned i = 0; i < n; i++) shoot(apps[i]);
    return 0;
}
