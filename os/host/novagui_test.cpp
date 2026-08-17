// The Nova D1 runner, on the host, with the hardware faked.
//
// This exists because of a crash that reached a desk: two service entries both
// ran `novad1 gui --bg`, the second called begin() while the first was drawing,
// and begin() reset the screen stack under it. Two tasks then owned one static
// pool, a slot's vtable pointer was read half-written, and the device jumped to
// address zero. It took the board down twice before the cause was found.
//
// Nothing about that needed hardware. It needed the runner to be startable
// twice, which is one line to arrange here and was not arrangeable at all
// before, because the runner had never been compiled off the device.
//
// So this harness runs the REAL novagui — the real screen stack, the real
// catalogue, the real status bar — against stub firmware, and drives it with
// scripted gestures. What it can catch is everything that is logic rather than
// electrons: stack depth, screen lifetimes, navigation that traps, a runner
// started twice.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../include/rpc_app.h"

#include "fakefw_d1.inc"

// The real thing, all of it.
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

static int checks, failures;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { failures++; printf("    FAIL %s\n", what); }
}
static void eq(int got, int want, const char *what) {
    checks++;
    if (got != want) { failures++; printf("    FAIL %s: got %d want %d\n", what, got, want); }
}
static void streq_(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got ? got : "(null)", want)) {
        failures++;
        printf("    FAIL %s: got '%s' want '%s'\n", what, got ? got : "(null)", want);
    }
}

// Drive the runner the way the loop does, without entering run() — one frame,
// with whatever gestures were injected.
static void frame(uint32_t dt = 33) {
    using namespace nova;
    g_ms += dt;
    Event e;
    bool turned = false;
    while ((e = input().next()) != EV_NONE) {
        if (e == EV_ROT_CW || e == EV_ROT_CCW) {
            if (turned) { input().unget(e); break; }
            turned = true;
        }
        ui::Screen *s = gui::top();
        if (!s) continue;
        ui::Action a = s->on_event(e);
        if (a == ui::ACT_STAY && e == EV_HOME && !s->modal()) a = ui::ACT_HOME;
        if      (a == ui::ACT_BACK) gui::pop();
        else if (a == ui::ACT_HOME) gui::go_home();
    }
    ui::Screen *s = gui::top();
    if (s) s->tick(dt);
    s = gui::top();                       // it may have popped itself
    if (s) {
        gui::canvas().clear(0);
        s->draw(gui::canvas());
    }
}

// --- one detent must animate ---------------------------------------------------------
//
// The bug this covers reached a desk twice and survived two attempts at fixing
// it, because the harness above takes dt as a parameter and the runner does not
// — it computes it, and the computation was the bug. So this drives
// gui::frame_dt, the runner's own rule, rather than a copy of it.
//
// The shape on the device: the screen sits idle napping NAP_IDLE, somebody
// turns the knob one detent, and the first tick of the new slide is handed the
// whole idle gap. NAP_IDLE is 140 ms, SLIDE_MS is 140 ms, so the step is
// exactly 256 and the animation is over before a single frame is drawn.

static void test_one_detent_animates(void) {
    using namespace nova;

    // The runner's rule, driven the way the runner drives it.
    uint32_t last = 1000;
    ok(gui::frame_dt(1140, &last, false) == 140, "an idle frame sees the whole nap");
    ok(gui::frame_dt(1156, &last, false) == 16,  "and a moving one sees a frame");

    // The frame a gesture lands on starts the clock again, so the idle gap
    // before it is not spent out of the animation.
    last = 1000;
    ok(gui::frame_dt(1300, &last, true) == 0, "a gesture frame hands tick nothing");
    ok(gui::frame_dt(1316, &last, false) == 16, "and the one after it a frame");

    // End to end: from idle, ONE detent has to take more than one frame to
    // arrive. Any number above one proves it animates; the real slide is about
    // eight frames at 140 ms and 16 ms a frame.
    gui::go_home();
    input().flush();
    input().inject(EV_ROT_CW);

    uint32_t clock = 5000, mark = 5000 - 300;   // 300 ms of stillness behind it
    int frames = 0;
    bool had = false;
    for (int i = 0; i < 40; i++) {
        Event e;
        had = false;
        while ((e = input().next()) != EV_NONE) {
            had = true;
            ui::Screen *t = gui::top();
            if (t) t->on_event(e);
        }
        uint32_t dt = gui::frame_dt(clock, &mark, had);
        ui::Screen *t = gui::top();
        if (!t) break;
        t->tick(dt);
        if (!t->animating()) break;
        frames++;
        clock += 16;                             // NAP_ANIMATING
    }
    ok(frames > 1, "one detent from idle animates over more than a single frame");
    ok(frames >= 4, "and takes enough frames to read as movement");
}

// --- a folder opens ITS OWN category --------------------------------------------------
//
// Every folder opened the first one. An OpenFn takes nothing and returns
// nothing, so open_category had no way to know which row invoked it; it tried
// to read the answer back off the gallery through an unchecked cast, and the
// value it read was never written. Reported from the device as "the System
// folder takes me to Network, and so do all the others".
//
// Nothing in the type system could catch that, so it is worth a test: walk to
// each folder in turn, open it, and check the title is the folder's own.

static void test_folders_open_themselves(void) {
    using namespace nova;

    fw_reg_set("Apps.NovaD1_HomeStyle", "folders");
    gui::go_home();
    ui::Screen *home = gui::top();
    if (!home) { ok(false, "home is up"); return; }
    home->enter();                       // re-read the style

    // How many folders there are is however many categories hold anything.
    int folders = 0;
    for (unsigned i = 0; i < gui::app_count(); i++) {
        bool seen = false;
        for (unsigned j = 0; j < i; j++)
            if (gui::apps()[j].cat == gui::apps()[i].cat) { seen = true; break; }
        if (!seen) folders++;
    }
    ok(folders > 1, "there is more than one folder to get wrong");

    int distinct = 0;
    static char titles[8][24];
    for (int f = 0; f < folders && f < 8; f++) {
        gui::go_home();
        ui::Screen *h = gui::top();
        if (!h) break;
        for (int step = 0; step < f; step++) h->on_event(EV_ROT_CW);
        h->on_event(EV_SELECT);

        ui::Screen *opened = gui::top();
        if (opened == h) { ok(false, "selecting a folder opens something"); break; }
        snprintf(titles[f], sizeof(titles[f]), "%s", opened->title());
        bool dupe = false;
        for (int k = 0; k < f; k++) if (!strcmp(titles[k], titles[f])) dupe = true;
        if (!dupe) distinct++;
    }
    // The whole bug in one check: N folders must open N DIFFERENT categories.
    eq(distinct, folders < 8 ? folders : 8, "each folder opens its own category");
    gui::go_home();

    // An app that cannot open must SAY so. Pressing a struck-through row used to
    // do nothing at all, and on a device whose only input is one encoder,
    // "nothing happened" is what a broken button looks like.
    // An app that cannot open must SAY so. Pressing a struck-through row used to
    // do nothing at all, and on a device whose only input is one encoder,
    // "nothing happened" is what a broken button looks like.
    //
    // Walked by PRESSING every row rather than by counting detents to a known
    // one. Two things make counting wrong: home lists a folder per non-empty
    // category, so a folder's row is not its category's enum value; and a
    // gallery remembers where its cursor was, deliberately, so row zero is not
    // where it starts. Counting found the System folder while looking for
    // Wireless and still opened something, which is a pass for the wrong
    // reason.
    const gui::App *apps = gui::apps();
    const unsigned n = gui::app_count();

    gui::go_home();
    ui::Screen *hh = gui::top();
    if (!hh) { ok(false, "home is up"); return; }
    hh->on_event(EV_SELECT);
    ui::Screen *gallery = gui::top();
    if (!gallery || gallery == hh) { ok(false, "a folder opened"); return; }

    const char *cat = gallery->title();
    int rows = 0, want = 0;
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(category_name(apps[i].cat), cat) != 0) continue;
        rows++;
        if (!gui::app_available(apps[i])) want++;
    }

    // Press each row in turn and note what it produced. Matching on the title
    // means it does not matter which row the cursor happened to start on.
    int answered = 0;
    for (int r = 0; r < rows; r++) {
        gallery->on_event(EV_SELECT);
        ui::Screen *opened = gui::top();
        if (opened && opened != gallery) {
            for (unsigned i = 0; i < n; i++)
                if (!gui::app_available(apps[i]) &&
                    strcmp(category_name(apps[i].cat), cat) == 0 &&
                    !strcmp(opened->title(), apps[i].label)) { answered++; break; }
            gui::pop();
        }
        gallery->on_event(EV_ROT_CW);
    }
    eq(answered, want,
       "every unavailable app in a folder says why instead of doing nothing");
    gui::go_home();
}

// Closing an app must leave the highlight ON that app.
//
// Reported from the board as "i open wardrive and close it, it wont close while
// hovering that app, itll hover the default app in that folder". Coming back to
// a screen re-ran set(), and set() zeroed the selection. It is checked here
// rather than by hand because hand-checking it is what let it ship.
static void test_selection_survives_a_round_trip(void) {
    using namespace nova;
    gui::go_home();
    ui::Screen *h = gui::top();
    if (!h) { ok(false, "home is up"); return; }

    // Into the first folder, then down to a row that is NOT the first — the bug
    // returns the highlight to row zero, so row zero cannot detect it.
    h->on_event(EV_SELECT);
    ui::Screen *gallery = gui::top();
    if (!gallery || gallery == h) { ok(false, "a folder opened"); return; }

    const int target_row = 2;
    for (int i = 0; i < target_row; i++) gallery->on_event(EV_ROT_CW);

    // What is under the cursor, named by what opening it produces.
    gallery->on_event(EV_SELECT);
    ui::Screen *first = gui::top();
    if (!first || first == gallery) { ok(false, "an app opened"); return; }
    static char opened[32];
    snprintf(opened, sizeof(opened), "%s", first->title());

    gui::pop();
    if (gui::top() != gallery) { ok(false, "back returns to the folder"); return; }

    // Press again WITHOUT rotating. The same app must come back.
    gallery->on_event(EV_SELECT);
    ui::Screen *again = gui::top();
    ok(again && again != gallery && !strcmp(again->title(), opened),
       "closing an app leaves the highlight on that app");
    gui::go_home();
}

// --- the bug that reached a desk ----------------------------------------------------

static void test_single_instance(void) {
    using namespace nova;
    ok(gui::begin(), "the first start succeeds");
    ok(gui::started(), "and is marked as started");
    unsigned depth_after_first = gui::depth();

    // The second start is what took the device down. It must be refused, and —
    // the part that actually mattered — it must not have touched the screen
    // stack on its way to being refused.
    ok(!gui::begin(), "a second start is refused");
    eq((int)gui::depth(), (int)depth_after_first, "and leaves the stack alone");

    for (int i = 0; i < 30; i++) frame();
    ok(gui::top() != nullptr, "the stack still has a screen on it");
}

// --- navigation -----------------------------------------------------------------------

static void test_navigation(void) {
    using namespace nova;
    // Past the boot screen.
    for (int i = 0; i < 60; i++) frame(50);
    eq((int)gui::depth(), 1, "the boot screen leaves on its own");

    // BACK at home does nothing. Falling out of the UI into a shell by pressing
    // back one time too many is not something anybody meant to do.
    for (int i = 0; i < 5; i++) { input().inject(EV_BACK); frame(); }
    eq((int)gui::depth(), 1, "BACK at home stays at home");

    // Into a folder and back out.
    input().inject(EV_SELECT); frame();
    ok(gui::depth() == 2, "SELECT opens a folder");
    input().inject(EV_BACK); frame();
    eq((int)gui::depth(), 1, "BACK leaves it");

    // HOME from depth, including from a screen that does not handle it — the
    // runner has to be the guarantee, not each screen's memory.
    input().inject(EV_SELECT); frame();
    input().inject(EV_SELECT); frame();
    ok(gui::depth() >= 2, "two levels down");
    input().inject(EV_HOME); frame();
    eq((int)gui::depth(), 1, "HOME always gets out");
}

// A screen stack cannot be pushed past its pool. This is the check that a
// static_assert cannot make, because the depth is a run-time count.
static void test_stack_bounds(void) {
    using namespace nova;
    unsigned before = gui::depth();
    for (int i = 0; i < 40; i++) { input().inject(EV_SELECT); frame(); }
    ok(gui::depth() <= gui::STACK_MAX, "the stack never exceeds its pool");
    for (int i = 0; i < 40; i++) gui::pop();
    eq((int)gui::depth(), 1, "and pops back to exactly home, never past it");
    (void)before;
}

// --- rotation ----------------------------------------------------------------------

static void test_one_step_per_frame(void) {
    using namespace nova;
    gui::go_home();
    // Six detents at once is a fast flick. The runner must take one per frame,
    // or the cursor teleports and the ring animation never plays.
    for (int i = 0; i < 6; i++) input().inject(EV_ROT_CW);
    frame();
    ok(input().pending(), "five of six detents are still queued after one frame");
    for (int i = 0; i < 6; i++) frame();
    ok(!input().pending(), "and all of them are consumed within six");
}

// --- the panel -----------------------------------------------------------------------

static void test_no_panel(void) {
    using namespace nova;
    // A failed start must give the claim back, or a device that could not find
    // its screen once could never be told to look again.
    //
    // stop() is what releases it here, because the loop was never entered.
    // Calling run() to unwind would block forever — which is how this harness
    // found that the claim had no owner on that path.
    gui::stop();
    ok(!gui::started(), "stop() releases a claim whose loop never started");
    g_panel_present = false;
    ok(!gui::begin(), "no panel means no start");
    ok(!gui::started(), "and the claim is released, not held");
    g_panel_present = true;
    ok(gui::begin(), "so it can be started again once the wiring is fixed");
}

// --- what the catalogue promises -------------------------------------------------------

static void test_catalogue(void) {
    using namespace nova;
    ok(gui::app_count() > 0, "there are apps");
    int with_screen = 0;
    for (unsigned i = 0; i < gui::app_count(); i++) {
        const gui::App &a = gui::apps()[i];
        ok(a.key && *a.key, "every app has a key");
        ok(a.label && *a.label, "every app has a label");
        if (a.open) with_screen++;
        // An app that names a module must name one that exists, or it is
        // permanently greyed for a reason nobody can find.
        if (a.module) ok(module_by_id(a.module) != nullptr, a.key);
    }
    ok(with_screen > 0, "some of them open something");
}


// --- an app somebody else installed -----------------------------------------------------
//
// The whole path, end to end: a file in /nova/apps becomes a catalogue row,
// the row opens a screen, the encoder moves down it, and SELECT runs the
// command the file named. That is the claim the framework makes, and none of
// it is provable by looking at any one piece.
//
// The fake filesystem carries two apps on purpose. devcheck.napp is written
// correctly; dice.napp says `app.kind: py`, which is exactly what somebody
// porting a MicroPython app writes, and it must be LISTED and must refuse to
// run rather than disappearing.

// Defined with the media screens further down, and wanted here too.
static int lit_pixels(void);

static const nova::gui::App *find_app(const char *key) {
    for (unsigned i = 0; i < nova::gui::app_total(); i++) {
        const nova::gui::App *a = nova::gui::app_at(i);
        if (a && !strcmp(a->key, key)) return a;
    }
    return nullptr;
}

// Open an app the way the Gallery does — record the choice, then call.
static nova::ui::Screen *open_app(const char *key) {
    const nova::gui::App *a = find_app(key);
    if (!a || !a->open) return nullptr;
    nova::gui::go_home();
    nova::gui::chose(a);
    a->open();
    return nova::gui::top();
}

static void test_installed_apps(void) {
    using namespace nova;

    ok(gui::app_total() == gui::app_count() + 2, "both installed apps joined the catalogue");

    const gui::App *dev = find_app("app_devcheck");
    ok(dev != nullptr, "the working one is there under its prefixed key");
    if (dev) {
        streq_(dev->label, "Device Check", "labelled with app.name");
        eq(dev->cat, CAT_SYSTEM, "in the folder its header asked for");
        ok(gui::app_available(*dev), "and it is openable");
    }
    // The prefix is what stops an app called cc1101.napp landing a second row
    // under a key a built-in already owns.
    for (unsigned i = 0; i < gui::app_count(); i++)
        ok(strncmp(gui::apps()[i].key, NAPP_KEY_PREFIX, 4) != 0,
           "no built-in wears the installed-app prefix");

    ui::Screen *s = open_app("app_devcheck");
    ok(s != nullptr, "it opens");
    if (!s) return;
    // novashots refuses a screen whose title disagrees with its row, and an
    // installed app has to hold to the same rule.
    streq_(s->title(), "Device Check", "and titles itself the way the home row is labelled");

    gui::canvas().clear(0);
    s->draw(gui::canvas());
    ok(lit_pixels() > 12, "it draws its rows");

    // The encoder. Down two, and the row that runs must be the third one in the
    // file rather than the first — a menu that ignores the encoder still looks
    // right in a photograph.
    s->on_event(EV_ROT_CW);
    s->on_event(EV_ROT_CW);
    ui::Menu *m = (ui::Menu *)s;
    eq(m->selected(), 2, "the encoder moves the cursor");

    g_last_shell[0] = 0;
    g_run_spawned = 1;                 // let the job finish inside the gesture
    shell_says("free", "  used 12K free 240K", 0);
    s->on_event(EV_SELECT);
    g_run_spawned = 0;
    streq_(g_last_shell, "free", "SELECT runs the command that row named");

    // The output arrives on the next tick, and it arrives in a pane.
    s->tick(33);
    ui::Screen *pane = gui::top();
    ok(pane != s, "the output opens a pane over the app");
    if (pane) streq_(pane->title(), "Memory", "titled with the row that produced it");
    gui::pop();

    // Coming BACK re-reads the file, so an app edited on the device picks its
    // new rows up without anybody leaving the screen.
    ok(gui::top() == s, "and BACK returns to the app");
    eq(napps::count_rows(), 4, "with its rows still loaded");

    // The one written the way a MicroPython app is written.
    ui::Screen *d = open_app("app_dice");
    ok(d != nullptr, "a kind:py app is listed and opens");
    if (d) {
        streq_(d->title(), "Dice", "under its own name");
        gui::canvas().clear(0);
        d->draw(gui::canvas());
        ok(lit_pixels() > 12, "and draws the reason it will not run");
        // It must not run anything. The row is there in the file; the refusal
        // has to happen before the rows are ever offered.
        g_last_shell[0] = 0;
        d->on_event(EV_SELECT);
        streq_(g_last_shell, "", "and pressing SELECT runs nothing at all");
    }

    // The file that is not an app was never a row.
    ok(find_app("app_readme") == nullptr, "a .txt in the apps folder is not an app");

    // The OTHER home style. Folders reach an installed app through its
    // category; the flat gallery reaches it by index out of an array that was
    // sized by the number of BUILT-IN apps, so this is the one place where
    // merging a thirty-sixth app writes past the end of a static.
    fw_reg_set("Apps.NovaD1_HomeStyle", "gallery");
    gui::go_home();
    ui::Screen *flat_home = gui::top();
    ok(flat_home != nullptr, "the flat home comes up");
    if (flat_home) {
        flat_home->enter();
        // One detent backwards from the first icon is the last one, and the
        // last one is the last app installed.
        flat_home->on_event(EV_ROT_CCW);
        flat_home->on_event(EV_SELECT);
        ui::Screen *last = gui::top();
        ok(last && last != flat_home, "and its last icon opens something");
        if (last && last != flat_home)
            streq_(last->title(), "Dice", "which is the last installed app");
    }
    fw_reg_set("Apps.NovaD1_HomeStyle", "folders");
    gui::go_home();
    ui::Screen *h2 = gui::top();
    if (h2) h2->enter();
}

// --- the media player -------------------------------------------------------------------
//
// novashots opens the CATALOGUE, and the player is one row of it with three
// screens behind it: a browser, a now-playing view and a speaker picker. None of
// those three can be photographed, so what a dump would have shown is asserted
// here instead — that they draw something, that they call themselves what they
// are, and that the four things underneath them are right.
//
// The four are the parts a device would otherwise be the first to find out
// about: the .wav filter, the WAV header reader, the command strings handed to
// the shell, and the parse of what btaudio prints back.

static int lit_pixels(void) {
    using namespace nova;
    Canvas &c = gui::canvas();
    ui::Screen *s = gui::top();
    if (!s) return 0;
    c.clear(0);
    s->draw(c);
    int n = 0;
    for (int y = 0; y < c.height(); y++)
        for (int x = 0; x < c.width(); x++) if (c.get(x, y)) n++;
    return n;
}

static void settle_top(int frames, uint32_t dt) {
    using namespace nova;
    for (int i = 0; i < frames; i++) {
        ui::Screen *s = gui::top();
        if (!s) return;
        s->tick(dt);
        g_ms += dt;
    }
}

static void test_media_parsing(void) {
    using namespace nova::screens;

    // The extension filter. readme.txt is in the fake filesystem for exactly
    // this: a music browser that lists it is a file manager.
    ok(media_is_wav("drift.wav"), "a .wav is playable");
    ok(media_is_wav("DRIFT.WAV"), "and the check is case-insensitive");
    ok(!media_is_wav("readme.txt"), "a .txt is not");
    ok(!media_is_wav("wav"), "and neither is a name that merely contains it");

    // The header, read through the ABI from the fake's synthesised RIFF. drift
    // carries a LIST/INFO chunk BEFORE its data chunk, which is the layout a
    // walk that assumes fmt-then-data reads as samples.
    MediaWav w;
    media_wav_read("/nova/music/Ambient/drift.wav", &w);
    eq(w.why, MEDIA_WAV_OK, "a tagged WAV parses");
    eq((int)w.rate, 44100, "its rate is read");
    eq((int)w.channels, 2, "and its channel count");
    ok(!strcmp(w.name, "Drift"), "INAM is the title");
    ok(!strcmp(w.artist, "Nova Labs"), "IART is the artist");
    eq((int)media_seconds(w), 3528044 / (44100 * 2 * 2), "and the length is arithmetic");

    // A file with no INFO must report NO artist rather than one invented from
    // its name. This is the check that stops the screen making something up.
    media_wav_read("/nova/music/beacon.wav", &w);
    eq(w.why, MEDIA_WAV_OK, "an untagged WAV parses too");
    ok(!w.artist[0], "and carries no artist at all");
    ok(!w.name[0], "and no title");
    eq((int)w.channels, 1, "mono is read as mono");

    media_wav_read("/nova/music/readme.txt", &w);
    eq(w.why, MEDIA_WAV_NONE, "something that is not a WAV is not read as one");

    // The stem is what a file with no INAM is called on screen.
    char stem[26];
    media_stem(stem, sizeof(stem), "signal-lost.wav");
    ok(!strcmp(stem, "signal-lost"), "the extension comes off the title");
    media_stem(stem, sizeof(stem), "noext");
    ok(!strcmp(stem, "noext"), "and a name with no extension survives whole");

    // The command handed to the shell. A space in a name is ordinary in music
    // and `btaudio play` takes one argument, so it has to be quoted.
    char line[RPC_SHELL_LINE_MAX];
    ok(media_play_line(line, sizeof(line), "/nova/music/my song.wav"),
       "a path with a space is accepted");
    ok(!strcmp(line, "btaudio play \"/nova/music/my song.wav\""),
       "and comes out quoted");
    // The tokeniser understands double quotes and nothing else, so a path with
    // one in it has no encoding and must be refused rather than mis-run.
    ok(!media_play_line(line, sizeof(line), "/nova/music/say \"hi\".wav"),
       "a path containing a quote is refused");
    char huge[300];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    ok(!media_play_line(line, sizeof(line), huge),
       "and a path too long for the shell is refused, not truncated");

    // What btaudio prints back. The text is the fake's, which is copied from the
    // real out_multi format strings.
    media_status_read(
        "[:] Bluetooth audio\n"
        "  Name       RPCortex\n"
        "  Speaker    C8:7B:23:11:04:9A\n"
        "  Playing    /nova/music/Ambient/drift.wav\n"
        "  Position   37%  (44100 Hz, 2 channels)\n"
        "  Volume     65%\n"
        "  Underruns  4\n");
    ok(g_media_linked, "a connected speaker is read as connected");
    ok(!strcmp(g_media_peer, "C8:7B:23:11:04:9A"), "and its address kept");
    eq(g_media_state, MEDIA_PLAYING, "a track is read as playing");
    eq(g_media_vol, 65, "the volume comes off the row");
    eq(g_media_prog, 37, "and the position");
    eq((int)g_media_under, 4, "and the underrun count");

    media_status_read(
        "[:] Bluetooth audio\n"
        "  Name       RPCortex\n"
        "  Speaker    not connected\n"
        "  Playing    nothing\n"
        "  Volume     80%\n"
        "  Underruns  0\n");
    ok(!g_media_linked, "'not connected' is not an address");
    eq(g_media_state, MEDIA_IDLE, "and 'nothing' is not a track");
    eq(g_media_prog, -1, "with no position printed, there is no progress to draw");

    // An EMPTY reply is a capture somebody else held, not an empty answer. It
    // must leave the picture alone rather than reporting a speaker gone.
    g_media_linked = true;
    media_status_read("");
    ok(g_media_linked, "an empty reply changes nothing");
    g_media_linked = false;
}

static void test_media_screens(void) {
    using namespace nova;
    using namespace nova::screens;

    gui::go_home();
    screens::open_media();
    ui::Screen *root = gui::top();
    if (!root) { ok(false, "the player opens"); return; }
    // The same rule novashots enforces on every catalogue row, checked here too
    // because this one is reached from a folder as well.
    ok(!strcmp(root->title(), "Media"), "the player titles itself as the row says");
    settle_top(4, 33);
    ok(lit_pixels() > 12, "and draws something");

    // Music -> the browser. Three rows, not four: readme.txt is filtered.
    root->on_event(EV_SELECT);
    ui::Screen *browse = gui::top();
    if (!browse || browse == root) { ok(false, "Music opens the browser"); return; }
    settle_top(2, 33);
    ok(lit_pixels() > 12, "the browser draws something");
    eq(g_media_rows, 3, "the browser lists folders and .wav files only");

    // Into Ambient, whose three tracks become the queue.
    browse->on_event(EV_SELECT);            // row 0 is the Ambient folder
    ui::Screen *amb = gui::top();
    if (!amb || amb == browse) { ok(false, "a folder opens"); return; }
    ok(!strcmp(amb->title(), "Ambient"), "and titles itself with the folder");
    eq(g_media_rows, 3, "which holds three tracks");

    // Play the first. The worker is run inline for this, so the whole round trip
    // happens: a command built, the shell answering, the reply parsed.
    g_run_spawned = 1;
    amb->on_event(EV_SELECT);
    ui::Screen *now = gui::top();
    if (!now || now == amb) { ok(false, "a track opens Now Playing"); g_run_spawned = 0; return; }
    ok(!strcmp(now->title(), "Now Playing"), "the transport titles itself");
    eq(g_media_qn, 3, "the folder became the queue");
    eq(g_media_pos, 0, "starting at the track that was chosen");
    ok(!strcmp(g_media_cmd, "btaudio play \"/nova/music/Ambient/drift.wav\""),
       "and the command names that track");

    // One tick reaps the reply; the poll behind it then reads the real state
    // back, which with the fake reporting nothing connected is 'not playing'.
    now->tick(33);
    ok(lit_pixels() > 12, "Now Playing draws something");

    // With the fake reporting a stream, the poll has to find it. This is the
    // path that also drives the end-of-track hand-off.
    g_bta_playing = 1;
    settle_top(2, 1600);
    eq(g_media_state, MEDIA_PLAYING, "a stream reported by status is picked up");
    ok(lit_pixels() > 12, "and the streaming layout still draws");

    // NEXT while playing stops first, because btaudio refuses a play over a
    // stream — and then starts the track it moved to.
    now->on_event(EV_ROT_CW);               // PLAY -> NEXT
    now->on_event(EV_SELECT);
    now->tick(33);
    eq(g_media_pos, 1, "NEXT moves to the second track");
    ok(!strcmp(g_media_cmd, "btaudio play \"/nova/music/Ambient/halo.wav\""),
       "and plays it once the stop has landed");

    // PREV from the first track wraps to the last rather than doing nothing.
    g_media_pos = 0;
    g_media_state = MEDIA_IDLE;
    now->on_event(EV_ROT_CCW);              // NEXT -> PLAY
    now->on_event(EV_ROT_CCW);              // PLAY -> PREV
    now->on_event(EV_SELECT);
    eq(g_media_pos, 2, "PREV from the first track wraps to the last");

    g_bta_playing = 0;
    g_run_spawned = 0;

    // The speaker picker, and what a scan gives it.
    gui::go_home();
    screens::open_media();
    root = gui::top();
    root->on_event(EV_ROT_CW);
    root->on_event(EV_ROT_CW);
    root->on_event(EV_SELECT);
    ui::Screen *spk = gui::top();
    if (!spk || spk == root) { ok(false, "the speaker picker opens"); return; }
    ok(!strcmp(spk->title(), "Speaker"), "and is called Speaker");
    settle_top(2, 33);
    ok(lit_pixels() > 12, "it draws something with nothing saved");

    // A classic inquiry, because A2DP is Classic and an LE scan finds a
    // different address that will not accept audio.
    g_run_spawned = 1;
    spk->on_event(EV_ROT_CW);
    spk->on_event(EV_SELECT);
    ok(!strcmp(g_media_cmd, "bt scan classic 10"), "the scan is a CLASSIC inquiry");
    spk->tick(33);
    eq(g_media_found_n, 2, "and its rows are parsed");
    ok(!strcmp(g_media_found[0].mac, "C8:7B:23:11:04:9A"), "address first");
    ok(!strcmp(g_media_found[0].name, "JBL Flip 5"), "then the name, spaces and all");
    ok(lit_pixels() > 12, "the found list draws");

    // Connecting to one remembers it — and only on success, or the next boot
    // offers a speaker that was never there.
    spk->on_event(EV_SELECT);               // the cursor is on the first result
    ok(!strcmp(g_media_cmd, "btaudio connect C8:7B:23:11:04:9A"),
       "connecting names the chosen address");
    spk->tick(33);
    ok(!strcmp(nova::reg(NOVA_KEY_PREFIX "Speaker", ""), "C8:7B:23:11:04:9A"),
       "a speaker that answered is remembered");

    g_run_spawned = 0;
    gui::go_home();
}

// --- the contact readers read the commands back correctly --------------------------
//
// novashots cannot reach this and must not be changed so that it can. Its
// fw_task_spawn COUNTS a spawn rather than running it, deliberately — that is
// what catches a second runner — so the worker never calls fw_shell_run and the
// reply is never parsed. The photograph is of the waiting state and nothing
// else.
//
// The parse is the half of these two screens that can be wrong without looking
// wrong: a row found by the wrong name puts a UID in the type line and reads as
// a fault in the reader. So the reply is fed straight in here, in the exact text
// shell/nfc.cpp and shell/ibutton.cpp print — the same strings the fake shell
// serves, kept in step with it on purpose.
static void test_contact_readers(void) {
    using namespace nova::screens;

    // A tag.
    snprintf(g_ct_out, sizeof(g_ct_out), "%s",
             "[:] Tag\n"
             "  UID       04 A2 B3 C4 D5 E6 F7\n"
             "  Type      NTAG / Ultralight\n"
             "  ATQA      44 00\n"
             "  SAK       00\n");
    g_ct_rc = 0;
    g_ct_ready = 1;
    ok(ct_collect("UID", "Type", "SAK"), "an nfc read is taken in");
    eq(g_ct_state, CT_OK, "and counts as a read");
    ok(!strcmp(g_ct_l1, "04 A2 B3 C4 D5 E6 F7"), "the UID row is the UID");
    ok(!strcmp(g_ct_l2, "NTAG / Ultralight"), "the Type row is the type");
    ok(!strcmp(g_ct_l3, "SAK 00"), "and the third row carries its own label");

    // A key.
    snprintf(g_ct_out, sizeof(g_ct_out), "%s",
             "[:] iButton\n"
             "  ROM       01 A2 B3 C4 D5 E6 F7 3D\n"
             "  Family    01  DS1990A / DS2401\n"
             "  CRC       ok\n");
    g_ct_rc = 0;
    g_ct_ready = 1;
    ok(ct_collect("ROM", "Family", "CRC"), "an ibutton read is taken in");
    ok(!strcmp(g_ct_l1, "01 A2 B3 C4 D5 E6 F7 3D"), "the ROM row is the ROM");
    ok(!strcmp(g_ct_l2, "01  DS1990A / DS2401"), "the family byte and its part");
    ok(!strcmp(g_ct_l3, "CRC ok"), "and the checksum is labelled");

    // THE ONE THAT MATTERS. A key whose checksum failed still prints a ROM row,
    // on purpose, so somebody at a terminal can see what came back. If the
    // screen looked for the field before the exit status it would find it and
    // show a rejected key as a good one — which is the whole failure the CRC
    // exists to prevent, reintroduced one layer up.
    // The tags are out.cpp's own: [:] info, [@] ok, [?] warn, [!] error. Worth
    // getting right even though nothing below keys off them — a fake carrying
    // the wrong prefix is a fake that would still pass while telling the next
    // person the wrong thing about the format.
    snprintf(g_ct_out, sizeof(g_ct_out), "%s",
             "[!] The key did not check out.\n"
             "  ROM       01 A2 B3 C4 D5 E6 F7 4C\n"
             "  CRC       bad - got 4C, wanted 3D\n");
    g_ct_rc = 1;
    g_ct_ready = 1;
    ct_collect("ROM", "Family", "CRC");
    eq(g_ct_state, CT_FAILED, "a rejected key is not reported as a read");
    ok(!strcmp(g_ct_why, "bad checksum - hold still"), "and it says why");
    ok(g_ct_l1[0] == 0, "with no ROM left on the screen");

    // Nothing presented is its own answer, not a fault.
    snprintf(g_ct_out, sizeof(g_ct_out), "%s",
             "[?] No tag.\n"
             "  Hold a card flat against the antenna while it reads.\n");
    g_ct_rc = 1;
    g_ct_ready = 1;
    ct_collect("UID", "Type", "SAK");
    eq(g_ct_state, CT_NOTHING, "an empty antenna is not a broken reader");

    snprintf(g_ct_out, sizeof(g_ct_out), "%s",
             "[!] No PN532 answered at 0x24 on I2C0.\n"
             "  Check the wiring and that the module's DIP switches select I2C.\n");
    g_ct_rc = 1;
    g_ct_ready = 1;
    ct_collect("UID", "Type", "SAK");
    eq(g_ct_state, CT_FAILED, "a missing reader is");
    ok(!strcmp(g_ct_why, "no reader answered"), "and is named as one");

    // An empty buffer is NOT an empty reader. There is one output capture in
    // the OS and a command whose output could not be captured comes back
    // untouched, which would otherwise read as "nothing was presented".
    g_ct_out[0] = 0;
    g_ct_rc = 0;
    g_ct_ready = 1;
    ct_collect("UID", "Type", "SAK");
    eq(g_ct_state, CT_BUSY_BUF, "an uncaptured command is not an empty antenna");

    ok(!ct_collect("UID", "Type", "SAK"), "and nothing pending collects nothing");
    g_ct_state = CT_NEVER;
}

#define STAGE(f) do { fprintf(stderr, "  .. %s\n", #f); f(); } while (0)

// --- the slider applies live -----------------------------------------------------
//
// novashots opens the catalogue and the slider sits behind a settings row, so it
// cannot photograph it. The part worth guarding anyway is that moving it does
// something NOW, not on the way out — brightness has to dim under the thumb — so
// this checks the live callback actually reaches the hardware (a contrast write
// is an I2C write, which the fake counts).
static void test_slider(void) {
    using namespace nova;
    gui::go_home();
    screens::open_display_settings();
    ui::Screen *d = gui::top();
    if (!d) { ok(false, "display settings opens"); return; }

    d->on_event(EV_SELECT);                 // row 0 is Brightness -> the slider
    ui::Screen *s = gui::top();
    ok(s && s != d && !strcmp(s->title(), "Brightness"),
       "brightness opens a slider titled after the row");
    if (!s || s == d) return;

    Canvas &c = gui::canvas();
    c.clear(0); s->draw(c);
    int lit = 0;
    for (int y = 0; y < c.height(); y++)
        for (int x = 0; x < c.width(); x++) if (c.get(x, y)) lit++;
    ok(lit > 12, "the slider draws a value and a bar");

    int w0 = g_i2c_writes;
    s->on_event(EV_ROT_CW);
    ok(g_i2c_writes > w0, "moving the brightness slider applies contrast live");
    gui::go_home();
}

// --- the notification toast picks up a post ------------------------------------
//
// The banner itself is drawn in run(), which the frame harness does not enter,
// so this covers the data path the runner reads: a post becomes one toast, taken
// once, and only while notifications are on.
static void test_toast(void) {
    using namespace nova;
    char buf[64];
    while (notify::take_toast(buf, sizeof(buf))) {}     // drain
    fw_reg_set("Apps.NovaD1_Notify", "on");
    notify::post("Wardrive: 42 APs");
    ok(notify::take_toast(buf, sizeof(buf)) && !strcmp(buf, "Wardrive: 42 APs"),
       "a posted notification is taken as a toast");
    ok(!notify::take_toast(buf, sizeof(buf)), "and only once");

    fw_reg_set("Apps.NovaD1_Notify", "off");
    notify::post("silent one");
    ok(!notify::take_toast(buf, sizeof(buf)), "no toast while notifications are off");
    fw_reg_set("Apps.NovaD1_Notify", "on");
}

// --- Updates: the version arithmetic ---------------------------------------------
//
// The same cases repoindex_test.cpp asserts against the firmware's
// repo_version_cmp, because upd_ver_cmp is a copy of it: repo_version_cmp is not
// on the package ABI, and two comparators that disagree would eventually offer
// an update that is older than what is installed.
static void test_update_versions(void) {
    using namespace nova::screens;
    ok(upd_ver_cmp("1.0.0", "1.0.0") == 0,  "version: equal");
    ok(upd_ver_cmp("1.2.0", "1.10.0") < 0,  "version: 1.10 is newer than 1.2");
    ok(upd_ver_cmp("2.0.0", "1.99.9") > 0,  "version: major wins");
    ok(upd_ver_cmp("2.1",   "2.1.0") == 0,  "version: missing component is zero");
    ok(upd_ver_cmp("3.0.0", "2.1.1") > 0,   "version: 3.0.0 beats 2.1.1");
    ok(upd_ver_cmp("0.9.1", "1.0.0") < 0,   "version: 0.9.1 is older than 1.0.0");
    // The pair a plain string comparison gets backwards, which is why the App
    // Store's " ^" mark deliberately does not claim to mean "newer".
    ok(upd_ver_cmp("1.10.0", "1.9.0") > 0,  "version: 1.10.0 beats 1.9.0");
    ok(strcmp("1.10.0", "1.9.0") < 0,       "...which strcmp gets backwards");
    ok(upd_ver_cmp("2.0.0.316", "2.0.0.315") > 0, "version: a build is a fourth component");

    eq(upd_state("1.3.3", "1.4.0"),  UPD_NEWER,   "a newer repo version is an update");
    eq(upd_state("1.3.3", "1.3.3"),  UPD_CURRENT, "the same version is not");
    eq(upd_state("1.9.0", "1.10.0"), UPD_NEWER,   "and 1.10 over 1.9 is one");
    eq(upd_state("1.4.0", "1.3.3"),  UPD_AHEAD,   "a hand-built newer one is not replaced");
    eq(upd_state("",      "1.4.0"),  UPD_UNKNOWN, "nothing installed is not an update");
    eq(upd_state("1.3.3", ""),       UPD_UNKNOWN, "and neither is an index that said nothing");
}

// --- Updates: reading `update check` ----------------------------------------------
//
// The OS version is frozen at v2.0.0 for the whole beta and what moves is the
// build number, so the command's VERDICT is read rather than recomputed.
static void test_update_os_reading(void) {
    using namespace nova::screens;
    static const char *kCurrent =
        "  Installed  v2.0.0 (build 315)\n"
        "  Available  2.0.0.315\n"
        "  Size       955 KB\n"
        "[@] Already up to date.\n";
    static const char *kNewer =
        "  Installed  v2.0.0 (build 315)\n"
        "  Available  2.0.0.316\n"
        "  Size       955 KB\n"
        "[:] An update is available. 'update install' to fetch and apply it.\n";
    static const char *kRefused =
        "  Installed  v2.0.0 (build 315)\n"
        "  Available  2.0.0.316\n"
        "[?] This device already rolled back from 2.0.0.316.\n"
        "  It was installed and would not start, so the previous version\n"
        "  was put back. 'update install --force' installs it anyway.\n";
    static const char *kNoNet = "[!] No network. Connect first.\n";

    eq(upd_os_state(kCurrent), UPD_CURRENT, "the OS reports itself up to date");
    eq(upd_os_state(kNewer),   UPD_NEWER,   "and reports an OS update when there is one");
    eq(upd_os_state(kRefused), UPD_REFUSED, "a build this device rolled back from is not offered");
    eq(upd_os_state(kNoNet),   UPD_UNKNOWN, "no network is not 'up to date'");
    eq(upd_os_state(""),       UPD_UNKNOWN, "and neither is an uncaptured command");

    char row[48], v[20];
    ok(upd_os_row(kNewer, "Installed", row, sizeof(row)), "the Installed row is found");
    upd_os_compact(row, v, sizeof(v));
    ok(!strcmp(v, "2.0.0.315"), "and reads as the build the firmware actually compares");
    ok(upd_os_row(kNewer, "Available", row, sizeof(row)), "the Available row is found");
    upd_os_compact(row, v, sizeof(v));
    ok(!strcmp(v, "2.0.0.316"), "and needs no rewriting");
    ok(!upd_os_row(kNoNet, "Installed", row, sizeof(row)),
       "a check that failed printed no rows to read");

    // The trap the compacting exists for: the two rows AS PRINTED disagree,
    // because the leading 'v' reads as a zeroth component and loses every time.
    eq(upd_state("v2.0.0 (build 315)", "2.0.0.315"), UPD_NEWER,
       "comparing the printed rows would claim an update forever");
    eq(upd_state("2.0.0.315", "2.0.0.315"), UPD_CURRENT,
       "and the compacted ones agree with the OS");
}

// --- Updates: what stops the flow ----------------------------------------------------
//
// A maintenance boot runs its staged command AFTER session_boot(), so a device
// that asks for a login stops at the prompt with the install never run — and
// with the panel frozen on whatever was last drawn. This is the gate.
static void test_update_refusals(void) {
    using namespace nova::screens;
    static const char *kRoot  = "root  (admin)\n";
    static const char *kGuest = "guest\n";
    static const char *kOn    = "[@] [autonomy] On, as 'root'.\n";

    ok(update_autonomy_ok(kOn, kRoot),
       "a device that boots without a prompt, as this admin, can stage an update");
    ok(!update_autonomy_ok("[:] [autonomy] Off - this device asks for a login.\n", kRoot),
       "one that asks for a login cannot");
    ok(!update_autonomy_ok("[?] [autonomy] Set to 'dash', which no longer exists. "
                           "It will ask instead.\n", kRoot),
       "nor one whose autonomous account has been removed");
    ok(!update_autonomy_ok("", kRoot), "and an uncaptured answer is not a yes");

    // The half that is easy to miss: autonomy is on, staging would work, and
    // the maintenance boot comes up as somebody who can run neither line of the
    // script — not even the reboot that gets the device back out.
    ok(!update_autonomy_ok("[@] [autonomy] On, as 'guest'.\n", kRoot),
       "an admin staging for a different autonomous user is refused");
    ok(!update_autonomy_ok("[@] [autonomy] On, as 'guest'.\n", kGuest),
       "and a non-admin autonomous user is refused even when it is the same person");
    ok(!update_autonomy_ok(kOn, ""), "an uncaptured whoami is not a yes either");
}

// --- Updates: the script the maintenance boot runs -----------------------------------
static void test_update_script(void) {
    using namespace nova::screens;
    g_write_path[0] = g_write_body[0] = 0;
    ok(update_write_script(), "the script is written");
    ok(!strcmp(g_write_path, "/nova/update.rps"),
       "under /nova, which boot does not sweep the way it sweeps /tmp");
    ok(strstr(g_write_body, "\npkg install novad1\n") != nullptr,
       "the install is a line of its own");
    ok(strstr(g_write_body, "\nreboot\n") != nullptr,
       "and so is the reboot");
    // safeboot's staged command goes through shell_run_line_now, which is
    // run_segment: pipes and redirection, but no connectors. `pkg install
    // novad1 && reboot` would ask pkg for three packages, two of them named
    // "&&" and "reboot".
    ok(strstr(g_write_body, "&&") == nullptr,
       "never as a chain, which the staged command cannot split");
    // rps discards a command's status unless it is a condition, so the reboot
    // happens either way and a failed install cannot strand the device.
    //
    // The LINES, not the words. An earlier version of this matched "reboot"
    // anywhere and passed until the script's own comment said the word.
    ok(strstr(g_write_body, "\npkg install novad1\n") <
       strstr(g_write_body, "\nreboot\n"),
       "install first, reboot after, unconditionally");
    ok(!strcmp(update_stage_line(), "safeboot script /nova/update.rps"),
       "and safeboot is what stages it");

    g_write_fails = 1;
    ok(!update_write_script(), "a filesystem that will not take it says so");
}

// Dispatch one gesture and honour what came back, the way the runner does.
static void upd_send(nova::ui::Screen *s, nova::Event e) {
    if (!s) return;
    nova::ui::Action a = s->on_event(e);
    if (a == nova::ui::ACT_STAY && e == nova::EV_HOME && !s->modal())
        a = nova::ui::ACT_HOME;
    if      (a == nova::ui::ACT_BACK) nova::gui::pop();
    else if (a == nova::ui::ACT_HOME) nova::gui::go_home();
}

// --- Updates: the screen, as far as the host can follow it ---------------------------
//
// The fake runs a spawned job inline, so two ticks carry one step of the check
// chain and a couple of dozen carry the whole of it.
//
// DEVICE-UNCONFIRMED past the staging. On real hardware safeboot never returns —
// it restarts from inside the call — so everything after "the restart was asked
// for" happens on the next boot and cannot be reached from here.
static void test_update_screen(void) {
    using namespace nova;
    g_run_spawned = 1;
    fw_reg_set("Apps.NovaD1_UpdTo", "");
    fw_reg_set("Apps.NovaD1_UpdFrom", "");

    // --- nothing to do. The fake's `pkg list` and `pkg search` both say 1.1.0.
    gui::go_home();
    screens::open_updates();
    ui::Screen *s = gui::top();
    if (!s) { ok(false, "Updates opens"); g_run_spawned = 0; return; }
    ok(!strcmp(s->title(), "Updates"), "Updates titles itself the way its row is labelled");

    for (int i = 0; i < 24; i++) s->tick(33);
    ok(!s->animating(), "the check finishes rather than spinning forever");

    Canvas &c = gui::canvas();
    c.clear(0); s->draw(c);
    int lit = 0;
    for (int y = 0; y < c.height(); y++)
        for (int x = 0; x < c.width(); x++) if (c.get(x, y)) lit++;
    ok(lit > 12, "and draws a verdict rather than an empty panel");

    upd_send(s, EV_SELECT);
    ok(gui::top() == s, "an up-to-date device is offered nothing to press");

    // --- the repo moves ahead.
    shell_says("pkg search",
               "  bench      1.0   RPCMark, the same workload as the MicroPython build\n"
               "  novad1     1.4.0 Nova D1 - the handheld multi-tool\n");
    upd_send(s, EV_SELECT_HOLD);                    // look again
    for (int i = 0; i < 24; i++) s->tick(33);

    upd_send(s, EV_SELECT);
    ui::Screen *q = gui::top();
    ok(q && q != s && !strcmp(q->title(), "Confirm"),
       "a newer version in the repo asks before it takes it");
    if (!q || q == s) { shell_says(nullptr, nullptr); g_run_spawned = 0; return; }

    // --- No, and nothing is staged.
    const int writes = g_writes;
    upd_send(q, EV_BACK);
    ok(gui::top() == s, "declining comes back to the screen");
    eq(g_writes, writes, "and writes no script");

    // --- Yes.
    upd_send(s, EV_SELECT);
    q = gui::top();
    upd_send(q, EV_ROT_CW);                         // No -> Update
    upd_send(q, EV_SELECT);
    ok(gui::top() == s, "agreeing comes back to the screen");

    for (int i = 0; i < 6; i++) s->tick(33);        // autonomy status, then the goodbye
    ok(g_writes > writes, "agreeing writes the script");
    ok(s->modal(), "and the goodbye holds the panel rather than letting HOME out");
    char to[24] = "";
    fw_reg_get("Apps.NovaD1_UpdTo", to, sizeof(to));
    ok(!strcmp(to, "1.4.0"), "remembering the version it is going to");

    c.clear(0); s->draw(c);
    lit = 0;
    for (int y = 0; y < c.height(); y++)
        for (int x = 0; x < c.width(); x++) if (c.get(x, y)) lit++;
    ok(lit > 12, "the goodbye says what is about to happen before the restart");

    // --- the restart is asked for only after the goodbye has been readable.
    for (int i = 0; i < 40; i++) s->tick(33);
    ok(!strcmp(g_last_shell, "safeboot script /nova/update.rps"),
       "then safeboot stages it");

    // Being here means safeboot did not restart, which on a device is a refusal
    // — nearly always an account that is not an admin. The device has to be left
    // usable and honest, not sitting on a goodbye for a reboot that never comes.
    ok(!s->modal(), "a refused restart releases the panel");
    fw_reg_get("Apps.NovaD1_UpdTo", to, sizeof(to));
    ok(to[0] == 0, "and forgets a version it never went to");

    // --- a device that asks for a login is told why, and nothing is staged.
    //
    // There is one override slot, so the repo goes ahead first and the check
    // runs, and only then is the autonomy answer swapped in — which is also the
    // order it happens in, since autonomy is asked at the moment somebody says
    // yes rather than up front.
    shell_says("pkg search", "  novad1     1.4.0 Nova D1 - the handheld multi-tool\n");
    upd_send(s, EV_SELECT_HOLD);
    for (int i = 0; i < 24; i++) s->tick(33);
    shell_says("autonomy status", "[:] [autonomy] Off - this device asks for a login.\n");

    upd_send(s, EV_SELECT);
    q = gui::top();
    ok(q && q != s, "the update is still offered");
    if (q && q != s) { upd_send(q, EV_ROT_CW); upd_send(q, EV_SELECT); }
    const int writes2 = g_writes;
    for (int i = 0; i < 8; i++) s->tick(33);
    eq(g_writes, writes2, "a device that asks for a login stages nothing");
    ok(!s->modal(), "and is not left waiting for a restart that cannot work");
    char to2[24] = "x";
    fw_reg_get("Apps.NovaD1_UpdTo", to2, sizeof(to2));
    ok(to2[0] == 0, "nor remembering a version it is not going to");

    shell_says(nullptr, nullptr);
    g_run_spawned = 0;
    gui::go_home();
}

// --- Updates: what the next start says -----------------------------------------------
//
// update_report_boot() in the firmware does this for a FIRMWARE update, keyed on
// System.Update_To. There is no equivalent for a package, so this is the
// package's own — and the direction that matters is the quiet one: an update
// that did not take must not pass without a word, or the reboot reads as
// something that happened for no reason.
static void test_update_report(void) {
    using namespace nova;
    char buf[64];
    while (notify::take_toast(buf, sizeof(buf))) {}
    fw_reg_set("Apps.NovaD1_Notify", "on");

    fw_reg_set("Apps.NovaD1_UpdTo", "");
    const int before = notify::count();
    screens::update_report_start();
    eq(notify::count(), before, "an ordinary start says nothing and asks nothing");

    // It landed: the fake's `pkg list` reports novad1 at 1.1.0.
    fw_reg_set("Apps.NovaD1_UpdFrom", "1.0.0");
    fw_reg_set("Apps.NovaD1_UpdTo",   "1.1.0");
    screens::update_report_start();
    ok(notify::take_toast(buf, sizeof(buf)) && strstr(buf, "1.1.0"),
       "an update that landed is reported");
    char to[24] = "x";
    fw_reg_get("Apps.NovaD1_UpdTo", to, sizeof(to));
    ok(to[0] == 0, "and is cleared, so it is reported once");
    ok(!strcmp(g_removed, "/nova/update.rps"), "the spent script is taken away with it");

    // It did not: the install was refused, the script rebooted anyway, and the
    // device is back on the version it started on.
    fw_reg_set("Apps.NovaD1_UpdFrom", "1.1.0");
    fw_reg_set("Apps.NovaD1_UpdTo",   "1.4.0");
    screens::update_report_start();
    ok(notify::take_toast(buf, sizeof(buf)) && strstr(buf, "did not take"),
       "and one that did not is said out loud rather than passed over");
}

// --- LAN: the device sweep -------------------------------------------------------
//
// Everything the host CAN answer about it, which is more than it looks: the
// sweep is arithmetic over addresses and a set of replies, and the fake supplies
// both. What it cannot answer is whether fw_net_ping yields — see the
// DEVICE-UNCONFIRMED note beside lan_sweep.
//
// g_run_spawned makes fw_task_spawn run the worker inline, so a whole sweep
// completes inside one call. That loses the progressive fill (which is the
// runner's business, not the sweep's) and buys every other question.
static void open_lan_screen(void) {
    using namespace nova;
    gui::go_home();
    for (unsigned i = 0; i < gui::app_total(); i++) {
        const gui::App *a = gui::app_at(i);
        if (strcmp(a->key, "lan")) continue;
        gui::chose(a);
        a->open();
        return;
    }
    ok(false, "the catalogue has a LAN row");
}

static void test_lan_offline(void) {
    using namespace nova;
    g_net_up = 0;
    ping_clear();

    open_lan_screen();
    ui::Screen *s = gui::top();
    ok(s != nullptr, "the LAN screen opens");
    streq_(s ? s->title() : "", "LAN", "and titles itself LAN");

    // The one row offline is a way OUT, not an apology. Pressing it has to reach
    // the screen that fixes the problem.
    const unsigned before = gui::depth();
    input().inject(EV_SELECT);
    frame();
    ok(gui::depth() == before + 1, "SELECT offline opens something");
    streq_(gui::top() ? gui::top()->title() : "", "WiFi",
           "and what it opens is the WiFi screen");
    eq(g_ping_calls, 0, "nothing was pinged with no network");
    gui::go_home();
}

static void test_lan_sweep(void) {
    using namespace nova;
    g_net_up = 1;
    g_net_ip = "192.168.1.42";
    ping_clear();
    ping_add(1,  2400);          // the gateway, 2.4 ms
    ping_add(57, 9100);
    ping_add(200,   400);        // under a millisecond: must not read as zero
    // The gateway tag comes from `net`, and only from `net`.
    shell_says("net", "  Address   192.168.1.42\n  Gateway   192.168.1.1\n");

    open_lan_screen();
    g_run_spawned = 1;
    input().inject(EV_SELECT);   // row 0 is Scan
    frame();
    g_run_spawned = 0;

    eq(g_ping_calls, 253, "253 addresses are probed — 254 less our own");
    eq(screens::g_lan_n, 4, "our own address and the three that answered are listed");

    char label[24], value[12];
    screens::lan_row(1, label, sizeof(label), value, sizeof(value));
    streq_(label, "192.168.1.1", "the first row is the lowest address that answered");
    streq_(value, "gw 2ms", "and it is tagged as the gateway, with its time");

    screens::lan_row(2, label, sizeof(label), value, sizeof(value));
    streq_(label, "192.168.1.42", "our own address keeps its place in the range");
    streq_(value, "me", "and says so rather than showing a time it never measured");

    screens::lan_row(3, label, sizeof(label), value, sizeof(value));
    streq_(value, "9ms", "a reading is milliseconds");

    screens::lan_row(4, label, sizeof(label), value, sizeof(value));
    streq_(value, "1ms", "and a sub-millisecond reply rounds UP, never to 0ms");

    screens::lan_row(0, label, sizeof(label), value, sizeof(value));
    streq_(label, "Scan again", "the control row offers a second sweep");
    streq_(value, "4", "and says how many it found");

    shell_says(nullptr, nullptr);
}

// A sweep is nearly a minute long, so every way out of it has to work.
static void test_lan_interrupts(void) {
    using namespace nova;
    g_net_up = 1;
    g_net_ip = "192.168.1.42";

    // 1. The stop flag. Set before the worker runs, the loop must come straight
    //    back out rather than probing anything.
    ping_clear();
    screens::g_lan_stop = true;
    screens::lan_sweep();
    eq(g_ping_calls, 0, "a sweep asked to stop before it starts probes nothing");
    streq_(screens::g_lan_msg, "Stopped", "and says why the list is short");

    // 2. The task being killed under it, which sets no flag of ours.
    ping_clear();
    screens::g_lan_stop = false;
    g_should_stop = 1;
    screens::lan_sweep();
    g_should_stop = 0;
    eq(g_ping_calls, 0, "a worker asked to end stops sweeping");

    // 3. Leaving the screen ends it. The survey next door deliberately runs on;
    //    this one is a question, and the answer stops mattering when you walk
    //    away from it.
    // 3. Navigation does NOT stop it, and that is the deliberate half.
    //    gui::push_slot() calls leave() on the screen it is about to COVER, not
    //    only on the one being popped — so a "stop on leave" rule would have had
    //    every notice this screen raises quietly cancel a minute of work,
    //    including the one on our own address, which is a press that looks like
    //    a press and kills the scan.
    ping_clear();
    ping_add(1, 2000);
    open_lan_screen();
    g_run_spawned = 1;
    input().inject(EV_SELECT); frame();          // a full sweep, run inline
    g_run_spawned = 0;
    eq(screens::g_lan_n, 2, "our own address and the one that answered");

    input().inject(EV_ROT_CW); frame();          // onto 192.168.1.1
    input().inject(EV_ROT_CW); frame();          // onto our own address

    // A row pressed DURING a sweep is refused — one worker, one radio — and it
    // says so instead of doing nothing.
    screens::g_lan_stop = false;
    screens::g_lan_run  = true;                  // as if the worker were still out
    const unsigned deep = gui::depth();
    input().inject(EV_SELECT); frame();
    eq((int)gui::depth(), (int)deep, "a row pressed mid-sweep opens nothing");
    ok(screens::g_lan_msg[0] != 0, "and says why");
    ok(!screens::g_lan_stop, "and does not cancel the sweep");

    // And with the sweep finished, our own row explains itself — through a
    // notice, which is a PUSH, which calls leave() on this screen. That is the
    // trap: a "stop the sweep on leave" rule would fire there.
    screens::g_lan_run = true;                   // still nominally sweeping
    screens::g_lan_stop = false;
    ui::notice("LAN", "anything");               // the same push the row makes
    ok(!screens::g_lan_stop, "a notice over the screen does NOT stop the sweep");
    screens::g_lan_run = false;
    gui::go_home();
}

// One radio, one user of it. A wifi scan started during a sweep would break the
// association the sweep is talking over.
static void test_lan_holds_the_radio(void) {
    using namespace nova;
    ok(!screens::op_busy(), "nothing is busy to begin with");
    screens::g_lan_run = true;
    ok(screens::op_busy(), "a sweep counts as the radio being busy");
    ok(!screens::op_start(OP_SCAN), "so a scan started during one is refused");
    screens::g_lan_run = false;
    ok(!screens::op_busy(), "and the radio is free again when it ends");
}

// The range comes from our own address and nothing else, so the two ways of not
// having one have to be told apart.
static void test_lan_derive(void) {
    using namespace nova;
    g_net_up = 0;
    ok(!screens::lan_derive(), "no network means no range");

    g_net_up = 1;
    g_net_ip = "0.0.0.0";
    ok(!screens::lan_derive(), "joined with no address yet is not a range either");

    g_net_ip = "10.0.0.7";
    ok(screens::lan_derive(), "a real address gives one");
    streq_(screens::g_lan_base, "10.0.0.", "the base keeps its trailing dot");
    eq(screens::g_lan_self, 7, "and our own last octet is remembered");
    g_net_ip = "192.168.1.42";
}

// Re-probing one row must leave the cursor on that row: it is the row whose
// answer was just asked for. enter() runs again when the notice it may raise
// pops, so this is the same class of bug SettingsList has a comment about.
static void test_lan_reprobe_keeps_the_cursor(void) {
    using namespace nova;
    g_net_up = 1;
    g_net_ip = "192.168.1.42";
    ping_clear();
    ping_add(1, 2000);
    ping_add(57, 9000);

    open_lan_screen();
    g_run_spawned = 1;
    input().inject(EV_SELECT);
    frame();
    eq(screens::g_lan_n, 3, "three rows to move between");

    // Row 3 is the last host, 192.168.1.57.
    input().inject(EV_ROT_CW); frame();
    input().inject(EV_ROT_CW); frame();
    input().inject(EV_ROT_CW); frame();
    ping_clear();
    input().inject(EV_SELECT); frame();
    streq_(g_ping_last, "192.168.1.57", "SELECT on a host re-pings that host");

    // And again, with an enter() in between — which is what a popped notice does.
    ui::Screen *s = gui::top();
    if (s) s->enter();
    ping_clear();
    input().inject(EV_SELECT); frame();
    streq_(g_ping_last, "192.168.1.57", "and the cursor is still on it afterwards");

    g_run_spawned = 0;
    gui::go_home();
    g_net_up = 0;
}

// --- the lock ---------------------------------------------------------------------
//
// It had four settings rows and no implementation: Lock_Kind, PIN, Pass and
// LockSec were written by the Security screen and read by nothing. So these
// checks are as much about the ARMING rule as about the screen — a lock that
// engages with no code stored is a panel with no way back into it, and that is
// the failure a host can and must catch.
static void set_pin(const char *pin) {
    fw_reg_set("Apps.NovaD1_Lock_Kind", pin && pin[0] ? "pin" : "none");
    fw_reg_set("Apps.NovaD1_PIN", pin ? pin : "");
    fw_reg_set("Apps.NovaD1_Pass", "");
}

static void clear_lock(void) {
    set_pin("");
    fw_reg_set("Apps.NovaD1_LockSec", "0");
}

// Turn the pad to `pin` and submit it, the way a person does: SELECT walks the
// digits, the encoder sets each one, HOME enters it.
static void type_pin(const char *pin) {
    using namespace nova;
    for (int i = 0; i < 6; i++) {
        for (int d = 0; d < pin[i] - '0'; d++) { input().inject(EV_ROT_CW); frame(); }
        input().inject(EV_SELECT); frame();
    }
    input().inject(EV_HOME); frame();
}

static void test_lock_needs_a_code(void) {
    using namespace nova;
    gui::go_home();
    clear_lock();

    // A preference with no code behind it. Lock_Kind alone used to be what the
    // settings row believed, and arming on it would lock a device that has
    // nothing to unlock with.
    fw_reg_set("Apps.NovaD1_Lock_Kind", "pin");
    fw_reg_set("Apps.NovaD1_PIN", "");
    streq_(screens::lock_state(), "None", "a type with no code set is not a lock");
    ok(!screens::lock_armed(), "and does not arm");
    screens::lock_engage();
    ok(!screens::lock_active(), "engaging it does nothing at all");
    eq((int)gui::depth(), 1, "and nothing is pushed");

    fw_reg_set("Apps.NovaD1_Lock_Kind", "password");
    fw_reg_set("Apps.NovaD1_Pass", "");
    streq_(screens::lock_state(), "None", "the same for a password with none stored");

    // A code cleared while the type still says password is the case pass_typed
    // exists for; the state has to follow the code, not the preference.
    set_pin("246810");
    streq_(screens::lock_state(), "PIN", "a stored code is what makes it a lock");
    ok(screens::lock_armed(), "and it arms");
    clear_lock();
}

static void test_lock_screen(void) {
    using namespace nova;
    gui::go_home();
    set_pin("246810");

    screens::lock_engage();
    ok(screens::lock_active(), "the lock goes up");
    ui::Screen *s = gui::top();
    ok(s && s->fullscreen(), "it takes the whole panel, status bar included");
    ok(s && s->modal(), "and HOME does not escape it");

    // It is fullscreen, so novashots' two rules do not apply and novashots
    // cannot reach it anyway — nothing in the catalogue opens it. What still
    // has to hold is the one that matters on a device: a blank panel and a hung
    // device look identical, and a lock is the worst place to find that out.
    frame();
    int lit = 0;
    for (int y = 0; y < gui::canvas().height(); y++)
        for (int x = 0; x < gui::canvas().width(); x++) if (gui::canvas().get(x, y)) lit++;
    ok(lit > 40, "and it draws a panel rather than a blank one");

    // The two gestures that get out of every other screen must not get out of
    // this one. BACK first, because Screen::on_event's default would pop it.
    const unsigned d = gui::depth();
    input().inject(EV_BACK); frame();
    eq((int)gui::depth(), (int)d, "BACK does not pop the lock");
    ok(screens::lock_active(), "and it is still locked");
    input().inject(EV_HOME); frame();
    eq((int)gui::depth(), (int)d, "HOME does not drop through it");
    ok(screens::lock_active(), "still locked");

    // Holding HOME opens the power menu from every other screen on the device.
    // Over the lock it must not, or Reboot and Screen off are two gestures away
    // from anybody who picks it up. Asked of the runner's own rule rather than
    // of the harness's copy of the loop, which does not have this branch in it.
    ok(!gui::power_gesture_ok(), "holding HOME opens no power menu over the lock");

    // A wrong code says so and stays put.
    input().inject(EV_SELECT); frame();
    ok(gui::depth() == d + 1, "SELECT opens the pad");
    type_pin("111111");
    frame();
    ok(screens::lock_active(), "a wrong code leaves it locked");
    ok(strstr(screens::g_lock_note, "Wrong") != nullptr, "and says so on the panel");

    // And the right one gets out.
    input().inject(EV_SELECT); frame();
    type_pin("246810");
    frame();
    ok(!screens::lock_active(), "the right code unlocks it");
    eq((int)gui::depth(), 1, "and leaves nothing of itself behind");
    ok(gui::power_gesture_ok(), "and holding HOME reaches the power menu again");

    clear_lock();
}

// A PASSWORD lock, which is a different screen from the PIN pad and was the one
// with the hole in it.
//
// ui::keyboard's screen returns ACT_HOME from EV_HOME — not ACT_STAY, so the
// runner's "modal opts out of the HOME fallback" rule never came into it — and
// go_home() popped the lock along with the keyboard while g_locked stayed true.
// That is not just an unlock: lock_engage() and power_gesture_ok() both read
// g_locked, so the device would have been permanently unlocked AND permanently
// without a power menu, until a reboot.
static void test_password_lock_is_not_escapable(void) {
    using namespace nova;
    gui::go_home();
    fw_reg_set("Apps.NovaD1_Lock_Kind", "password");
    fw_reg_set("Apps.NovaD1_PIN", "");
    fw_reg_set("Apps.NovaD1_Pass", "hunter2");
    streq_(screens::lock_state(), "Password", "a stored password is a lock");

    screens::lock_engage();
    ok(screens::lock_active(), "it goes up");
    const unsigned at = gui::depth();

    input().inject(EV_SELECT); frame();
    streq_(gui::top() ? gui::top()->title() : "", "Password",
           "SELECT opens the keyboard, not the PIN pad");

    input().inject(EV_HOME); frame();
    ok(screens::lock_active(), "HOME on the keyboard does not unlock the device");
    eq((int)gui::depth(), (int)at, "and lands back on the lock, not on home");

    // The floor moves with it: once unlocked, home means home again.
    screens::g_lock_pass = true;
    frame();
    ok(!screens::lock_active(), "the lock comes down when it is answered");
    eq((int)screens::lock_floor(), 1, "and stops being the floor");
    gui::go_home();
    eq((int)gui::depth(), 1, "so HOME reaches home");

    clear_lock();
    fw_reg_set("Apps.NovaD1_Pass", "");
}

// The runner arms it off the idle clock, the same one that dims and blanks the
// panel. Driven through gui::lock_due so the rule is the runner's own and not a
// second copy written here.
static void test_lock_idle(void) {
    using namespace nova;
    gui::go_home();
    set_pin("246810");
    fw_reg_set("Apps.NovaD1_LockSec", "30");

    ok(!gui::lock_due(29), "29 seconds idle with a 30 second timeout is not due");
    ok(gui::lock_due(30),  "30 is");
    ok(gui::lock_due(600), "and so is anything past it");

    fw_reg_set("Apps.NovaD1_LockSec", "0");
    ok(!gui::lock_due(100000), "zero is never, not immediately");

    // Never with no code, whatever the timeout says.
    fw_reg_set("Apps.NovaD1_LockSec", "30");
    set_pin("");
    ok(!gui::lock_due(600), "and a timeout with no code set is still never");

    clear_lock();
}

// A staged update owns the panel and must not be interrupted by the lock: it is
// modal for exactly that reason, and the arming rule honours it.
static void test_lock_yields_to_a_modal(void) {
    using namespace nova;
    gui::go_home();
    set_pin("246810");

    // Any modal will do; the update screen's own is scripted elsewhere, so this
    // uses the one every screen can raise.
    ui::confirm("Something that has to be answered?", "Yes", nullptr, nullptr);
    ui::Screen *s = gui::top();
    ok(s && s->modal(), "a confirmation is modal");
    screens::lock_engage();
    ok(!screens::lock_active(), "the lock does not go up over one");

    gui::go_home();
    screens::lock_engage();
    ok(screens::lock_active(), "and does once it has been answered");

    // Twice must not stack. Two lock screens is two things to unlock.
    const unsigned d = gui::depth();
    screens::lock_engage();
    eq((int)gui::depth(), (int)d, "engaging it again pushes nothing");

    screens::g_lock_pass = true;
    frame();
    ok(!screens::lock_active(), "unlocked again for whatever runs next");
    clear_lock();
}

// --- naming the device from the panel ------------------------------------------------
//
// System.Device_ID and System.Owner are the OS's keys — the shell prompt, fetch
// and sysinfo all read them — and until now the only way to set either was a
// serial cable, on a device whose whole point is not needing one.
static void open_by_key(const char *key) {
    using namespace nova;
    gui::go_home();
    for (unsigned i = 0; i < gui::app_total(); i++) {
        const gui::App *a = gui::app_at(i);
        if (strcmp(a->key, key)) continue;
        gui::chose(a);
        a->open();
        return;
    }
    ok(false, key);
}

static void test_device_naming(void) {
    using namespace nova;
    char buf[40];

    fw_reg_set("System.Device_ID", "vela");
    // An empty name is a mistake, not an edit: the shell prompt is built from
    // this and a device named "" reads as broken rather than as renamed.
    screens::devid_typed(nullptr, "");
    fw_reg_get("System.Device_ID", buf, sizeof(buf));
    streq_(buf, "vela", "an empty device ID is refused, not stored");

    screens::devid_typed(nullptr, "workbench");
    fw_reg_get("System.Device_ID", buf, sizeof(buf));
    streq_(buf, "workbench", "and a real one is kept");

    // An owner CAN be emptied — "nobody has claimed this" is a real answer, and
    // that is the deliberate difference from the name above.
    screens::owner_typed(nullptr, "dash");
    fw_reg_get("System.Owner", buf, sizeof(buf));
    streq_(buf, "dash", "an owner is stored");
    screens::owner_typed(nullptr, "");
    fw_reg_get("System.Owner", buf, sizeof(buf));
    ok(buf[0] == 0, "and can be cleared again");

    // And the rows reach the right editors, which is the half a callback test
    // cannot cover: a row wired to the wrong index edits the wrong key.
    open_by_key("set_system");
    input().inject(EV_ROT_CW); frame();
    input().inject(EV_ROT_CW); frame();
    input().inject(EV_SELECT); frame();
    streq_(gui::top() ? gui::top()->title() : "", "Device ID",
           "the third row opens the device ID editor");
    gui::pop();
    input().inject(EV_ROT_CW); frame();
    input().inject(EV_SELECT); frame();
    streq_(gui::top() ? gui::top()->title() : "", "Owner",
           "and the fourth opens the owner editor");
    gui::go_home();
    fw_reg_set("System.Device_ID", "vela");
}

// --- what a pool slot arrives holding ----------------------------------------------
//
// Several comments in this package say a re-used slot holds the last screen's
// bytes. It does not: gui::push<T>() is `new (slot) T()`, and value-initialising
// a class with an implicit default constructor zeroes it first. That matters
// because fourteen screens stopped resetting their cursor in enter(), and what
// they lean on instead is the slot being zero on the FIRST push and the clamp
// doing the rest.
//
// It is checked rather than reasoned about because the reasoning is a language
// rule three steps deep, and getting it wrong looks like a cursor that lands on
// a random row once in a while.
struct SlotProbe : nova::ui::Screen {
    int      a;
    unsigned b;
    void    *c;
    const char *title(void) const override { return "probe"; }
};

static void test_pool_slots_arrive_zeroed(void) {
    using namespace nova;
    gui::go_home();

    SlotProbe *p = gui::push<SlotProbe>();
    ok(p != nullptr, "a screen can be pushed");
    // Dirty every byte a screen of this shape could see.
    p->a = -12345;
    p->b = 0xdeadbeefu;
    p->c = (void *)p;
    gui::pop();

    SlotProbe *q = gui::push<SlotProbe>();
    ok(q == p, "the next push lands in the same slot");
    eq(q->a, 0, "and its members arrive zeroed, not holding the last screen's");
    ok(q->b == 0 && q->c == nullptr, "all of them");
    gui::pop();
}

// --- the '?' in the status bar had nothing behind it -------------------------------
//
// Every Screen has a help(), the status bar draws a '?' for any screen that
// returns lines, and novaui.h described holding HOME as the way to see them.
// Holding HOME opens the power menu and always has, so the marker pointed at a
// screen that did not exist — on nearly every screen in the suite.
static void test_help_is_reachable(void) {
    using namespace nova;
    gui::go_home();

    // A screen with help on it, and not the home screen, so the check is about
    // the subject rather than about whatever happens to be at the bottom.
    open_by_key("diag");
    ui::Screen *subject = gui::top();
    const char *want[8];
    const int n = subject->help(want, 8);
    ok(n > 0, "the Hardware screen has help to show");

    screens::open_power();
    streq_(gui::top() ? gui::top()->title() : "", "Power", "holding HOME opens Power");
    input().inject(EV_SELECT); frame();          // row 0 is Controls
    streq_(gui::top() ? gui::top()->title() : "", "Controls",
           "and its first row opens the controls for the screen underneath");

    // What it draws has to be the SUBJECT's help, not the power menu's own —
    // capturing the wrong screen is the way this goes quietly wrong.
    gui::canvas().clear(0);
    gui::top()->draw(gui::canvas());
    int lit = 0;
    for (int y = 0; y < gui::canvas().height(); y++)
        for (int x = 0; x < gui::canvas().width(); x++) if (gui::canvas().get(x, y)) lit++;
    ok(lit > 40, "it draws something");
    ok(screens::g_help_of == subject, "and it is about the screen Power was opened over");

    gui::go_home();
}

// --- the sweep: a list must remember where you were ------------------------------
//
// enter() runs on the first push AND every time a child screen pops, so a screen
// that sets its cursor there sends you back to row zero after every look at a
// row. It was true of fourteen screens. Two of them are checked here, one of
// each kind — a ui::Menu that rebuilds its rows on the way in, and a hand-rolled
// list that pushes a detail screen.
static void test_menu_keeps_its_place(void) {
    using namespace nova;
    open_by_key("cmds");
    ui::Menu *m = (ui::Menu *)gui::top();
    ok(m != nullptr, "the Commands list opens");
    streq_(gui::top()->title(), "Commands", "and is the menu");

    for (int i = 0; i < 3; i++) { input().inject(EV_ROT_CW); frame(); }
    eq(m->selected(), 3, "the cursor moves to the fourth row");

    // Every row here opens an output pane you then back out of.
    input().inject(EV_SELECT); frame();
    ok(gui::depth() > 2, "SELECT opens the output");
    input().inject(EV_BACK); frame();
    m = (ui::Menu *)gui::top();
    streq_(gui::top()->title(), "Commands", "BACK returns to the list");
    eq(m->selected(), 3, "and the cursor is where it was, not back at the top");
    gui::go_home();
}

static void test_list_keeps_its_place(void) {
    using namespace nova;
    open_by_key("diag");
    streq_(gui::top()->title(), "Hardware", "the module list opens");

    // Its cursor is private, so the question is asked the way a person would:
    // the detail screen titles itself with the module that was chosen.
    for (int i = 0; i < 3; i++) { input().inject(EV_ROT_CW); frame(); }
    input().inject(EV_SELECT); frame();
    char first[24];
    nova::copy(first, sizeof(first), gui::top()->title());
    ok(strcmp(first, "Hardware") != 0, "SELECT opens a module");

    input().inject(EV_BACK); frame();
    input().inject(EV_SELECT); frame();
    streq_(gui::top()->title(), first,
           "and coming back leaves the cursor on the same module");
    gui::go_home();
}

// --- the sweep: a request that was refused has to be re-asked ----------------------
//
// rf_run refuses while a previous screen's worker is winding down, and the
// status request was dropped when it did — leaving "CC1101: checking..." on the
// panel for ever, with no spinner, on a device whose radio was fine.
static void test_radio_status_is_re_asked(void) {
    using namespace nova;
    gui::go_home();

    screens::g_rf_busy = 1;              // as if a previous screen's worker is out
    open_by_key("cc1101");
    streq_(gui::top()->title(), "Sub-GHz", "the Sub-GHz screen opens");
    g_last_shell[0] = 0;

    ui::Screen *s = gui::top();
    s->tick(50);
    ok(g_last_shell[0] == 0, "nothing is asked while the worker is still out");

    screens::g_rf_busy = 0;              // it finished, disowned, and set nothing
    g_run_spawned = 1;
    s->tick(50);
    g_run_spawned = 0;
    ok(strstr(g_last_shell, "subghz status") != nullptr,
       "the status request is made once the bus is free");

    screens::g_rf_busy = 0;
    screens::g_rf_ready = 0;
    gui::go_home();
}

// --- the sweep: a command that failed must not report success ---------------------
static void test_set_time_reports_the_truth(void) {
    using namespace nova;
    gui::go_home();

    // Settings -> Clock -> Set Time.
    open_by_key("set_system");
    input().inject(EV_SELECT); frame();                 // row 0 is Clock
    streq_(gui::top()->title(), "Clock", "the clock group opens");
    input().inject(EV_SELECT); frame();                 // row 0 is Set Time
    streq_(gui::top()->title(), "Set Time", "and Set Time with it");

    // A refusal with nothing captured, which is exactly what `date set` looks
    // like when something else holds the OS's single output buffer.
    shell_says("date set", "", 1);
    g_run_spawned = 1;
    for (int i = 0; i < 5; i++) { input().inject(EV_SELECT); frame(); }
    g_run_spawned = 0;
    ok(strstr(g_last_shell, "date set") != nullptr, "the command was run");
    ok(strstr(ui::last_notice(), "Clock set") == nullptr,
       "a refused `date set` does NOT report the clock as set");
    ok(strstr(ui::last_notice(), "Refused") != nullptr, "it says it was refused");

    shell_says(nullptr, nullptr);
    gui::go_home();
}

int main(void) {
    STAGE(test_single_instance);
    STAGE(test_one_detent_animates);
    STAGE(test_folders_open_themselves);
    STAGE(test_selection_survives_a_round_trip);
    STAGE(test_navigation);
    STAGE(test_stack_bounds);
    STAGE(test_one_step_per_frame);
    STAGE(test_catalogue);
    STAGE(test_installed_apps);
    STAGE(test_media_parsing);
    STAGE(test_media_screens);
    STAGE(test_contact_readers);
    STAGE(test_slider);
    STAGE(test_toast);
    STAGE(test_update_versions);
    STAGE(test_update_os_reading);
    STAGE(test_update_refusals);
    STAGE(test_update_script);
    STAGE(test_update_screen);
    STAGE(test_update_report);
    STAGE(test_lan_offline);
    STAGE(test_lan_sweep);
    STAGE(test_lan_interrupts);
    STAGE(test_lan_holds_the_radio);
    STAGE(test_lan_derive);
    STAGE(test_lan_reprobe_keeps_the_cursor);
    STAGE(test_lock_needs_a_code);
    STAGE(test_lock_screen);
    STAGE(test_password_lock_is_not_escapable);
    STAGE(test_lock_idle);
    STAGE(test_lock_yields_to_a_modal);
    STAGE(test_device_naming);
    STAGE(test_help_is_reachable);
    STAGE(test_pool_slots_arrive_zeroed);
    STAGE(test_menu_keeps_its_place);
    STAGE(test_list_keeps_its_place);
    STAGE(test_radio_status_is_re_asked);
    STAGE(test_set_time_reports_the_truth);
    STAGE(test_no_panel);

    printf("  %d checks", checks);
    if (failures) printf(", %d FAILED", failures);
    printf("\n");
    return failures ? 1 : 0;
}
