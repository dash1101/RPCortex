// Desc: Drive the simulator's exported entry points the way the web page does.
// File: sim_wasm_test.cpp
//
// sim_wasm.cpp is built twice: by build_sim.sh into the module the page loads, and
// here natively, under the sanitizers, so a change that breaks it is caught by the
// ordinary test run and not by somebody opening the simulator. The browser build
// differs in the compiler and nothing else.
//
// What is held to account is the page's own path: sim_init, then sim_frame and
// sim_key only, then reading the panel back out of sim_fb. Nothing reaches into the
// runner. If the page could not do it, this does not.
#include "sim_wasm.cpp"

#include <stdio.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("    FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int lit(void) {
    const uint8_t *fb = sim_fb();
    int n = 0;
    for (int i = 0; i < 1024; i++) for (int b = 0; b < 8; b++) n += (fb[i] >> b) & 1;
    return n;
}

// Run the page's loop for `ms` of wall time, in the ~16 ms steps requestAnimationFrame gives.
static void run_ms(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 16) sim_frame(16);
}

static void press(int ev, uint32_t settle = 300) { sim_key(ev); run_ms(settle); }

int main() {
    CHECK(sim_init(1782826800u) == 1, "sim_init refused to start the runner");
    run_ms(6000);
    CHECK(lit() > 200, "nothing on the panel after boot (%d pixels lit)", lit());
    CHECK((sim_panel() & 1) == 1, "panel is off after boot");

    // The status bar clock is the wall clock handed to sim_init (2026-06-30 12:20:00 local).
    struct FwTime t;
    CHECK(fw_time_get(&t) == 1 && t.year == 2026 && t.month == 6 && t.day == 30, "clock not set from sim_init");

    // A gesture changes the picture.
    uint32_t g0 = sim_frame(0);
    press(1);
    CHECK(sim_frame(0) != g0, "turning the knob changed nothing");
    press(2);                                       // and back, so the walk below starts where the ring began

    // Every app the catalogue lists must open, draw something, and come back —
    // through keys. The home screen is a ring of folders; SELECT opens the one in
    // the middle and the next ring is its apps, so walk both with the knob.
    // The apps in each folder, in catalogue order — which is the order the ring
    // is walked in with CCW.
    const nova::gui::App *in[nova::CAT_COUNT][24] = {};
    int per[nova::CAT_COUNT] = {0};
    for (unsigned i = 0; i < nova::gui::app_total(); i++) {
        const nova::gui::App *a = nova::gui::app_at(i);
        if (per[a->cat] < 24) in[a->cat][per[a->cat]++] = a;
    }

    int folders = 0;
    for (int cat = 0; cat < nova::CAT_COUNT; cat++) folders += per[cat] ? 1 : 0;

    // The home ring REMEMBERS its cursor across HOME (it is the same list coming
    // back), and so does each folder's, so a walk has to be relative to where the
    // ring was left rather than counted from the start.
    int opened = 0, expected = 0, folder = 0, at = 0;
    for (int cat = 0; cat < nova::CAT_COUNT; cat++) {
        if (!per[cat]) continue;                    // a category with nothing in it has no folder
        press(5, 400);                              // HOME
        for (int i = (folder - at + folders) % folders; i > 0; i--) press(2, 250);   // CCW: the ring runs this way
        at = folder;
        press(3, 500);                              // open the folder
        for (int app = 0; app < per[cat]; app++) {
            const nova::gui::App *a = in[cat][app];
            expected++;
            press(3, 150);                          // open the app under the cursor
            run_ms(1500);
            // It has to be THE app, not just something lit: the home screen draws
            // plenty of pixels too. A browser titles itself with its path, so Files
            // is the one exception, as it is in novashots.
            nova::ui::Screen *top = nova::gui::top();
            const char *title = top ? top->title() : "";
            bool right = a->open && (!strcmp(a->key, "files") || !strcmp(title, a->label));
            if (right && lit() > 12) opened++;
            else printf("    wrong screen: folder %d wanted '%s' got '%s' (%d lit)\n",
                        folder, a->label, title, lit());
            press(4, 300);                          // BACK to the folder
            press(2, 250);                          // next app
        }
        folder++;
    }
    CHECK(opened == expected, "%d of %d screens opened as themselves", opened, expected);

    // No worker is left queued: a task that never ran would be a spinner that never ends.
    press(5, 400);
    run_ms(15000);
    int still = 0;
    for (SimTask &t2 : g_sim_tasks) still += t2.used ? 1 : 0;
    CHECK(still == 0, "%d task(s) still queued after 15 s", still);

    printf("  sim_wasm_test: %d checks, %d opened\n", g_checks, opened);
    return g_fail ? 1 : 0;
}
