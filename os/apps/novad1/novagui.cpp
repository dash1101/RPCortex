// Desc: The runner — the screen stack, the status bar, and the home screen.
// File: novagui.cpp
#include "novagui.h"
#include "novaicons.h"
#include "novacore.h"
#include "novaboard.h"
#include "display.h"
#include "novasplash.h"
#include "novagui_tools.h"
#include "novagui_system.h"

#include "rpc_app.h"
#include <string.h>
#include <stdio.h>

// Placement construction, declared rather than included.
//
// <new> pulls in a freestanding header that is not guaranteed to exist for this
// target, and the only thing wanted from it is the one-line placement form.
// Nothing here ever calls the allocating new.

namespace nova {
namespace gui {

using ui::Screen;
using ui::Action;
using ui::ACT_STAY;
using ui::ACT_BACK;
using ui::ACT_HOME;

// --- the stack -----------------------------------------------------------------

// Aligned to eight, because a screen may hold a 64-bit member and an
// under-aligned placement construction is a fault on some cores and silently
// slow on others.
alignas(8) static uint8_t g_slots[STACK_MAX][SLOT_BYTES];
static Screen  *g_stack[STACK_MAX];
static unsigned g_depth;
static bool     g_dirty;
static bool     g_running;
static Canvas   g_canvas;
static uint8_t  g_fb[128 * 8];
static Perf     g_perf;

Canvas &canvas(void)  { return g_canvas; }
Screen *top(void)     { return g_depth ? g_stack[g_depth - 1] : nullptr; }
unsigned depth(void)  { return g_depth; }
void invalidate(void) { g_dirty = true; }
const Perf &perf(void){ return g_perf; }
bool running(void)    { return g_running; }
void stop(void)       { g_running = false; }

void *push_slot(void) {
    if (g_depth >= STACK_MAX) return nullptr;
    if (Screen *cur = top()) cur->leave();
    return g_slots[g_depth];
}

void push_commit(Screen *s) {
    if (!s || g_depth >= STACK_MAX) return;
    g_stack[g_depth++] = s;
    s->enter();
    g_dirty = true;
}

void pop(void) {
    // The bottom screen is home and is never popped: BACK on the home screen is
    // not "leave the UI", it is nothing at all.
    if (g_depth <= 1) return;
    Screen *s = g_stack[--g_depth];
    s->leave();
    // Explicit destruction rather than delete. These were constructed in place
    // and there is no operator delete in this package on purpose.
    s->~Screen();
    g_stack[g_depth] = nullptr;
    if (Screen *cur = top()) cur->enter();
    g_dirty = true;
}

void go_home(void) {
    while (g_depth > 1) pop();
    g_dirty = true;
}

// --- the app catalogue ----------------------------------------------------------
//
// Every app the device has, module-backed or built in, in the order the home
// screen lists them within a folder.
//
// `open` being null means the screen is not written yet. Those rows are LISTED
// rather than hidden — the same treatment an absent module gets — because a list
// that silently omits what is coming is a list that reads as complete when it is
// not.

static const App kApps[] = {
    // key          label         category      open      module
    { "wifi",       "WiFi",       CAT_WIRELESS, nullptr,  nullptr },
    { "bt",         "BLE",        CAT_WIRELESS, nullptr,  "bt" },
    { "radar",      "Radar",      CAT_WIRELESS, nullptr,  nullptr },
    { "presence",   "Presence",   CAT_WIRELESS, nullptr,  nullptr },
    { "wardrive",   "Wardrive",   CAT_WIRELESS, nullptr,  nullptr },
    { "pn532",      "NFC",        CAT_WIRELESS, nullptr,  "pn532" },
    { "ir_rx",      "IR",         CAT_WIRELESS, nullptr,  "ir_rx" },
    { "cc1101",     "Sub-GHz",    CAT_WIRELESS, nullptr,  "cc1101" },
    { "sx1276",     "LoRa",       CAT_WIRELESS, nullptr,  "sx1276" },
    { "msg",        "Messages",   CAT_WIRELESS, nullptr,  "sx1276" },

    { "gps",        "GPS",        CAT_SENSORS,  nullptr,  "gps" },
    { "dht11",      "Climate",    CAT_SENSORS,  nullptr,  "dht11" },
    { "battery",    "Battery",    CAT_SENSORS,  nullptr,  "battery" },
    { "clock",      "Clock",      CAT_SENSORS,  screens::open_clock,     nullptr },

    { "files",      "Files",      CAT_TOOLS,    nullptr,  nullptr },
    { "shell",      "Shell",      CAT_TOOLS,    nullptr,  nullptr },
    { "res",        "Resources",  CAT_TOOLS,    screens::open_resources, nullptr },
    { "notes",      "Alerts",     CAT_TOOLS,    nullptr,  nullptr },
    { "scripts",    "Scripts",    CAT_TOOLS,    nullptr,  nullptr },
    { "store",      "App Store",  CAT_TOOLS,    nullptr,  nullptr },
    { "cmds",       "Commands",   CAT_TOOLS,    nullptr,  nullptr },
    { "logs",       "Logs",       CAT_TOOLS,    nullptr,  nullptr },

    { "diag",       "Hardware",   CAT_SYSTEM,   screens::open_hardware,  nullptr },
    { "check",      "Sys Check",  CAT_SYSTEM,   nullptr,  nullptr },
    { "fix",        "Repair",     CAT_SYSTEM,   nullptr,  nullptr },
    { "power",      "Power",      CAT_SYSTEM,   nullptr,  nullptr },
    { "set_display","Display",    CAT_SYSTEM,   screens::open_display_settings, nullptr },
    { "set_home",   "Home",       CAT_SYSTEM,   nullptr,  nullptr },
    { "set_network","Network",    CAT_SYSTEM,   nullptr,  nullptr },
    { "set_security","Security",  CAT_SYSTEM,   nullptr,  nullptr },
    { "set_system", "System",     CAT_SYSTEM,   nullptr,  nullptr },
    { "set_device", "Device",     CAT_SYSTEM,   nullptr,  nullptr },
};

#define APP_COUNT (sizeof(kApps) / sizeof(kApps[0]))

const App *apps(void)    { return kApps; }
unsigned   app_count(void) { return APP_COUNT; }

bool app_available(const App &a) {
    if (!a.open) return false;
    if (!a.module) return true;
    const Module *m = module_by_id(a.module);
    if (!m) return true;
    Presence p = module_presence(*m);
    // UNKNOWN means nothing has probed for it, which is not the same as absent
    // and must not grey the app out — the SPI modules are all in that state
    // until their drivers exist.
    return p != MOD_ABSENT && p != MOD_UNWIRED;
}

// --- the status bar --------------------------------------------------------------
//
// The top nine pixels belong to the runner on every screen but a fullscreen one.
// Screens draw from ui::TOP down and never paint over this, so the time and the
// signal are in the same place whatever is open.

static void draw_status(Canvas &c, Screen *s) {
    // Left: where you are. The screen's own title, or the device name at home.
    const char *title = s ? s->title() : "";
    if (!title || !*title) title = nova::reg(NOVA_KEY_PREFIX "Name", "Nova D1");

    // Right: the clock. Laid out from the right edge so a longer title never
    // pushes the time somewhere else.
    char clock[12];
    nova::time_hhmm(clock, sizeof(clock));
    int clock_w = c.text_width(clock, 1, false);
    int right = c.width() - clock_w;
    c.text(right, 1, clock, 1);

    // A '?' marks a screen that has controls worth reading. It sits just left of
    // the clock, and its absence is information too — most screens do not need
    // one and should not carry a mark that means nothing.
    const char *help_lines[8];
    if (s && s->help(help_lines, 8) > 0) {
        right -= ui::ADV + 2;
        c.text(right, 1, "?", 1);
    }

    // The link indicator: three ascending bars when connected, an outline when
    // not. Drawn rather than lettered because "WiFi" costs four characters of a
    // twenty-one-character line and says less.
    right -= 9;
    bool linked = fw_net_connected() != 0;
    for (int i = 0; i < 3; i++) {
        int h = 2 + i * 2;
        if (linked) c.fill_rect(right + i * 3, 1 + (6 - h), 2, h, 1);
        else        c.rect(right + i * 3, 1 + (6 - h), 2, h, 1);
    }

    c.text_fit(0, 1, title, 1, right - 2, false);
    c.hline(0, ui::BARH - 1, c.width(), 1);
}

// --- the gallery ------------------------------------------------------------------
//
// The home style, and the inside of a folder. One large icon in the middle with
// its neighbours small on either side, and the label underneath.

class Gallery : public Screen {
public:
    void set(const char *title, const App *const *items, int count) {
        title_ = title; items_ = items; count_ = count; sel_ = 0;
    }

    void draw(Canvas &c) override {
        if (count_ <= 0) {
            c.text_centred(ui::TOP + 12, "Nothing here", 1);
            return;
        }
        const int mid_y = ui::TOP + 20;
        const int cx = c.width() / 2;

        // The neighbours first, so the centre icon overlaps them rather than the
        // other way round — at this size an icon drawn over the big one reads as
        // damage.
        for (int d = -2; d <= 2; d++) {
            if (d == 0) continue;
            int idx = wrapped(sel_ + d);
            int x = cx + d * 26;
            if (x < 8 || x > c.width() - 8) continue;
            const App *a = items_[idx];
            icons::draw(c, a->key, x, mid_y, 6, a->label);
            if (!app_available(*a)) strike(c, x, mid_y, 7);
        }

        const App *a = items_[sel_];
        icons::draw(c, a->key, cx, mid_y, 12, a->label);
        if (!app_available(*a)) strike(c, cx, mid_y, 13);

        c.text_centred(c.height() - ui::FH - 1, a->label, 1, 1, false);

        // Where you are in the ring. A row of pips rather than "3/12": on a list
        // that wraps, the shape tells you more than the number.
        if (count_ > 1 && count_ <= 16) {
            int w = count_ * 3 - 1;
            int x0 = cx - w / 2;
            for (int i = 0; i < count_; i++) {
                if (i == sel_) c.fill_rect(x0 + i * 3, ui::TOP + 1, 2, 2, 1);
                else           c.pixel(x0 + i * 3, ui::TOP + 1, 1);
            }
        }
    }

    Action on_event(Event e) override {
        if (count_ <= 0) return Screen::on_event(e);
        if (e == EV_ROT_CW)  { sel_ = wrapped(sel_ + 1); return ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = wrapped(sel_ - 1); return ACT_STAY; }
        if (e == EV_SELECT) {
            const App *a = items_[sel_];
            if (a->open && app_available(*a)) a->open();
            return ACT_STAY;
        }
        return Screen::on_event(e);
    }

    const char *title(void) const override { return title_; }

protected:
    const char *title_;
    const App *const *items_;
    int count_;
    int sel_;

    int wrapped(int i) const {
        if (count_ <= 0) return 0;
        while (i < 0) i += count_;
        return i % count_;
    }
    // A diagonal through an icon that cannot be opened. Greying is not available
    // on a 1-bit panel — there is no grey — so the mark has to be a shape.
    static void strike(Canvas &c, int cx, int cy, int r) {
        c.line(cx - r, cy + r, cx + r, cy - r, 1);
    }
};

// --- home -------------------------------------------------------------------------

// The lists a gallery points at. Static because a Gallery holds the pointer and
// the pool slot it lives in is smaller than the list would be.
static const App *g_cat_items[CAT_COUNT][APP_COUNT];
static int        g_cat_count[CAT_COUNT];
static const App *g_folder_items[CAT_COUNT];
static App        g_folder_apps[CAT_COUNT];
static int        g_folder_count;
static Category   g_open_cat;

static void open_category(void);

static void build_catalogue(void) {
    for (int i = 0; i < CAT_COUNT; i++) g_cat_count[i] = 0;
    for (unsigned i = 0; i < APP_COUNT; i++) {
        Category cat = kApps[i].cat;
        g_cat_items[cat][g_cat_count[cat]++] = &kApps[i];
    }
    // A folder for every category that has anything in it. The folder itself is
    // an App so the same Gallery draws both levels — one widget, one set of
    // gestures, and a folder behaves exactly like an app because it is one.
    g_folder_count = 0;
    for (int i = 0; i < CAT_COUNT; i++) {
        if (!g_cat_count[i]) continue;
        App &f = g_folder_apps[g_folder_count];
        f.key = category_name((Category)i);
        f.label = f.key;
        f.cat = (Category)i;
        f.open = open_category;
        f.module = nullptr;
        g_folder_items[g_folder_count] = &f;
        g_folder_count++;
    }
}

class Home : public Gallery {
public:
    void enter(void) override {
        style_ = nova::reg(NOVA_KEY_PREFIX "HomeStyle", "folders");
        if (nova::ieq(style_, "gallery")) {
            static const App *flat[APP_COUNT];
            for (unsigned i = 0; i < APP_COUNT; i++) flat[i] = &kApps[i];
            set("", flat, (int)APP_COUNT);
        } else {
            // Folders is the default, and the fallback when there are too few
            // groups for folders to be worth the extra level.
            set("", g_folder_items, g_folder_count);
        }
    }
    // Home is where BACK stops. Somewhere below this there is a shell, and
    // falling out of the UI into it by pressing back one time too many is not
    // something anybody meant to do.
    Action on_event(Event e) override {
        if (e == EV_BACK || e == EV_HOME) return ACT_STAY;
        return Gallery::on_event(e);
    }
private:
    const char *style_;
};

class CategoryScreen : public Gallery {
public:
    void enter(void) override {
        set(category_name(g_open_cat), g_cat_items[g_open_cat], g_cat_count[g_open_cat]);
    }
};

static void open_category(void) {
    // Which folder was chosen is read off the gallery that is still on top.
    Gallery *g = (Gallery *)top();
    (void)g;
    push<CategoryScreen>();
}

// --- the power menu -----------------------------------------------------------------

static Action power_reboot(void *, int) { fw_reboot(); return ACT_STAY; }

static const ui::MenuItem kPowerItems[] = {
    { "Lock",       nullptr,      nullptr },
    { "Incognito",  nullptr,      nullptr },
    { "Controls",   nullptr,      nullptr },
    { "Reload",     nullptr,      nullptr },
    { "Reboot",     power_reboot, nullptr },
    { "Shutdown",   nullptr,      nullptr },
};

class PowerMenu : public ui::Menu {
public:
    void enter(void) override { set("Power", kPowerItems, 6); }
};

// --- the loop --------------------------------------------------------------------

// The pacing, carried over from the MicroPython suite. These are not arbitrary:
// they are what a device feels like when it is moving, when it has just been
// touched, and when it has been put down.
#define NAP_ANIMATING 16      // 60 a second while something is moving
#define NAP_ACTIVE    33      // 30 a second just after a gesture
#define NAP_IDLE     140      // still on, nothing happening
#define NAP_DIM      200
#define NAP_OFF      300

// Screen levels. Contrast only, NEVER power-off — a panel powered down and a
// panel that failed to come up look identical, and one of those is recoverable.
#define LVL_ACTIVE 0
#define LVL_DIM    1
#define LVL_OFF    2

static int      g_level;
static uint32_t g_last_input;
static uint32_t g_frame_count;
static uint32_t g_frame_second;

static void set_level(int lvl) {
    if (lvl == g_level) return;
    g_level = lvl;
    uint8_t contrast = lvl == LVL_ACTIVE ? 0x80 : (lvl == LVL_DIM ? 0x10 : 0x00);
    display().contrast(contrast);
    g_dirty = true;
}

bool begin(void) {
    g_canvas.attach(g_fb, 128, 64);
    build_catalogue();

    bool panel = display().begin();
    input().begin();
    modules_scan();

    g_depth = 0;
    push<Home>();
    // On TOP of home, so it pops back to a screen that is already built rather
    // than building one after the animation. That is what stops the splash
    // adding to boot time instead of covering it.
    if (panel && nova::reg_bool(NOVA_KEY_PREFIX "Splash", true)) push<SplashScreen>();
    g_level = LVL_ACTIVE;
    g_last_input = fw_millis();
    g_dirty = true;
    return panel;
}

void run(void) {
    g_running = true;
    uint32_t last = fw_millis();

    while (g_running && !fw_task_should_stop()) {
        input().poll();

        bool had_input = false;
        Event e;
        while ((e = input().next()) != EV_NONE) {
            had_input = true;

            // A gesture while the screen is dark wakes it and is CONSUMED. On a
            // device you pick up out of a pocket, the first press is "show me",
            // not "open whatever the cursor happens to be on".
            if (g_level != LVL_ACTIVE) { set_level(LVL_ACTIVE); g_last_input = fw_millis(); continue; }
            g_last_input = fw_millis();

            // Holding HOME opens the power menu from ANY screen, whatever state
            // it is in, so lock and shutdown are always one gesture away. It
            // does not stack a second copy on itself.
            if (e == EV_HOME_HOLD) {
                Screen *s = top();
                if (!s || !nova::ieq(s->title(), "Power")) push<PowerMenu>();
                g_dirty = true;
                continue;
            }

            Screen *s = top();
            if (!s) continue;
            Action a = s->on_event(e);

            // HOME is a guaranteed way out, not a courtesy each screen has to
            // remember. A screen that handles it keeps its own behaviour; one
            // that ignores it gets dropped to home anyway — without this, a
            // screen that forgot the branch was a room with no door.
            //
            // `modal` is the deliberate opt-out and exists for one reason: a
            // lock that HOME escapes is not a lock.
            if (a == ACT_STAY && e == EV_HOME && !s->modal()) a = ACT_HOME;

            if      (a == ACT_BACK) pop();
            else if (a == ACT_HOME) go_home();
            g_dirty = true;
        }

        uint32_t now = fw_millis();
        uint32_t dt  = now - last;
        last = now;

        Screen *s = top();
        if (s && s->tick(dt)) g_dirty = true;

        // Dim, then off, on their own timers. Both are contrast changes, so
        // waking is instant and nothing has to be re-initialised.
        uint32_t idle = (now - g_last_input) / 1000;
        int dim_s = nova::reg_int(NOVA_KEY_PREFIX "DimSec", 30);
        int off_s = nova::reg_int(NOVA_KEY_PREFIX "OffSec", 120);
        if      (off_s > 0 && idle >= (uint32_t)off_s) set_level(LVL_OFF);
        else if (dim_s > 0 && idle >= (uint32_t)dim_s) set_level(LVL_DIM);

        if (g_dirty && s) {
            uint32_t t0 = fw_micros();
            g_canvas.clear(0);
            if (!s->fullscreen()) draw_status(g_canvas, s);
            s->draw(g_canvas);
            uint32_t t1 = fw_micros();
            display().show(g_canvas);
            uint32_t t2 = fw_micros();

            g_perf.draw_us = t1 - t0;
            g_perf.push_us = t2 - t1;
            if (g_perf.draw_us > g_perf.draw_us_max) g_perf.draw_us_max = g_perf.draw_us;
            g_perf.pages = display().last_pages();
            g_frame_count++;
            g_dirty = false;
        }

        if (now - g_frame_second >= 1000) {
            g_perf.frames = g_frame_count;
            g_frame_count = 0;
            g_frame_second = now;
        }

        // How long to sleep. A still screen must not be redrawn at sixty frames
        // a second: the loop shares a device with a shell and background work,
        // and the difference between napping 16 ms and 300 ms is the difference
        // between a day of battery and an afternoon.
        uint32_t nap;
        if      (s && s->animating())   nap = NAP_ANIMATING;
        else if (had_input)             nap = NAP_ACTIVE;
        else if (g_level == LVL_OFF)    nap = NAP_OFF;
        else if (g_level == LVL_DIM)    nap = NAP_DIM;
        else                            nap = NAP_IDLE;
        fw_task_sleep_ms(nap);
    }
    g_running = false;
}

}  // namespace gui
}  // namespace nova
