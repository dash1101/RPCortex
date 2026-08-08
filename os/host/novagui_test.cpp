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

int main(void) {
    STAGE(test_single_instance);
    STAGE(test_one_detent_animates);
    STAGE(test_folders_open_themselves);
    STAGE(test_selection_survives_a_round_trip);
    STAGE(test_navigation);
    STAGE(test_stack_bounds);
    STAGE(test_one_step_per_frame);
    STAGE(test_catalogue);
    STAGE(test_media_parsing);
    STAGE(test_media_screens);
    STAGE(test_contact_readers);
    STAGE(test_no_panel);

    printf("  %d checks", checks);
    if (failures) printf(", %d FAILED", failures);
    printf("\n");
    return failures ? 1 : 0;
}
