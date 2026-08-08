// Desc: The runner — the screen stack, the status bar, and the home screen.
// File: novagui.cpp
#include "novagui.h"
#include "novakeys.h"
#include "novaicons.h"
#include "novacore.h"
#include "novaboard.h"
#include "display.h"
#include "novasplash.h"
#include "novabootcheck.h"
#include "novapower.h"
#include "novagui_tools.h"
#include "novagui_system.h"
#include "novagui_files.h"
#include "novagui_settings.h"
#include "novagui_wifi.h"
#include "novagui_ops.h"
#include "novagui_apps.h"
#include "novagui_ble.h"
#include "novagui_tasks.h"

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

// Has a start been CLAIMED? Not the same question as "is the loop running".
//
// This is the bug that took the device down twice. running() is set inside
// run(), which happens after the task has been spawned AND scheduled — so
// between one caller's begin() and its loop actually starting there was a window
// where a second caller saw "not running", called begin() itself, reset g_depth
// to zero and pushed a fresh Home into slot 0 while the first task was drawing
// out of it. Two tasks then owned one screen stack: a slot was constructed by
// one while the other read its vtable pointer half-written, and the device
// jumped to address zero.
//
// It needed two service entries to happen, which a malformed `service add`
// obligingly produced. But one service plus one hand-typed `novad1 gui` is the
// same race, and that is an ordinary thing to do.
static bool     g_claimed;

Canvas &canvas(void)  { return g_canvas; }
Screen *top(void)     { return g_depth ? g_stack[g_depth - 1] : nullptr; }
unsigned depth(void)  { return g_depth; }
void invalidate(void) { g_dirty = true; }
const Perf &perf(void){ return g_perf; }
bool running(void)    { return g_running; }

// Whether run() is actually turning. Distinct from both of the others: claimed
// says a start has been taken, running says the loop wants to continue, and this
// says the loop was ever ENTERED — which is what decides who is responsible for
// giving the claim back.
static bool g_loop_live;

void stop(void) {
    g_running = false;
    // If the loop was never entered, nobody else is going to release the claim.
    // That is not hypothetical: begin() succeeds and then the task spawn fails,
    // and without this the screen could never be started again without a reboot
    // — for a failure whose whole message is "try again".
    if (!g_loop_live) {
        input().stop();
        g_claimed = false;
    }
}

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
    { "wifi",       "WiFi",       CAT_WIRELESS, screens::open_wifi,      nullptr },
    { "bt",         "BLE",        CAT_WIRELESS, screens::open_ble,       "bt" },
    { "radar",      "Radar",      CAT_WIRELESS, screens::open_radar,     nullptr },
    { "presence",   "Presence",   CAT_WIRELESS, screens::open_presence,  nullptr },
    { "wardrive",   "Wardrive",   CAT_WIRELESS, screens::open_wardrive,  nullptr },
    { "pn532",      "NFC",        CAT_WIRELESS, nullptr,  "pn532" },
    // 'ir' is the app the MicroPython home used; ir_rx and ir_tx are the two
    // halves of the hardware behind it, and they have their own icons.
    { "ir",         "IR",         CAT_WIRELESS, nullptr,  "ir_rx" },
    { "cc1101",     "Sub-GHz",    CAT_WIRELESS, nullptr,  "cc1101" },
    { "sx1276",     "LoRa",       CAT_WIRELESS, nullptr,  "sx1276" },
    { "msg",        "Messages",   CAT_WIRELESS, nullptr,  "sx1276" },

    { "gps",        "GPS",        CAT_SENSORS,  nullptr,  "gps" },
    { "dht11",      "Climate",    CAT_SENSORS,  nullptr,  "dht11" },
    { "battery",    "Battery",    CAT_SENSORS,  nullptr,  "battery" },
    { "clock",      "Clock",      CAT_SENSORS,  screens::open_clock,     nullptr },

    { "files",      "Files",      CAT_TOOLS,    screens::open_files,     nullptr },
    { "shell",      "Shell",      CAT_TOOLS,    screens::open_shell,     nullptr },
    { "res",        "Resources",  CAT_TOOLS,    screens::open_resources, nullptr },
    { "notes",      "Alerts",     CAT_TOOLS,    screens::open_alerts,    nullptr },
    { "scripts",    "Scripts",    CAT_TOOLS,    screens::open_scripts,   nullptr },
    { "store",      "App Store",  CAT_TOOLS,    screens::open_store,     nullptr },
    { "cmds",       "Commands",   CAT_TOOLS,    screens::open_commands,  nullptr },
    { "logs",       "Logs",       CAT_TOOLS,    screens::open_logs,      nullptr },

    { "diag",       "Hardware",   CAT_SYSTEM,   screens::open_hardware,  nullptr },
    { "tasks",      "Tasks",      CAT_SYSTEM,   screens::open_tasks,     nullptr },
    { "check",      "Sys Check",  CAT_SYSTEM,   screens::open_check,     nullptr },
    { "fix",        "Repair",     CAT_SYSTEM,   screens::open_repair,    nullptr },
    { "power",      "Power",      CAT_SYSTEM,   screens::open_power,     nullptr },
    { "set_display","Display",    CAT_SYSTEM,   screens::open_display_settings, nullptr },
    { "set_home",   "Home",       CAT_SYSTEM,   screens::open_set_home,     nullptr },
    { "set_network","Network",    CAT_SYSTEM,   screens::open_set_network,  nullptr },
    { "set_security","Security",  CAT_SYSTEM,   screens::open_set_security, nullptr },
    { "set_system", "System",     CAT_SYSTEM,   screens::open_set_system,   nullptr },
    // No "Device" row. What this device IS reaches through System -> Versions,
    // which is where the MicroPython suite kept it — identity is not a peer of
    // Display and Network, and a sixth settings icon in this folder was the
    // clutter the grouped layout exists to avoid.
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

// Say why a struck-through app did not open.
//
// Pressing one used to do nothing whatsoever, which on a device with one button
// is indistinguishable from the button not working. v1 always answered — "No
// CC1101 found" — and being told the chip is missing is the difference between
// a broken device and one that is telling you what to plug in.
//
// The two reasons are not the same and must not read the same: an app with no
// screen yet is on us, and an app whose chip is absent is a wiring job.
void explain_unavailable(const App &a) {
    static char body[128];

    if (!a.open) {
        snprintf(body, sizeof(body),
                 "%s has no screen in this build yet.", a.label);
        ui::notice(a.label, body);
        return;
    }

    const Module *m = a.module ? module_by_id(a.module) : nullptr;
    if (!m) { ui::notice(a.label, "Unavailable."); return; }

    if (module_presence(*m) == MOD_UNWIRED)
        snprintf(body, sizeof(body),
                 "No pins are assigned to the %s. Set them in "
                 "System, Hardware.", m->chip);
    else
        snprintf(body, sizeof(body),
                 "No %s answered. Check it is wired to the pins in "
                 "System, Hardware.", m->chip);
    ui::notice(a.label, body);
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

    // Power, to the left of the clock. The badge is drawn in novaui.cpp; the
    // decision about WHICH of its three states to show is here, because it is
    // the one place that has both halves of the question.
    //
    // They are separate questions. Where the power comes from is the firmware's
    // to answer — VBUS sense on a Pico 2 W is a pin on the radio module, not one
    // a package can read — while whether there is a LEVEL depends on the battery
    // divider being wired, which on the reference board it is not. So the board
    // can know perfectly well that it is running off the cell and still have
    // nothing to say about how full it is, and the badge has to survive that.
    //
    // USB wins when it is present: on external power a cell reads as a charging
    // voltage rather than a state of charge, and drawing that as a level would
    // be a number that moves for the wrong reason.
    right -= ui::POWER_W;
    ui::power_badge(c, right, power::source() == power::PWR_USB, power::percent());

    // The link indicator, and the bars mean something.
    //
    // The first version drew filled bars when connected and "an outline" when
    // not — except a rect two pixels wide has no inside, so the outline WAS a
    // filled bar and the two states were the same picture. It read as full
    // signal on a device that had never joined anything.
    //
    // Now: how many bars are lit follows the actual RSSI, and not connected is
    // an empty baseline with a dot over it — a shape, not a shade, because a
    // 1-bit panel has no shades to tell apart.
    right -= 11;
    {
        int bars = 0;
        if (fw_net_connected()) {
            int rssi = fw_net_rssi();
            // -50 and better is three, -67 is two, -75 is one, worse is a link
            // that exists and is barely there. A reading of 0 means the radio
            // did not answer, which is still a link — one bar.
            bars = rssi == 0 ? 1 : rssi >= -55 ? 3 : rssi >= -70 ? 2 : 1;
        }
        for (int i = 0; i < 3; i++) {
            int h = 2 + i * 2;
            int x = right + i * 3;
            if (i < bars) c.fill_rect(x, 1 + (6 - h), 2, h, 1);
            else          c.hline(x, 7, 2, 1);        // the baseline it would stand on
        }
        if (!bars) {
            // A dot above the empty bars: unmistakably "nothing", and it cannot
            // be confused with one weak bar.
            c.pixel(right + 2, 2, 1);
            c.pixel(right + 4, 2, 1);
        }
    }

    c.text_fit(0, 1, title, 1, right - 2, false);
    c.hline(0, ui::BARH - 1, c.width(), 1);
}

// --- the gallery ------------------------------------------------------------------
//
// The home style, and the inside of a folder. One large icon in the middle with
// its neighbours small on either side, and the label underneath.

// The row whose open() is running, for an OpenFn that serves several of them.
// Set immediately before the call and read immediately inside it; nothing keeps
// it beyond that, and nothing should.
static const App *g_chosen;

class Gallery : public Screen {
public:
    void set(const char *title, const App *const *items, int count) {
        // KEEP THE SELECTION when the same list comes back.
        //
        // pop() calls enter() on the screen underneath and enter() calls this,
        // so closing an app dropped the cursor to the first icon in the folder
        // rather than leaving it on the one just closed. Reported from the
        // device as "open Wardrive, close it, and it hovers the default app
        // instead" — and it did that for every folder and for home.
        //
        // Identity of the item ARRAY is the test rather than the title. Each
        // category has its own array, so moving between folders resets the
        // cursor and coming back to one does not.
        if (items != items_ || count != count_) sel_ = 0;
        title_ = title; items_ = items; count_ = count;
        // A list that shrank underneath a remembered position — an app hidden
        // from the home screen while its folder was open — would otherwise read
        // past the end of it.
        if (sel_ >= count_) sel_ = count_ ? count_ - 1 : 0;
        slide_ = 0; dir_ = 0;
    }

    // The ring SLIDES rather than jumping.
    //
    // `slide_` runs from 256 down to 0 across a step, and every icon is placed
    // and sized from it — so the one arriving at the centre grows into it while
    // the one leaving shrinks away, and the whole row moves together. Jumping
    // between two fixed sizes reads as a slideshow; this reads as a ring being
    // turned, which is what it is.
    //
    // Fixed point, in 256ths. There is no floating point anywhere in this
    // package and this is not the place to start needing it.
    bool tick(uint32_t dt) override {
        if (!slide_) return false;
        uint32_t step = dt * 256 / SLIDE_MS;
        slide_ = (slide_ > (int)step) ? slide_ - (int)step : 0;
        return true;
    }
    bool animating(void) const override { return slide_ != 0; }

    void draw(Canvas &c) override {
        if (count_ <= 0) {
            c.text_centred(ui::TOP + 12, "Nothing here", 1);
            return;
        }
        const int mid_y = ui::RING_Y;
        const int cx = c.width() / 2;

        // Where the ring is between two positions, in pixels. dir_ says which
        // way it is going, slide_ how far it still has to travel.
        const int off = dir_ * slide_ * SPACING / 256;

        // THREE at a time — the one under the cursor and one either side, which
        // is what the MicroPython home showed. Five fits on the panel and is
        // worse: at six pixels the outer pair are unreadable, so they add
        // clutter without adding information, and they make the ring look
        // crowded rather than focused.
        //
        // Outside in, so the centre icon is drawn last and overlaps its
        // neighbours rather than the other way round — at this size an icon
        // drawn over the big one reads as damage.
        for (int pass = 1; pass >= 0; pass--) {
            for (int d = -1; d <= 1; d++) {
                if ((d < 0 ? -d : d) != pass) continue;
                int idx = wrapped(sel_ + d);
                int x = cx + d * SPACING + off;
                if (x < -R_BIG || x > c.width() + R_BIG) continue;

                // Size follows DISTANCE FROM THE CENTRE, continuously. An icon
                // halfway between two slots is halfway between two sizes, which
                // is the whole reason this reads as motion.
                int dist = x > cx ? x - cx : cx - x;
                int r = R_BIG - dist * (R_BIG - R_SMALL) / SPACING;
                if (r < R_SMALL) r = R_SMALL;
                if (r > R_BIG)   r = R_BIG;

                const App *a = items_[idx];
                icons::draw(c, a->key, x, mid_y, r, a->label);
                if (!app_available(*a)) strike(c, x, mid_y, r + 1);
            }
        }

        // The label belongs to whichever icon is nearest the middle, so it
        // changes at the halfway point of a step rather than at either end.
        const App *a = items_[wrapped(sel_ + ((off > SPACING / 2) ? -1 :
                                              (off < -SPACING / 2) ? 1 : 0))];
        c.text_centred(ui::label_y(c), a->label, 1, 1, false);

        // Where you are in the ring. A row of pips rather than "3/12": on a list
        // that wraps, the shape tells you more than the number.
        //
        // UNDER THE LABEL, at the foot of the panel. They used to sit on the
        // first row of the body, a pixel below the status rule, where a row of
        // dots hard against the bar read as part of it — decoration competing
        // with the clock rather than an answer to "where am I". At the bottom
        // they belong to the thing they describe and the top of the screen goes
        // back to being the status bar.
        //
        // Every pip shares a baseline and the current one is the only one with
        // any height, so the row reads as a scale with a marker on it.
        if (count_ > 1 && count_ <= 16) {
            const int y = ui::pip_y(c);
            const int base = y + ui::PIP_H - 1;
            int x0 = cx - (count_ * ui::PIP_PITCH - 1) / 2;
            for (int i = 0; i < count_; i++) {
                int x = x0 + i * ui::PIP_PITCH;
                if (i == sel_) c.fill_rect(x, y, ui::PIP_H, ui::PIP_H, 1);
                else           c.pixel(x, base, 1);
            }
        }
    }

    Action on_event(Event e) override {
        if (count_ <= 0) return Screen::on_event(e);
        // Moving starts the slide from a full step away, in the direction it
        // came from, so the icons appear to travel from where they were.
        if (e == EV_ROT_CW)  { sel_ = wrapped(sel_ + 1); slide_ = 256; dir_ =  1; return ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = wrapped(sel_ - 1); slide_ = 256; dir_ = -1; return ACT_STAY; }
        if (e == EV_SELECT) {
            const App *a = items_[sel_];
            // WHICH app is being opened, recorded before the call.
            //
            // An OpenFn takes nothing and returns nothing, so a screen that
            // serves more than one row — the folders all share open_category —
            // has no other way to know which one was chosen. It used to try to
            // read it back off the gallery still on top, through an unchecked
            // cast, and the read was never written: every folder opened the
            // first category. Reported as "the System folder takes me to
            // Network, and so do all the others".
            g_chosen = a;
            if (a->open && app_available(*a)) a->open();
            else explain_unavailable(*a);
            return ACT_STAY;
        }
        return Screen::on_event(e);
    }

    const char *title(void) const override { return title_; }

protected:
    // The ring's shape is a layout token, not a private constant: the host
    // renderer draws the same frame and had its own copy of all three.
    static constexpr int SPACING  = ui::ICON_SPACING;
    static constexpr int R_BIG    = ui::ICON_BIG;
    static constexpr int R_SMALL  = ui::ICON_SMALL;
    static constexpr int SLIDE_MS = 140;  // long enough to see, short enough
                                          // that a fast spin does not lag behind

    const char *title_;
    const App *const *items_;
    int count_;
    int sel_;
    int slide_;      // 256 at the start of a step, 0 when it has arrived
    int dir_;        // which way the ring is turning

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
    // The folder that was just chosen, not the gallery it was chosen from.
    // Casting top() to a Gallery to ask it was the old way and it was wrong
    // twice over: the cast is unchecked, and the answer was never recorded.
    if (!g_chosen) return;
    g_open_cat = g_chosen->cat;
    push<CategoryScreen>();
}

// --- the power menu -----------------------------------------------------------------
//
// The real one lives in novagui_ops.cpp. This used to be a six-row menu with
// five inert rows, written before there was anything behind them — and holding
// HOME went on opening it after the real screen arrived, because this is what
// the runner pushed. A stub that outlives its replacement is worse than no stub
// at all: the feature exists and cannot be reached.

// --- the loop --------------------------------------------------------------------

// The pacing, carried over from the MicroPython suite. These are not arbitrary:
// they are what a device feels like when it is moving, when it has just been
// touched, and when it has been put down.
// AN ANIMATION STARTS WHEN THE GESTURE IS HANDLED, not however long the screen
// sat still before the knob moved.
//
// dt is the gap since the last frame, and the frame before a gesture is the
// IDLE one — so the first tick of a new slide was handed the whole idle nap.
// NAP_IDLE is 140 ms and SLIDE_MS is 140 ms, which makes the step exactly 256:
// the entire animation consumed in a single frame, every time, arriving as a
// jump. Dimmed it is 200 ms and worse.
//
// A fast scroll escaped it because the queue was never empty, so the loop was
// already turning at 16 ms by the time the second detent landed and every dt
// after the first was small. That is the reported symptom exactly — one click
// jumps, several animate — and it survived two passes of fixing the frame
// pacing, which was a real bug and a different one.
//
// Zeroing the gap costs one frame of stillness at the very start of a movement,
// which is not visible.
uint32_t frame_dt(uint32_t now, uint32_t *last, bool had_input) {
    if (!last) return 0;
    if (had_input) *last = now;
    uint32_t dt = now - *last;
    *last = now;
    return dt;
}

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

bool started(void) { return g_claimed; }

bool begin(void) {
    // ONE INSTANCE. The screen stack, the canvas and the frame buffer are all
    // statics: a second runner is not a second screen, it is two tasks writing
    // the same memory.
    if (g_claimed) return false;
    g_claimed = true;

    g_canvas.attach(g_fb, 128, 64);
    build_catalogue();

    bool panel = display().begin();
    if (input().begin()) input().start();
    modules_scan();

    g_depth = 0;
    push<Home>();
    // On TOP of home, so it pops back to a screen that is already built rather
    // than building one after the animation. That is what stops the splash
    // adding to boot time instead of covering it.
    // Home, then the check, then the splash — so they pop in the order they are
    // meant to be seen and each one lands on something already built.
    if (panel) {
        push<BootCheckScreen>();
    }
    g_level = LVL_ACTIVE;
    g_last_input = fw_millis();
    g_dirty = true;

    // No panel means no runner, so the claim goes back. Holding it would mean a
    // device that failed to find its screen once could never be told to look
    // again without a reboot — and looking again after fixing the wiring is
    // exactly what somebody does next.
    if (!panel) g_claimed = false;
    return panel;
}

void run(void) {
    // Refuse a loop nobody claimed, and refuse a second one. Two tasks in here
    // is the same corruption from the other end, and a task arriving without a
    // claim is a spawn that outlived the start that made it.
    if (!g_claimed || g_loop_live) return;
    g_loop_live = true;
    g_running = true;
    uint32_t last = fw_millis();

    while (g_running && !fw_task_should_stop()) {
        // No poll here. The input task samples the encoder at 2 ms because
        // quadrature needs consecutive readings, and this loop sleeps up to
        // 300 ms — see the note at the top of novainput.cpp.

        bool had_input = false;
        bool turned = false;      // a rotation was consumed this frame
        Event e;
        while ((e = input().next()) != EV_NONE) {
            had_input = true;

            // ONE DETENT PER FRAME, and this is the whole difference between an
            // encoder that feels connected to the screen and one that does not.
            //
            // Draining the queue in a single frame means a fast spin moves the
            // selection six rows and redraws once: the cursor teleports and
            // there is no sense of having travelled. Taking one step per frame
            // draws every position it passes through, which is what reads as
            // smooth. The MicroPython suite arrived at the same answer and was
            // capped at its loop rate; here the loop runs at 60 a second while
            // anything is queued, so a fast spin keeps up instead of lagging.
            //
            // Buttons are NOT rationed — a press waiting behind three detents
            // would be a button that feels stuck.
            if (e == EV_ROT_CW || e == EV_ROT_CCW) {
                if (turned) { input().unget(e); break; }
                turned = true;
            }

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
                if (!s || !nova::ieq(s->title(), "Power")) screens::open_power();
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
        uint32_t dt  = frame_dt(now, &last, had_input);

        Screen *s = top();
        if (s && s->tick(dt)) g_dirty = true;
        // RE-READ THE TOP. A screen may pop ITSELF from tick — the splash does
        // exactly that when its animation ends — and the pointer taken before
        // the call then refers to a slot that has been handed back. Drawing
        // through it painted the splash again every frame, so the boot screen
        // stayed up until a gesture arrived and pushed things along by another
        // route. Nothing crashed, which is why it looked like a timing problem.
        s = top();

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
        // Still something queued means still turning. Holding the fast frame
        // rate is what lets the one-step-per-frame rule keep up with a spin.
        if      (input().pending())     nap = NAP_ANIMATING;
        else if (s && s->animating())   nap = NAP_ANIMATING;
        else if (had_input)             nap = NAP_ACTIVE;
        else if (g_level == LVL_OFF)    nap = NAP_OFF;
        else if (g_level == LVL_DIM)    nap = NAP_DIM;
        else                            nap = NAP_IDLE;
        // PACE THE FRAME, do not just sleep after it.
        //
        // Sleeping the full nap AFTER the work means a 16 ms target actually
        // takes 16 plus however long the frame took, and how long that is
        // depends entirely on how much of the panel changed. Measured on a
        // Pico 2 W: composing costs about 1.5 ms whatever happens, and the push
        // costs 1.5 ms for the one page a settled screen touches and 11.6 ms
        // for all eight.
        //
        // An ANIMATION is the eight-page case, every frame. So the "60 a
        // second" animation rate was really 34, and a 90 ms slide got three
        // frames. Three frames does not read as motion; it reads as a jump,
        // which is exactly what a single detent looked like while a fast scroll
        // — which queues enough steps to last — animated perfectly.
        uint32_t spent = fw_millis() - now;
        fw_task_sleep_ms(spent >= nap ? 1 : nap - spent);
    }
    g_running = false;
    g_loop_live = false;
    input().stop();
    // Released last, so nothing can claim a start until this one has finished
    // tearing down.
    g_claimed = false;
}

}  // namespace gui
}  // namespace nova
