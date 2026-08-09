// Desc: The Tools and Sensors screens that need no wired module.
// File: novagui_tools.cpp
#include "novagui_tools.h"
#include "novagui.h"
#include "novacore.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// --- Resources -------------------------------------------------------------------
//
// What the device is actually doing right now. The screen somebody opens when
// something feels wrong, so every row is a live reading rather than a
// configured value.

class ResourcesScreen : public Screen {
public:
    const char *title(void) const override { return "Resources"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Largest is the biggest single";
        out[1] = "block free, not the total.";
        return 2;
    }

    void enter(void) override { last_ = 0; sample(); }

    bool tick(uint32_t dt) override {
        // Once a second. Every one of these readings crosses the ABI, and a
        // panel that redraws a number changing in its last digit sixty times a
        // second costs a lot to say very little.
        acc_ += dt;
        if (acc_ < 1000) return false;
        acc_ = 0;
        sample();
        return true;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;
        char line[32];

        // The network, and what it is called. An IP address is the answer to
        // "is it really on", where a bar chart is only the answer to "does it
        // think it is".
        if (linked_) {
            snprintf(line, sizeof(line), "%s", ssid_);
            c.text(0, y, "Link", 1);
            c.text_fit(30, y, line, 1, c.width() - 30, false);
            y += ui::ROWH;
            c.text(0, y, "IP", 1);
            c.text(30, y, ip_, 1);
        } else {
            c.text(0, y, "Link", 1);
            c.text(30, y, "not connected", 1);
            y += ui::ROWH;
        }
        y += ui::ROWH;

        // Free and largest, together, because they are different numbers and
        // the gap between them is the interesting part. On the MicroPython
        // build "90 KB free" and "cannot open a socket" were routinely both
        // true; this build does not fragment the same way, and showing both is
        // still how anyone would notice if it started.
        snprintf(line, sizeof(line), "%u KB  max %u KB",
                 fw_heap_free() / 1024u, fw_heap_largest() / 1024u);
        c.text(0, y, "RAM", 1);
        c.text(30, y, line, 1);
        y += ui::ROWH;

        snprintf(line, sizeof(line), "%u%%  %u fps", cpu_, gui::perf().frames);
        c.text(0, y, "Load", 1);
        c.text(30, y, line, 1);
        y += ui::ROWH;

        // The bar is the one thing here that is a picture rather than a number,
        // because "how full" is a proportion and a proportion reads faster as a
        // length than as a percentage.
        c.text(0, y, "Draw", 1);
        snprintf(line, sizeof(line), "%u us  %u/8", gui::perf().draw_us, gui::perf().pages);
        c.text(30, y, line, 1);
    }

private:
    uint32_t acc_, last_, cpu_;
    bool     linked_;
    char     ssid_[34];
    char     ip_[18];

    void sample(void) {
        linked_ = fw_net_connected() != 0;
        ssid_[0] = ip_[0] = 0;
        if (linked_) {
            fw_net_ssid(ssid_, sizeof(ssid_));
            fw_net_ip(ip_, sizeof(ip_));
        }
        cpu_ = fw_cpu_percent();
    }
};

void open_resources(void) { gui::push<ResourcesScreen>(); }

// --- Clock -----------------------------------------------------------------------
//
// The big-value archetype: one thing, large, and nothing competing with it.

// THE STOPWATCH RUNS IN THE BACKGROUND. Its state lives here, not in the screen,
// so starting it and walking away leaves it counting — it is still going when
// you come back, and the status bar shows a dot while it runs.
//
// The elapsed time is COMPUTED from a start timestamp, never accumulated per
// frame. That is what makes "the background" free: nothing has to tick while the
// screen is closed, and no hundredths are lost — fw_millis has been running the
// whole time, so elapsed is just now minus when it started, plus whatever was
// banked before the current run.
static bool     g_sw_running;
static uint32_t g_sw_start;     // fw_millis() at the last start
static uint32_t g_sw_accum;     // milliseconds banked before the current run

static uint32_t sw_elapsed(void) {
    return g_sw_accum + (g_sw_running ? fw_millis() - g_sw_start : 0);
}

// For the status bar, which draws a dot while this is true — see novagui.cpp.
bool stopwatch_running(void) { return g_sw_running; }

class ClockScreen : public Screen {
public:
    const char *title(void) const override { return "Clock"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Turn for the stopwatch.";
        out[1] = "SELECT starts and stops it;";
        out[2] = "it keeps running if you leave.";
        return 3;
    }

    void enter(void) override {
        // Open on whichever face is worth seeing: the stopwatch if it is running
        // or holds a time, the clock otherwise. The stopwatch itself is NOT reset
        // — it is running in the background and this is only a window onto it.
        mode_ = (g_sw_running || g_sw_accum) ? 1 : 0;
        acc_ = 0; last_ = 99;
    }

    bool tick(uint32_t dt) override {
        if (mode_ == 1) {
            if (!g_sw_running) return false;
            // Ten times a second: the hundredths are what a stopwatch is FOR,
            // and one that ticks once a second is a clock.
            acc_ += dt;
            if (acc_ < 100) return false;
            acc_ = 0;
            return true;
        }
        // A clock redraws when the MINUTE changes, not every frame and not
        // every second — nothing on this screen moves in between.
        FwTime t;
        if (!fw_time_get(&t)) return false;
        if ((uint32_t)t.minute == last_) return false;
        last_ = (uint32_t)t.minute;
        return true;
    }

    void draw(Canvas &c) override {
        char big[16];
        if (mode_ == 0) {
            nova::time_hhmm(big, sizeof(big));
            c.text_centred(ui::TOP + 8, big, 1, 2, false);
            char date[24];
            nova::time_date(date, sizeof(date));
            c.text_centred(ui::TOP + 30, date, 1);
            if (!fw_time_get(nullptr))
                c.text_centred(c.height() - ui::FH, "clock not set", 1);
        } else {
            const uint32_t e = sw_elapsed();
            uint32_t cs = (e / 10) % 100;
            uint32_t s  = (e / 1000) % 60;
            uint32_t m  = e / 60000;
            snprintf(big, sizeof(big), "%02u:%02u", (unsigned)m, (unsigned)s);
            c.text_centred(ui::TOP + 8, big, 1, 2, false);
            char sub[16];
            snprintf(sub, sizeof(sub), ".%02u", (unsigned)cs);
            c.text_centred(ui::TOP + 30, sub, 1);
            c.text_centred(c.height() - ui::FH, g_sw_running ? "running" : "stopped", 1);
        }
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW || e == EV_ROT_CCW) {
            mode_ ^= 1;
            last_ = 99;
            return ui::ACT_STAY;
        }
        if (e == EV_SELECT && mode_ == 1) {
            // Start banks nothing new; stop banks the run just finished. Elapsed
            // stays correct across the toggle because it is always accum plus the
            // live run.
            if (g_sw_running) { g_sw_accum += fw_millis() - g_sw_start; g_sw_running = false; }
            else              { g_sw_start = fw_millis(); g_sw_running = true; }
            return ui::ACT_STAY;
        }
        // Held SELECT clears a stopped watch. Clearing a RUNNING one is not
        // offered: it is the gesture people reach for by accident, and losing a
        // timing you were part-way through is not recoverable.
        if (e == EV_SELECT_HOLD && mode_ == 1 && !g_sw_running) { g_sw_accum = 0; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

    bool animating(void) const override { return mode_ == 1 && g_sw_running; }

private:
    int      mode_;
    uint32_t acc_, last_;
};

void open_clock(void) { gui::push<ClockScreen>(); }

}  // namespace screens
}  // namespace nova
