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

// --- the firmware, faked ----------------------------------------------------------
//
// Small and honest rather than clever: enough for the runner to run, with the
// pieces a test wants to control exposed as globals.

static uint32_t g_ms;
static char     g_reg[32][2][128];
static int      g_regn;
static bool     g_panel_present = true;
static int      g_spawned;
static int      g_i2c_writes;

extern "C" {

uint32_t fw_millis(void) { return g_ms; }
uint32_t fw_micros(void) { return g_ms * 1000u; }
void     fw_task_sleep_ms(uint32_t ms) { g_ms += ms; }
void     fw_task_yield(void) {}
int      fw_task_should_stop(void) { return 0; }
int      fw_task_self(void) { return 1; }
uint32_t fw_cores(void) { return 2; }
uint32_t fw_core_id(void) { return 0; }
uint32_t fw_cpu_percent(void) { return 7; }
void     fw_reboot(void) {}

// Spawning is COUNTED rather than performed. A second runner is what this
// harness is here to catch, and counting the attempt is how it catches it
// without needing threads.
int fw_task_spawn(const char *, int (*)(void *), void *, uint32_t) {
    return ++g_spawned + 10;
}
int fw_task_kill(int) { return 1; }

int fw_printf(const char *, ...) { return 0; }
void fw_log(int, const char *) {}
void fw_progress(const char *) {}
int  rpc_register_command(const char *, const char *, int (*)(int, char **)) { return 1; }
int  fw_shell_run(const char *, char *out, uint32_t cap) { if (out && cap) out[0] = 0; return 0; }

void *fw_malloc(size_t n) { return malloc(n); }
void  fw_free(void *p) { free(p); }
uint32_t fw_heap_free(void) { return 240 * 1024; }
uint32_t fw_heap_total(void) { return 300 * 1024; }
uint32_t fw_heap_largest(void) { return 200 * 1024; }

int  fw_reg_get(const char *k, char *out, uint32_t cap) {
    for (int i = 0; i < g_regn; i++)
        if (!strcmp(g_reg[i][0], k)) { snprintf(out, cap, "%s", g_reg[i][1]); return 1; }
    if (cap) out[0] = 0;
    return 0;
}
int fw_reg_set(const char *k, const char *v) {
    for (int i = 0; i < g_regn; i++)
        if (!strcmp(g_reg[i][0], k)) { snprintf(g_reg[i][1], 128, "%s", v); return 1; }
    if (g_regn >= 32) return 0;
    snprintf(g_reg[g_regn][0], 128, "%s", k);
    snprintf(g_reg[g_regn][1], 128, "%s", v);
    g_regn++;
    return 1;
}
int32_t fw_reg_get_int(const char *k, int32_t d) {
    char b[128];
    if (!fw_reg_get(k, b, sizeof(b)) || !b[0]) return d;
    return (int32_t)atoi(b);
}
int  fw_reg_has(const char *k) { char b[4]; return fw_reg_get(k, b, sizeof(b)); }
void fw_reg_save(void) {}

int      fw_file_exists(const char *) { return 1; }
int      fw_mkdir(const char *) { return 1; }
int      fw_file_write(const char *, const void *, uint32_t) { return 1; }
uint32_t fw_file_read(const char *, void *, uint32_t) { return 0; }
int      fw_file_remove(const char *) { return 1; }

int fw_board(char *out, unsigned cap) { snprintf(out, cap, "pico2_w"); return 1; }
unsigned fw_gpio_count(void) { return 30; }
int fw_gpio_usable(unsigned p) { return (p < 30 && p != 23 && p != 24 && p != 25 && p != 29) ? 1 : 0; }
int fw_gpio_init(unsigned, int) { return 0; }
int fw_gpio_pull(unsigned, int) { return 0; }
int fw_gpio_put(unsigned, int) { return 0; }
int fw_gpio_get(unsigned) { return 1; }
int fw_gpio_watch(unsigned, int) { return 0; }
int fw_gpio_events(unsigned, int *level) { if (level) *level = 1; return 0; }
int fw_adc_init(unsigned) { return 0; }
int fw_adc_read(unsigned) { return -1; }
unsigned fw_adc_temp_channel(void) { return 4; }

int fw_i2c_init(unsigned, unsigned, unsigned, unsigned) { return 0; }
int fw_i2c_write(unsigned, unsigned, const void *, unsigned, int) {
    g_i2c_writes++;
    return g_panel_present ? 0 : -1;
}
int fw_i2c_read(unsigned, unsigned, void *, unsigned, int) { return -1; }
int fw_i2c_deinit(unsigned) { return 0; }

int fw_pwm_init(unsigned, unsigned) { return 1000; }
int fw_pwm_duty(unsigned, unsigned) { return 0; }
int fw_pwm_stop(unsigned) { return 0; }
void fw_busy_wait_us(uint32_t) {}

int fw_net_connected(void) { return 0; }
int fw_net_ssid(char *out, unsigned cap) { if (cap) out[0] = 0; return 0; }
int fw_net_ip(char *out, unsigned cap) { if (cap) out[0] = 0; return 0; }
int fw_net_rssi(void) { return 0; }

int fw_time_get(struct FwTime *out) { (void)out; return 0; }

// The screens that arrived with the settings, wireless, files and operations
// groups reach further into the ABI than the runner alone ever did.
int fw_unique_id(char *out, unsigned cap) { snprintf(out, cap, "E6614104037A5E2F"); return 1; }
uint32_t fw_clock_hz(void) { return 150000000u; }
int fw_net_scan(FwNetAp *out, unsigned max) { (void)out; (void)max; return 0; }
int fw_dir_count(const char *) { return 0; }
int fw_dir_entry(const char *, unsigned, FwDirEntry *) { return 0; }
uint32_t fw_file_size(const char *) { return 0; }
uint32_t fw_file_read_at(const char *, uint32_t, void *, uint32_t) { return 0; }
unsigned long fw_random(void) { return 4; }
void fw_random_bytes(void *buf, unsigned len) { for (unsigned i = 0; i < len; i++) ((unsigned char *)buf)[i] = 0; }
int fw_power_sleep(unsigned, int, int) { return 0; }
int fw_power_dormant(unsigned, int, int) { return 0; }
unsigned fw_power_min_sleep_ms(void) { return 1; }

}  // extern "C"

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

#define STAGE(f) do { fprintf(stderr, "  .. %s\n", #f); f(); } while (0)

int main(void) {
    STAGE(test_single_instance);
    STAGE(test_one_detent_animates);
    STAGE(test_folders_open_themselves);
    STAGE(test_navigation);
    STAGE(test_stack_bounds);
    STAGE(test_one_step_per_frame);
    STAGE(test_catalogue);
    STAGE(test_no_panel);

    printf("  %d checks", checks);
    if (failures) printf(", %d FAILED", failures);
    printf("\n");
    return failures ? 1 : 0;
}
