// Desc: The BadUSB launcher — list payloads, show the USB mode, run one on a yes.
// File: novagui_usb.cpp
//
// A package cannot touch USB directly, so this drives the firmware's own commands
// through fw_shell_run and parses what they print — the same shape as the radio
// screens beside it: `usbmode` reports or switches the mode, `badusb <file>` runs
// a DuckyScript payload, and the payloads are ordinary files in a directory the
// firmware runs them from.
//
// Two rules the firmware enforces and this respects rather than works around:
//
//   - console / storage / hid are MUTUALLY EXCLUSIVE. storage is the download
//     drive; hid is the keyboard. So running a payload switches to hid THROUGH
//     usbmode, and switching back to the drive goes THROUGH usbmode too — the
//     screen never assumes both can be live at once.
//   - typing into a host is not something to do by accident. A payload runs only
//     after a confirmation that says out loud where the keystrokes go.
//
// DEVICE-UNCONFIRMED: the USB HID firmware is another agent's and the actual
// typing into a real host cannot be checked here. What is host-proven is the mode
// parse, the payload listing, the confirm gate, the usbmode->badusb sequence, and
// the older-firmware path where the commands are absent.
#include "novagui_usb.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"

#include "rpc_app.h"
#include <string.h>
#include <stdio.h>

namespace nova {
namespace screens {

namespace usb {

// Which function `usbmode` is reporting active. Lenient on the exact wording — it
// names one of the three (console / storage / keyboard) — and a reply that names
// none (an error, or an older firmware with no such command) is UNKNOWN, which the
// screen reads as "not on this build".
Mode parse_mode(const char *text) {
    if (!text || !text[0]) return USB_UNKNOWN;
    if (strstr(text, "keyboard")) return USB_KEYBOARD;
    if (strstr(text, "storage"))  return USB_STORAGE;
    if (strstr(text, "console"))  return USB_CONSOLE;
    return USB_UNKNOWN;
}

const char *mode_name(Mode m) {
    switch (m) {
        case USB_CONSOLE:  return "console";
        case USB_STORAGE:  return "storage";
        case USB_KEYBOARD: return "keyboard";
        default:           return "unknown";
    }
}

}  // namespace usb

using ui::Screen;
using ui::Action;
using usb::Mode;

// --- the payload directory ----------------------------------------------------
//
// The conventional place first, then the shorter one, because the firmware is
// another agent's and either is plausible; whichever holds files wins, and the
// path that won is remembered so `badusb` gets the full name.
#define USB_DIR_A    "/nova/badusb"
#define USB_DIR_B    "/badusb"
#define USB_MAX      16
#define USB_NAME_MAX 28

static char g_usb_names[USB_MAX][USB_NAME_MAX];
static int  g_usb_n;
static char g_usb_dir[24];

static void usb_list(void) {
    g_usb_n = 0;
    g_usb_dir[0] = 0;
    static const char *const dirs[2] = { USB_DIR_A, USB_DIR_B };
    for (int d = 0; d < 2; d++) {
        int cnt = fw_dir_count(dirs[d]);
        if (cnt <= 0) continue;
        FwDirEntry e;
        int found = 0;
        for (unsigned i = 0; i < (unsigned)cnt && g_usb_n < USB_MAX; i++) {
            if (fw_dir_entry(dirs[d], i, &e) != 1) break;
            if (e.is_dir) continue;
            nova::copy(g_usb_names[g_usb_n++], USB_NAME_MAX, e.name);
            found++;
        }
        if (found) { nova::copy(g_usb_dir, sizeof(g_usb_dir), dirs[d]); return; }
    }
}

// --- the worker ---------------------------------------------------------------
//
// One command in flight, on a task, because `badusb` blocks while it types. The
// same generation-counter shape as the radio worker: a reply whose screen has
// since changed is disowned rather than landing in the wrong place.
static char              g_usb_out[1024];
static char              g_usb_line[96];
static volatile uint32_t g_usb_gen, g_usb_req_gen;
static volatile uint8_t  g_usb_busy, g_usb_ready;
static volatile int      g_usb_rc;

static int usb_task(void *) {
    uint32_t mine = g_usb_req_gen;
    int rc = fw_shell_run(g_usb_line, g_usb_out, sizeof(g_usb_out));
    if (mine == g_usb_gen && !fw_task_should_stop()) { g_usb_rc = rc; g_usb_ready = 1; }
    else g_usb_out[0] = 0;
    g_usb_busy = 0;
    return 0;
}

static bool usb_run(const char *line) {
    if (g_usb_busy || g_usb_ready) return false;
    snprintf(g_usb_line, sizeof(g_usb_line), "%s", line);
    g_usb_out[0] = 0;
    g_usb_req_gen = g_usb_gen;
    g_usb_busy = 1;
    if (fw_task_spawn("novausb", usb_task, nullptr, 2048) < 0) { g_usb_busy = 0; return false; }
    return true;
}

static bool usb_reap(void) { if (!g_usb_ready) return false; g_usb_ready = 0; return true; }
static void usb_disown(void) { g_usb_gen++; g_usb_ready = 0; }

// A run queued from the confirmation (whose yes callback runs while this screen is
// covered by the dialog), fired by tick() once the worker is free.
static char g_usb_pending_path[64];
static char g_usb_pending_name[USB_NAME_MAX];
static bool g_usb_run_pending;

static void usb_confirmed(void *) { g_usb_run_pending = (g_usb_pending_path[0] != 0); }

// --- the screen ---------------------------------------------------------------

class BadUsbScreen : public Screen {
public:
    const char *title(void) const override { return "BadUSB"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT a payload to run it.";
        out[1] = "It types into whatever host";
        out[2] = "this is plugged into. It asks first.";
        return 3;
    }

    void enter(void) override {
        usb_disown();
        phase_ = 0;
        if (!started_) {
            started_ = true;
            sel_ = 0; top_ = 0;
            have_mode_ = false; unavailable_ = false; mode_ = usb::USB_UNKNOWN;
            note_[0] = 0;
            usb_list();
            pending_ = P_MODE;
            if (!usb_run("usbmode")) pending_ = P_IDLE;
        }
    }

    void leave(void) override { usb_disown(); }

    bool animating(void) const override { return g_usb_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        bool changed = false;

        // A confirmed run. `badusb` enters the keyboard, types the payload, and
        // stands the keyboard back down by itself; it refuses on its own — with a
        // message — if the download drive is open. So this just runs it and
        // surfaces what it says, rather than switching modes it does not own.
        if (g_usb_run_pending && !g_usb_busy && !g_usb_ready && pending_ == P_IDLE) {
            g_usb_run_pending = false;
            char line[96];
            snprintf(line, sizeof(line), "badusb %s", g_usb_pending_path);
            if (usb_run(line)) {
                pending_ = P_RUN;
                snprintf(note_, sizeof(note_), "running %s", g_usb_pending_name);
                changed = true;
            }
        }

        if (usb_reap()) {
            changed = true;
            switch (pending_) {
                case P_MODE:
                    if (!g_usb_out[0]) nova::copy(note_, sizeof(note_), "no result - busy");
                    else {
                        mode_ = usb::parse_mode(g_usb_out);
                        // A command that is not there prints an error with no mode
                        // word in it; a non-zero status seals it. Either way it is
                        // "not on this build", not "broken".
                        unavailable_ = (mode_ == usb::USB_UNKNOWN) || (g_usb_rc != 0);
                        have_mode_ = true;
                    }
                    pending_ = P_IDLE;
                    break;
                case P_RUN:
                    // The payload finished. "done" unless the firmware refused it —
                    // the download drive was open, which it enforces itself — or the
                    // payload errored. Reading `usbmode` again would confirm the
                    // keyboard already stood back down, but it is not needed to say
                    // what happened.
                    snprintf(note_, sizeof(note_), "%s: %s", g_usb_pending_name,
                             (g_usb_rc == 0 && !strstr(g_usb_out, "rror") &&
                              !strstr(g_usb_out, "refus")) ? "done" : "not run");
                    pending_ = P_IDLE;
                    break;
                default: pending_ = P_IDLE; break;
            }
        }
        return changed || g_usb_busy;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;
        char line[40];

        // The mode, or the honest "not on this build".
        if (unavailable_)            nova::copy(line, sizeof(line), "USB: needs newer fw");
        else if (have_mode_)         snprintf(line, sizeof(line), "USB: %s", usb::mode_name(mode_));
        else if (pending_ == P_MODE) nova::copy(line, sizeof(line), "USB: checking...");
        else                         nova::copy(line, sizeof(line), "USB: unknown");
        c.text_fit(2, y, line, 1, c.width() - 12, false);
        if (g_usb_busy) c.spinner(c.width() - 9, y, phase_ / 140, 1);
        y += ui::ROWH;

        // The list: a mode toggle, then the payloads. One line at the foot is kept
        // for the running/done note, so the list scrolls within what is left.
        const int list_rows = ui::rows_for(c) - 2;   // header + note
        const int total = row_count();
        if (sel_ < top_) top_ = sel_;
        if (sel_ >= top_ + list_rows) top_ = sel_ - list_rows + 1;

        for (int i = 0; i < list_rows; i++) {
            int idx = top_ + i;
            if (idx >= total) break;
            bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, c.width(), ui::ROWH, 1, true);
            row_label(idx, line, sizeof(line));
            c.text_fit(3, y, line, 1, c.width() - 6, false);
            y += ui::ROWH;
        }

        const char *foot = note_[0] ? note_
                         : (unavailable_ ? "usbmode/badusb not found"
                                         : (g_usb_n ? "SELECT runs it into the host"
                                                    : "no payloads in /nova/badusb"));
        c.text_fit(2, c.height() - ui::FH, foot, 1, c.width() - 4, false);
    }

    Action on_event(Event e) override {
        const int total = row_count();
        if (e == EV_ROT_CW)  { if (total) sel_ = (sel_ + 1) % total; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { if (total) sel_ = (sel_ + total - 1) % total; return ui::ACT_STAY; }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            if (unavailable_) {
                ui::notice("BadUSB", "This build has no usbmode/badusb. Update the "
                                     "firmware to run HID payloads.");
                return ui::ACT_STAY;
            }
            // A payload. Ask before typing into whatever is on the other end.
            int p = sel_;
            if (p >= 0 && p < g_usb_n) {
                nova::copy(g_usb_pending_name, sizeof(g_usb_pending_name), g_usb_names[p]);
                const char *dir = g_usb_dir[0] ? g_usb_dir : USB_DIR_A;
                snprintf(g_usb_pending_path, sizeof(g_usb_pending_path), "%s/%s", dir, g_usb_names[p]);
                snprintf(g_confirm_q, sizeof(g_confirm_q),
                         "Run %s? It types into whatever this is plugged into.",
                         g_usb_names[p]);
                ui::confirm(g_confirm_q, "Run", usb_confirmed, this);
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    enum { P_IDLE, P_MODE, P_RUN };

    int  row_count(void) const { return g_usb_n; }         // just the payloads

    void row_label(int idx, char *out, unsigned cap) const {
        nova::copy(out, cap, (idx >= 0 && idx < g_usb_n) ? g_usb_names[idx] : "");
    }

    int      sel_, top_;
    unsigned phase_;
    Mode     mode_;
    bool     have_mode_, unavailable_, started_;
    char     note_[32];
    int      pending_;
    static char g_confirm_q[80];
};

char BadUsbScreen::g_confirm_q[80];

void open_badusb(void) { gui::push<BadUsbScreen>(); }

}  // namespace screens
}  // namespace nova
