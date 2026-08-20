// Desc: The runner — the screen stack, the status bar, and the home screen.
// File: novagui.cpp
#include "novagui.h"
#include "novaapps.h"
#include "novakeys.h"
#include "novanotify.h"
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
#include "novagui_media.h"
#include "novagui_contact.h"
#include "novagui_radios.h"
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
    // "Home" means the bottom of the stack — unless the lock is up, in which
    // case it means the lock, because the lock IS the floor. See lock_floor():
    // the on-screen keyboard returns ACT_HOME from EV_HOME and it is the
    // password lock's entry screen, so without this a locked device is one
    // press away from being unlocked for good.
    //
    // Here rather than in run(), so it holds for every caller — including the
    // harness, which drives the loop with its own copy and would otherwise be
    // testing a rule that is not the one the device follows.
    unsigned floor = screens::lock_floor();
    if (floor < 1) floor = 1;
    while (g_depth > floor) pop();
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
    // Next to WiFi rather than in Tools: it is a question about the network the
    // row above it joined, and it is useless until that one has.
    { "lan",        "LAN",        CAT_WIRELESS, screens::open_lan,       nullptr },
    { "bt",         "BLE",        CAT_WIRELESS, screens::open_ble,       "bt" },
    { "radar",      "Radar",      CAT_WIRELESS, screens::open_radar,     nullptr },
    { "presence",   "Presence",   CAT_WIRELESS, screens::open_presence,  nullptr },
    { "wardrive",   "Wardrive",   CAT_WIRELESS, screens::open_wardrive,  nullptr },
    { "pn532",      "NFC",        CAT_WIRELESS, screens::open_nfc,       "pn532" },
    // 'ir' is the app the MicroPython home used; ir_rx and ir_tx are the two
    // halves of the hardware behind it, and they have their own icons.
    { "ir",         "IR",         CAT_WIRELESS, nullptr,  "ir_rx" },
    { "cc1101",     "Sub-GHz",    CAT_WIRELESS, screens::open_subghz, "cc1101" },
    { "sx1276",     "LoRa",       CAT_WIRELESS, screens::open_lora,   "sx1276" },
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
    // The media player needs Classic Bluetooth to reach a speaker, so it names
    // the radio and greys out on a board that has none.
    { "media",      "Media",      CAT_TOOLS,    screens::open_media,     "bt" },
    { "cmds",       "Commands",   CAT_TOOLS,    screens::open_commands,  nullptr },
    { "logs",       "Logs",       CAT_TOOLS,    screens::open_logs,      nullptr },
    // The module table files iButton under Testing, and this row is in Tools
    // ON PURPOSE. No app has ever been in the Testing category, and build_catalogue
    // makes a home-screen folder for every category that has anything in it — so
    // putting it there would grow a new "Testing" folder holding one app. It is a
    // tool in the sense Files and Commands are: a thing you open to do a job.
    { "ibutton",    "iButton",    CAT_TOOLS,    screens::open_ibutton,   "ibutton" },

    { "diag",       "Hardware",   CAT_SYSTEM,   screens::open_hardware,  nullptr },
    { "tasks",      "Tasks",      CAT_SYSTEM,   screens::open_tasks,     nullptr },
    { "check",      "Sys Check",  CAT_SYSTEM,   screens::open_check,     nullptr },
    { "fix",        "Repair",     CAT_SYSTEM,   screens::open_repair,    nullptr },
    // In System rather than in Tools beside the App Store, and the split is
    // real: the store installs somebody else's package, this replaces the
    // device's own software and the OS underneath it. It is the same shelf as
    // Sys Check and Repair — things you open when you are asking after the
    // device itself.
    { "update",     "Updates",    CAT_SYSTEM,   screens::open_updates,   nullptr },
    { "power",      "Power",      CAT_SYSTEM,   screens::open_power,     nullptr },
    { "set_display","Display",    CAT_SYSTEM,   screens::open_display_settings, nullptr },
    { "set_home",   "Home",       CAT_SYSTEM,   screens::open_set_home,     nullptr },
    { "set_network","Network",    CAT_SYSTEM,   screens::open_set_network,  nullptr },
    { "set_security","Security",  CAT_SYSTEM,   screens::open_set_security, nullptr },
    // "Settings", not "System": this row lives INSIDE the System folder, and a
    // System folder holding a System row read as a dead end that looped back on
    // itself. It is the general device settings — clock, versions, reset — so
    // Settings is both what it is and what stops the folder repeating its name.
    { "set_system", "Settings",   CAT_SYSTEM,   screens::open_set_system,   nullptr },
    // No "Device" row. What this device IS reaches through System -> Versions,
    // which is where the MicroPython suite kept it — identity is not a peer of
    // Display and Network, and a sixth settings icon in this folder was the
    // clutter the grouped layout exists to avoid.
};

#define APP_COUNT (sizeof(kApps) / sizeof(kApps[0]))

// The apps somebody else wrote, as catalogue rows.
//
// A SECOND array rather than a longer first one. The built-in table is
// .data.rel.ro — every field is a relocated pointer, so it is already in RAM —
// and copying it into a combined table would cost seven hundred bytes to say
// what two accessors say for nothing. The key and label point INTO the scan's
// own entry, which outlives every row here because both are statics.
static App g_user_apps[NAPP_MAX];
static unsigned g_user_n;

const App *apps(void)      { return kApps; }
unsigned   app_count(void) { return APP_COUNT; }

unsigned app_total(void)   { return APP_COUNT + g_user_n; }

const App *app_at(unsigned i) {
    if (i < APP_COUNT) return &kApps[i];
    i -= APP_COUNT;
    return i < g_user_n ? &g_user_apps[i] : nullptr;
}

// Rebuild the user half from whatever the last scan found.
static void user_apps_build(void) {
    g_user_n = 0;
    for (int i = 0; i < napps::count() && g_user_n < NAPP_MAX; i++) {
        const napps::NappItem *it = napps::at(i);
        if (!it) break;
        App &a = g_user_apps[g_user_n++];
        a.key    = it->key;
        a.label  = it->label;
        a.cat    = it->cat;
        // Openable even when the header is faulty. The screen is where the
        // reason gets said, and an app that greys out instead has nowhere to
        // say it — which is how the MicroPython suite left somebody with a file
        // on the device, no icon and no explanation.
        a.open   = screens::open_user_app;
        // No module gate. An app's rows name commands, and whether a command
        // works is the command's business to report.
        a.module = nullptr;
    }
}

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

    const Module *m = a.module ? module_by_id(a.module) : nullptr;

    if (!a.open) {
        // The chip it will want, when it is known. "No screen yet" answers the
        // button; naming the part answers the question that comes next, and the
        // notice title is already the app's name so repeating it wastes a line
        // on a screen 128 pixels wide.
        if (m) snprintf(body, sizeof(body),
                        "No screen for this yet. It will need a %s.", m->chip);
        else   snprintf(body, sizeof(body), "No screen for this yet.");
        ui::notice(a.label, body);
        return;
    }

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

    // A little watch face when the stopwatch is running in the background, so it
    // is visible from any screen — the one cue that timing is still happening
    // while you are somewhere else.
    if (screens::stopwatch_running()) {
        right -= 8;
        c.circle(right + 3, 4, 2, 1);
        c.pixel(right + 3, 4, 1);
    }

    c.text_fit(0, 1, title, 1, right - 2, false);
    c.hline(0, ui::BARH - 1, c.width(), 1);
}

// --- the toast ------------------------------------------------------------------
//
// A notification, bannered over whatever screen is up for a few seconds, so it
// is seen from ANYWHERE without the app underneath being touched. The screen
// stack does not change — this is drawn last, on top, and cleared by a timer or
// the next press. The Alerts app and the status count remain the record; this is
// only the glance as it happens.
static char     g_toast_msg[64];
static uint32_t g_toast_until;      // fw_millis deadline, 0 when nothing is up

// A hash of exactly what the STATUS BAR draws that can change on its own. The
// bar is the runner's, not any screen's, so when the clock ticked or the power
// changed nothing marked the frame dirty and it went stale until a gesture
// forced a redraw. The loop compares this each frame and redraws when it moves.
//
// Only SAFE, cheap reads: the connected flag and power are cached, the clock is
// a timer read, the stopwatch is a bool. The link BARS are deliberately NOT in
// here — reading live RSSI means a cyw43 call, and doing that on a timer from
// this (the GUI) core is the cross-core radio hazard the incognito freeze was.
// The bars still refresh, because draw_status reads them live on every paint and
// these other changes cause paints; only an RSSI-only change waits for the next.
static uint32_t g_status_sig;

static uint32_t status_signature(void) {
    uint32_t h = 2166136261u;      // FNV-1a, folded inline (no macro — checkuniq)
    h = (h ^ (uint32_t)(fw_net_connected() ? 1 : 0)) * 16777619u;
    char hhmm[12];
    nova::time_hhmm(hhmm, sizeof(hhmm));
    for (const char *p = hhmm; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ (uint32_t)power::source()) * 16777619u;
    h = (h ^ (uint32_t)(power::percent() + 1)) * 16777619u;
    h = (h ^ (uint32_t)(screens::stopwatch_running() ? 1 : 0)) * 16777619u;
    return h;
}

static void draw_toast(Canvas &c, const char *msg) {
    const int h = ui::ROWH + 3;
    const int y = ui::TOP + 3;
    const int x = 3, w = c.width() - 6;
    c.fill_rect(x, y, w, h, 0);                 // wipe the app out from under it
    c.rounded_rect(x, y, w, h, 1, false);
    c.text_fit(x + 4, y + 2, msg, 1, w - 8, false);
}

// --- the gallery ------------------------------------------------------------------
//
// The home style, and the inside of a folder. One large icon in the middle with
// its neighbours small on either side, and the label underneath.

// The row whose open() is running, for an OpenFn that serves several of them.
// Set immediately before the call and read immediately inside it; nothing keeps
// it beyond that, and nothing should.
static const App *g_chosen;

void chose(const App *a) { g_chosen = a; }
const App *chosen(void)  { return g_chosen; }

class Gallery : public Screen {
public:
    // The home screen and every folder had NO help at all, which also meant no
    // '?' in the status bar — so the first screen anybody sees was the one with
    // no way to ask what its two conventions mean. Both are non-obvious and
    // neither is spelled out anywhere else on the device: a struck-through icon
    // is an app whose hardware is missing (pressing it says which), and the pips
    // along the bottom are where you are in the ring.
    int help(const char **out, int max) const override {
        if (max < 5) return 0;
        out[0] = "Turn to move, SELECT opens.";
        out[1] = "A crossed-out icon needs";
        out[2] = "hardware — press it to see";
        out[3] = "which. The pips below are";
        out[4] = "your place in the ring.";
        return 5;
    }

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
        //
        // EASE-OUT, so the ring decelerates into place instead of moving at a
        // flat speed and stopping dead. slide_ still falls linearly in tick(),
        // so the animation lasts exactly SLIDE_MS and the frame count — and the
        // test that guards it — are unchanged; only the mapping from "time
        // remaining" to "distance remaining" is curved. slide_^2/256 is a
        // quadratic ease-out in 256ths: distance closes fast at first and
        // settles gently, with no float and one multiply a frame.
        const int eased = slide_ * slide_ / 256;
        const int off = dir_ * eased * SPACING / 256;

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
        // changes when the arriving icon crosses the halfway POSITION, not at
        // either end. This keys off `off`, which is the eased offset, so it
        // stays tied to what is on screen: the ease-out reaches halfway sooner
        // in time, and the label flips exactly then — when the new icon is
        // visibly the one in the middle — rather than on a separate clock.
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
            chose(a);
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
// Sized for the built-ins AND the installed apps.
//
// Filled by iterating to app_total(), so anything sized by APP_COUNT is a write
// past the end of a static into whatever bss sits next to it. The static_assert
// is what actually holds the line: this is out of reach of a host test — ASan
// does not redzone these, and the values written and read back agree either
// way, so a suite that overran by two pointers passed with 314 checks green. A
// number no test can check has to be checked by the compiler.
static const App *g_cat_items[CAT_COUNT][APP_COUNT + NAPP_MAX];
static_assert(sizeof(g_cat_items[0]) / sizeof(g_cat_items[0][0]) >= APP_COUNT + NAPP_MAX,
              "a category must hold every app, built-in and installed");
static int        g_cat_count[CAT_COUNT];
static const App *g_folder_items[CAT_COUNT];
static App        g_folder_apps[CAT_COUNT];
static int        g_folder_count;
static Category   g_open_cat;

static void open_category(void);
static void build_catalogue(void);

void refresh_apps(void) {
    if (napps::rescan_if_dirty()) build_catalogue();
}

static void build_catalogue(void) {
    user_apps_build();
    for (int i = 0; i < CAT_COUNT; i++) g_cat_count[i] = 0;
    for (unsigned i = 0; i < app_total(); i++) {
        const App *a = app_at(i);
        if (!a) break;
        g_cat_items[a->cat][g_cat_count[a->cat]++] = a;
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
        // An app installed since the last look. Home is re-entered on the way
        // out of everything, so this is where a new icon appears without
        // anybody rebooting — and it costs a directory listing only when
        // something has actually said it changed.
        //
        // ON THIS TASK, which is the point. A shell command that installed an
        // app marked napps dirty and left the rebuild here rather than doing it
        // underneath a Gallery that was drawing.
        refresh_apps();
        style_ = nova::reg(NOVA_KEY_PREFIX "HomeStyle", "folders");
        if (nova::ieq(style_, "gallery")) {
            static const App *flat[APP_COUNT + NAPP_MAX];
            static_assert(sizeof(flat) / sizeof(flat[0]) >= APP_COUNT + NAPP_MAX,
                          "the flat home must hold every app, built-in and installed");
            const unsigned n = app_total();
            for (unsigned i = 0; i < n; i++) flat[i] = app_at(i);
            set("", flat, (int)n);
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
// The lock's arming rule, in one place. THE LAST TEST IS THE ONE THAT MATTERS:
// a timeout can be set on a device with no code stored — Lock_Kind keeps its
// last value after `novad1 pin clear` — and arming on the clock alone would put
// up a screen that asks for a PIN nothing will ever accept.
bool lock_due(uint32_t idle_s) {
    int s = nova::reg_int(NOVA_KEY_PREFIX "LockSec", 0);
    if (s <= 0) return false;
    if (idle_s < (uint32_t)s) return false;
    return screens::lock_armed();
}

bool power_gesture_ok(void) { return !screens::lock_active(); }

uint32_t frame_dt(uint32_t now, uint32_t *last, bool had_input) {
    if (!last) return 0;
    if (had_input) *last = now;
    uint32_t dt = now - *last;
    *last = now;
    return dt;
}

#define NAP_ANIMATING 16      // 60 a second while something is moving
#define NAP_ACTIVE    33      // 30 a second just after a gesture
#define NAP_ACTIVE_MS 1200    // how long "just after a gesture" lasts — the loop
                              // stays at NAP_ACTIVE for this long after the last
                              // input, so a follow-up press or detent is seen
                              // promptly instead of waiting out a full idle nap
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
    // What somebody else installed, before the first catalogue is built from
    // it. Marked dirty first because `novad1 apps` may have scanned before the
    // runner started — a catalogue that is up to date is not the same as one
    // that has been built.
    napps::mark_dirty();
    refresh_apps();

    bool panel = display().begin();
    if (input().begin()) input().start();
    modules_scan();

    g_depth = 0;
    // The stack is gone, so anything that remembers a position in it has to go
    // with it. The lock is the only such thing today, and it remembers two.
    screens::lock_forget();
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

    // Did the update staged before the last restart land? Here rather than in
    // the Updates screen, because somebody who asked for an update wants to be
    // told how it went without having to go back and look — the same reason
    // update_report_boot() sits where it does in main.cpp.
    //
    // Costs one registry read on a start where nothing was staged, which is
    // every start but the one after an update.
    //
    // Only with a panel. The report is a notification, the notification queue
    // is only readable from this screen, and firing it on a device that has no
    // screen would spend the one telling on nobody.
    if (panel) screens::update_report_start();

    // No panel means no runner, so the claim goes back. Holding it would mean a
    // device that failed to find its screen once could never be told to look
    // again without a reboot — and looking again after fixing the wiring is
    // exactly what somebody does next.
    if (!panel) g_claimed = false;
    return panel;
}

// A headless driver — `novad1 tap` — injects gestures into the same queue the
// encoder feeds. But a gesture that lands on a dimmed or dark panel is spent on
// waking it — see the level check in drain_input — and never reaches the screen.
// On the glass that is exactly right: the first press you make picking the device
// up should say "show me", not "act on whatever the cursor happens to be on". A
// driver has none of that walking-up, and on the lock, which is dark PRECISELY
// because the device idled into it, the eaten press meant the pinpad never opened
// and the unlock could not be driven headlessly at all.
//
// So the driver ASKS, and the runner grants it at the top of the next drain, on
// its own task, where set_level's I2C write and every g_level write already live.
// The driver must never wake the panel itself: that would put the shell task
// inside the display driver while the runner is mid-show — the battery-icon
// freeze wearing a different coat. One writer (the driver) sets it, one
// reader-and-clearer (the runner) grants it, volatile, the same shape as the
// input ring two files over.
static volatile bool g_wake_req;

void request_wake(void) { g_wake_req = true; }

bool screen_active(void) { return g_level == LVL_ACTIVE; }

// The panel level on the idle clock, pulled out of run() so a test can put the
// panel to sleep the way the runner does rather than by imitating it.
void update_level(uint32_t now) {
    // Dim, then off, on their own timers. Both are contrast changes, so waking is
    // instant and nothing has to be re-initialised.
    uint32_t idle = (now - g_last_input) / 1000;
    int dim_s = nova::reg_int(NOVA_KEY_PREFIX "DimSec", 30);
    int off_s = nova::reg_int(NOVA_KEY_PREFIX "OffSec", 120);
    if      (off_s > 0 && idle >= (uint32_t)off_s) set_level(LVL_OFF);
    else if (dim_s > 0 && idle >= (uint32_t)dim_s) set_level(LVL_DIM);
}

// One pass over the input queue — the whole of the loop's inner while, so the
// harness can drive the runner through the SAME dispatch tap injects into rather
// than through a copy that omits the one branch that matters.
bool drain_input(void) {
    // A driver asked the panel to wake. Granted here, on the runner's task and
    // BEFORE the per-gesture wake-consume below would otherwise spend the driver's
    // first gesture on the wake. See g_wake_req.
    if (g_wake_req) {
        g_wake_req = false;
        set_level(LVL_ACTIVE);
        g_last_input = fw_millis();
    }

    bool had_input = false;
    bool turned = false;      // a rotation was consumed this frame
    Event e;
    while ((e = input().next()) != EV_NONE) {
        had_input = true;

        // ONE DETENT PER FRAME, and this is the whole difference between an
        // encoder that feels connected to the screen and one that does not.
        //
        // Draining the queue in a single frame means a fast spin moves the
        // selection six rows and redraws once: the cursor teleports and there is
        // no sense of having travelled. Taking one step per frame draws every
        // position it passes through, which is what reads as smooth. Buttons are
        // NOT rationed — a press waiting behind three detents would feel stuck.
        if (e == EV_ROT_CW || e == EV_ROT_CCW) {
            if (turned) { input().unget(e); break; }
            turned = true;
        }

        // A gesture while the screen is dark wakes it and is CONSUMED. On a device
        // you pick up out of a pocket, the first press is "show me", not "open
        // whatever the cursor happens to be on". A headless driver escapes this by
        // asking for the wake above, so its gesture arrives with the panel already
        // lit and is delivered rather than eaten.
        if (g_level != LVL_ACTIVE) { set_level(LVL_ACTIVE); g_last_input = fw_millis(); continue; }
        g_last_input = fw_millis();

        // Holding HOME opens the power menu from ANY screen, whatever state it is
        // in, so lock and shutdown are always one gesture away. It does not stack
        // a second copy on itself.
        // ...with one exception, and power_gesture_ok() is where it is written
        // down and where it is checked.
        if (e == EV_HOME_HOLD && power_gesture_ok()) {
            Screen *s = top();
            if (!s || !nova::ieq(s->title(), "Power")) {
                // Make room first. This is the one push reached by a gesture from
                // ANY screen, and it is the one that can find the pool already
                // full. STACK_MAX - 1, not STACK_MAX: the menu's own first row is
                // Controls, which is a second push.
                if (depth() >= STACK_MAX - 1) go_home();
                screens::open_power();
            }
            g_dirty = true;
            continue;
        }

        Screen *s = top();
        if (!s) continue;
        Action a = s->on_event(e);

        // HOME is a guaranteed way out, not a courtesy each screen has to
        // remember. `modal` is the deliberate opt-out and exists for one reason: a
        // lock that HOME escapes is not a lock.
        if (a == ACT_STAY && e == EV_HOME && !s->modal()) a = ACT_HOME;

        if      (a == ACT_BACK) pop();
        else if (a == ACT_HOME) go_home();
        g_dirty = true;
    }
    return had_input;
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
        //
        // The whole of the dispatch is drain_input(), so the one rule the harness
        // used to copy — and copied without the wake-consume — is now the runner's
        // own, driven the same way by both.
        bool had_input = drain_input();

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

        // The status bar changes on its own — the clock (so a timezone change
        // shows without a gesture), the power state, the stopwatch — and it is
        // the runner's, not the screen's, so nothing above marked the frame
        // dirty when it moved. Redraw when a drawn value changes. Only while
        // ACTIVE and not fullscreen: a dimmed panel means nobody is looking, and
        // a fullscreen screen has no bar.
        if (s && !s->fullscreen() && g_level == LVL_ACTIVE) {
            uint32_t sig = status_signature();
            if (sig != g_status_sig) { g_status_sig = sig; g_dirty = true; }
        }

        // Dim, then off, on their own timers — the runner's rule, now in the one
        // place both it and the harness call.
        update_level(now);
        uint32_t idle = (now - g_last_input) / 1000;

        // And the lock, on the same clock and read the same way. It is a third
        // tier rather than a thing hung off "screen off", because the two are
        // set independently and somebody may well want the panel to stay lit
        // and still want it locked — or the other way round.
        //
        // lock_engage refuses when it is already up and when the screen on top
        // is modal, so this can be an unguarded call every frame and both
        // reasons live in one place.
        if (lock_due(idle)) screens::lock_engage();

        // Notification toast. Clear an expired or dismissed one FIRST (so a press
        // that lands on the same frame a new one arrives does not eat it), then
        // pick up a new arrival. It is worth waking for — a banner behind a
        // dimmed panel is not seen — so come to full contrast and reset the idle
        // clock, which also holds the active frame rate for its first second.
        if (g_toast_until && (now >= g_toast_until || had_input)) { g_toast_until = 0; g_dirty = true; }
        if (notify::take_toast(g_toast_msg, sizeof(g_toast_msg))) {
            g_toast_until = now + 3000;
            set_level(LVL_ACTIVE);
            g_last_input = now;
            g_dirty = true;
        }

        if (g_dirty && s) {
            uint32_t t0 = fw_micros();
            g_canvas.clear(0);
            if (!s->fullscreen()) draw_status(g_canvas, s);
            s->draw(g_canvas);
            if (g_toast_until) draw_toast(g_canvas, g_toast_msg);   // last, so it is on top
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
        // A WINDOW, not a single frame. Holding the active rate for a short
        // while after the last input is what makes a SECOND detent or press
        // land promptly: without it the loop dropped to NAP_IDLE the very next
        // frame, so anything done a moment later waited up to 140 ms to be seen
        // even though the input task had already queued it. g_last_input is
        // already kept for the dim/off timers; this reuses it. The window is far
        // shorter than the dim timeout, so it cannot hold the panel awake.
        else if (now - g_last_input < NAP_ACTIVE_MS) nap = NAP_ACTIVE;
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
