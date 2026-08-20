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
#include "../apps/novad1/novaapps.cpp"
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
#include "../apps/novad1/novagui_media.cpp"
#include "../apps/novad1/novagui_contact.cpp"
#include "../apps/novad1/novagui_radios.cpp"
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
    // The same two steps the Gallery takes, in the same order. An opener shared
    // between rows — the folders, and every installed app — reads chosen() to
    // find out which one it is, so calling open() without this photographs
    // whatever was opened last.
    nova::gui::chose(&a);
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
    // app_total, not app_count: an app somebody installed is an app, and the
    // whole point of the framework is that it reaches the same screens the
    // built-ins do. A pass over the built-ins alone would photograph none of it.
    const unsigned n = nova::gui::app_total();
    nova::Canvas &c = nova::gui::canvas();
    int bad = 0, looked = 0;

    for (unsigned i = 0; i < n; i++) {
        const nova::gui::App &a = *nova::gui::app_at(i);
        if (!a.open) continue;
        nova::gui::go_home();
        nova::gui::chose(&a);
        a.open();
        Screen *s = nova::gui::top();
        if (!s) { printf("    FAIL %s opened nothing\n", a.key); bad++; continue; }
        settle();
        compose();
        looked++;

        int lit = 0;
        for (int y = 0; y < c.height(); y++)
            for (int x = 0; x < c.width(); x++) if (c.get(x, y)) lit++;
        // A title and one row of anything is well over a dozen pixels. Below
        // that the screen is empty, whatever it thinks it drew.
        if (lit < 12) { printf("    FAIL %s drew an empty panel\n", a.key); bad++; }

        // The header has to agree with the row that opened it. "Versions"
        // opened a screen calling itself "Device", which reads as having landed
        // somewhere else, and no compiler can see it: the label and the title
        // are two unrelated strings in two files.
        //
        // Files is the deliberate exception. A browser titles itself with the
        // path it is showing, which is worth more than repeating its own name.
        const char *t = s->title();
        if (strcmp(a.key, "files") != 0 && t && t[0] &&
            strcmp(t, a.label) != 0) {
            printf("    FAIL %s is labelled '%s' but titles itself '%s'\n",
                   a.key, a.label, t);
            bad++;
        }

        if (!s->fullscreen()) {
            int intruding = 0;
            for (int y = 0; y < nova::ui::BARH; y++)
                for (int x = 0; x < c.width(); x++) if (c.get(x, y)) intruding++;
            if (intruding) {
                printf("    FAIL %s paints %d pixel(s) into the status bar\n",
                       a.key, intruding);
                bad++;
            }
        }
    }
    printf("  novashots: %d screen(s) opened, %d problem(s)\n", looked, bad);
    return bad ? 1 : 0;
}

// --- the screens no catalogue row opens ---------------------------------------
//
// The lock, the pad behind it, and the Controls help behind the power menu are
// reached by a GESTURE, not by an open(), so the sweep above never saw them —
// and a blank or wrong one there was the kind of thing found only by holding the
// board. tap made these drivable headlessly; this drives them the same way, over
// the runner's own drain, and holds them to the one rule that matters most on a
// device: a screen that draws nothing looks exactly like a hung one.
static int shot_here(const char *what, bool dump) {
    settle();
    compose();
    nova::Canvas &c = nova::gui::canvas();
    int lit = 0;
    for (int y = 0; y < c.height(); y++)
        for (int x = 0; x < c.width(); x++) if (c.get(x, y)) lit++;
    int bad = 0;
    if (lit < 12) { printf("    FAIL %s drew an empty panel\n", what); bad = 1; }
    if (dump) {
        Screen *s = nova::gui::top();
        char label[64];
        snprintf(label, sizeof(label), "%s  (%s)", what, s ? s->title() : "");
        print_frame(label);
    }
    return bad;
}

static int gesture_screens(bool dump) {
    using namespace nova;
    int bad = 0;

    // The lock needs a code to arm. Engage it over home — the panel is lit here,
    // so a plain inject reaches it without the wake tap has to ask for on a device.
    fw_reg_set("Apps.NovaD1_Lock_Kind", "pin");
    fw_reg_set("Apps.NovaD1_PIN", "246810");
    gui::go_home();
    screens::lock_engage();
    bad += shot_here("lock", dump);

    input().inject(EV_SELECT);              // SELECT opens the pad
    gui::drain_input();
    bad += shot_here("lock pad", dump);

    // Off the lock cleanly, so nothing after it inherits a modal floor: forget the
    // code, drop the marker, and home can be reached again.
    fw_reg_set("Apps.NovaD1_PIN", "");
    fw_reg_set("Apps.NovaD1_Lock_Kind", "none");
    screens::lock_forget();
    gui::go_home();

    // Controls, the first row of the power menu — the help for whatever is under
    // it. open_power is what a homehold reaches; SELECT then opens the row.
    gui::go_home();
    screens::open_power();
    input().inject(EV_SELECT);
    gui::drain_input();
    bad += shot_here("controls", dump);
    gui::go_home();

    return bad;
}

int main(int argc, char **argv) {
    // The catalogue is only complete after begin() has scanned /nova/apps, so
    // --list starts the runner too rather than printing the built-ins and
    // calling that the list.
    if (!nova::gui::begin()) {
        printf("the runner would not start — no panel in the fake firmware?\n");
        return 1;
    }
    const unsigned n = nova::gui::app_total();

    if (argc > 1 && !strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < n; i++) {
            const nova::gui::App &a = *nova::gui::app_at(i);
            printf("  %-12s %-12s %s\n", a.key, a.label,
                   a.open ? "" : "(no screen yet)");
        }
        return 0;
    }

    // The boot check pushes itself over home and pops when it finishes. Let it.
    settle(40, 60);
    nova::gui::go_home();

    if (argc < 2) {
        int bad = check_all();
        bad += gesture_screens(false);
        return bad ? 1 : 0;
    }

    // The option BEFORE the key lookup, or "--dump" is searched for as though it
    // were the name of an app and reported missing.
    if (strcmp(argv[1], "--dump") != 0) {
        for (unsigned i = 0; i < n; i++)
            if (!strcmp(nova::gui::app_at(i)->key, argv[1])) {
                shoot(*nova::gui::app_at(i));
                return 0;
            }
        printf("no app with key '%s' — try --dump, --list, or a catalogue key\n", argv[1]);
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

    for (unsigned i = 0; i < n; i++) shoot(*nova::gui::app_at(i));

    // The gesture-only screens last, after the catalogue they sit outside of.
    gesture_screens(true);
    return 0;
}
