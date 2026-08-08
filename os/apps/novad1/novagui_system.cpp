// Desc: The System screens — what the hardware is doing, and the settings that change it.
// File: novagui_system.cpp
#include "novagui_system.h"
#include "novagui.h"
#include "novamodtab.h"
#include "novaboard.h"
#include "novacore.h"
#include "display.h"
#include "novakeys.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// --- Hardware ---------------------------------------------------------------------
//
// The bring-up screen. Every module, its bus, and whether it answered — on the
// device, so somebody wiring a board can see the result without a serial cable
// in the other hand.

class HardwareScreen : public Screen {
public:
    const char *title(void) const override { return "Hardware"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT rescans everything.";
        out[1] = "A module with no pins set";
        out[2] = "reads 'not wired', not absent.";
        return 3;
    }

    void enter(void) override { sel_ = 0; top_ = 0; }

    void draw(Canvas &c) override {
        const int rows = ui::rows_for(c);
        const int n = (int)module_count();
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

        const Module *m = modules();
        for (int i = 0; i < rows; i++) {
            int idx = top_ + i;
            if (idx >= n) break;
            int y = ui::TOP + i * ui::ROWH;
            bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, c.width() - ui::SB_W - 1, ui::ROWH, 1, true);

            c.text_fit(3, y, m[idx].label, on ? 0 : 1, 54, false);
            // The state, right-aligned in its own column, so the eye runs down
            // one edge rather than hunting along each row.
            const char *state = mark(module_presence(m[idx]));
            int w = c.text_width(state, 1, false);
            c.text(c.width() - ui::SB_W - 3 - w, y, state, on ? 0 : 1);
        }
        c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP, top_, rows, n);
    }

    Action on_event(Event e) override {
        const int n = (int)module_count();
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + n - 1) % n; return ui::ACT_STAY; }
        if (e == EV_SELECT)  { push_detail(); return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD) { modules_scan(); gui::invalidate(); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    int sel_, top_;

    static const char *mark(Presence p) {
        switch (p) {
            case MOD_PRESENT: return "ok";
            case MOD_ABSENT:  return "--";
            case MOD_UNWIRED: return "no pin";
            default:          return "?";
        }
    }
    void push_detail(void);
};

// The one module, in full: the chip to search for, the bus, and every signal it
// wants with the GPIO it resolved to. This is the screen that answers "what do I
// connect where", which is otherwise a doc on another machine.
class ModuleScreen : public Screen {
public:
    static const Module *g_mod;

    const char *title(void) const override { return g_mod ? g_mod->label : "Module"; }

    void draw(Canvas &c) override {
        if (!g_mod) return;
        const Module &m = *g_mod;
        int y = ui::TOP;
        char line[32];

        snprintf(line, sizeof(line), "%s on %s", m.chip, bus_name(m.bus));
        c.text(0, y, line, 1);
        y += ui::ROWH;

        if (m.addr) {
            snprintf(line, sizeof(line), "address 0x%02x", m.addr);
            c.text(0, y, line, 1);
            y += ui::ROWH;
        }

        for (unsigned i = 0; i < m.npins && y < c.height() - ui::FH; i++) {
            int g = board::pin(m.pins[i]);
            if (g == board::PIN_NONE)
                snprintf(line, sizeof(line), "%-9s --", board::name(m.pins[i]));
            else
                snprintf(line, sizeof(line), "%-9s %d", board::name(m.pins[i]), g);
            c.text(0, y, line, 1);
            y += ui::ROWH;
        }
        if (m.bus == BUS_NONE) c.text(0, y, "on the board", 1);
    }
};

const Module *ModuleScreen::g_mod;

void HardwareScreen::push_detail(void) {
    ModuleScreen::g_mod = &modules()[sel_];
    gui::push<ModuleScreen>();
}

void open_hardware(void) { gui::push<HardwareScreen>(); }

// --- Display settings ----------------------------------------------------------------
//
// A settings group: labelled rows, turn to move, SELECT to change the one under
// the cursor. Every change takes effect IMMEDIATELY — brightness that only
// applies after a restart is a setting nobody can judge.

class DisplaySettings : public Screen {
public:
    const char *title(void) const override { return "Display"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "SELECT changes the row.";
        out[1] = "Changes are saved on BACK.";
        return 2;
    }

    void enter(void) override {
        sel_ = 0;
        bright_ = nova::reg_int(NOVA_KEY_PREFIX "Contrast", 128);
        dim_    = nova::reg_int(NOVA_KEY_PREFIX "DimSec", 30);
        off_    = nova::reg_int(NOVA_KEY_PREFIX "OffSec", 120);
        invert_ = nova::reg_bool(NOVA_KEY_PREFIX "Invert", false);
        dirty_  = false;
    }

    void leave(void) override {
        // One flash write for however many things changed, on the way out.
        if (!dirty_) return;
        nova::reg_set_int(NOVA_KEY_PREFIX "Contrast", bright_);
        nova::reg_set_int(NOVA_KEY_PREFIX "DimSec", dim_);
        nova::reg_set_int(NOVA_KEY_PREFIX "OffSec", off_);
        nova::reg_set_bool(NOVA_KEY_PREFIX "Invert", invert_);
        nova::reg_save();
    }

    void draw(Canvas &c) override {
        char v[20];
        for (int i = 0; i < ROWS; i++) {
            int y = ui::TOP + i * ui::ROWH;
            bool on = (i == sel_);
            if (on) c.rounded_rect(0, y - 1, c.width(), ui::ROWH, 1, true);
            c.text(3, y, kLabels[i], on ? 0 : 1);
            value(i, v, sizeof(v));
            int w = c.text_width(v, 1, false);
            c.text(c.width() - w - 3, y, v, on ? 0 : 1);
        }
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % ROWS; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + ROWS - 1) % ROWS; return ui::ACT_STAY; }
        if (e == EV_SELECT)  { bump(); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    static constexpr int ROWS = 5;
    static const char *const kLabels[ROWS];

    int  sel_, bright_, dim_, off_;
    bool invert_, dirty_;

    void value(int i, char *out, unsigned cap) const {
        switch (i) {
            case 0: snprintf(out, cap, "%d%%", bright_ * 100 / 255); break;
            case 1: if (dim_) snprintf(out, cap, "%ds", dim_); else nova::copy(out, cap, "never"); break;
            case 2: if (off_) snprintf(out, cap, "%ds", off_); else nova::copy(out, cap, "never"); break;
            case 3: nova::copy(out, cap, invert_ ? "on" : "off"); break;
            default: nova::copy(out, cap, display().kind_name()); break;
        }
    }

    void bump(void) {
        dirty_ = true;
        switch (sel_) {
            case 0:
                // Steps of a quarter, wrapping. A slider adjusted one unit at a
                // time out of 255 is 255 clicks of an encoder, which is not a
                // control, it is a punishment.
                bright_ += 64;
                if (bright_ > 255) bright_ = 15;
                display().contrast((uint8_t)bright_);
                break;
            case 1: dim_ = next_timeout(dim_); break;
            case 2: off_ = next_timeout(off_); break;
            case 3:
                invert_ = !invert_;
                display().invert(invert_);
                break;
            default: next_panel(); break;
        }
    }

    // Cycle the panel controller, and say so — the change cannot be shown,
    // because the screen it would be shown on is the one being reconfigured.
    //
    // Written to the registry immediately rather than on the way out like the
    // rest, since it only takes effect at the next start and a setting that
    // needs a restart must survive one to be worth anything.
    //
    // THE DEFAULT IS THE SSD1309, and the MicroPython suite's default of SH1106
    // is deliberately NOT carried over. Its own documentation said SSD1309, its
    // code said SH1106, and the disagreement cost four versions of a black
    // screen: an SSD1309 sent the SH1106 sequence never unlocks its command
    // interface and stays dark. Fidelity does not extend to a bug already paid
    // for. See the note at the top of display.h.
    void next_panel(void) {
        static const char *const kPanels[] = { "ssd1309", "sh1106", "ssd1306" };
        const char *now = nova::reg(NOVA_KEY_PREFIX "Display", "ssd1309");
        unsigned i = 0;
        for (; i < 3; i++) if (nova::ieq(now, kPanels[i])) break;
        const char *next = kPanels[(i + 1) % 3];
        nova::reg_set(NOVA_KEY_PREFIX "Display", next);
        nova::reg_save();
        ui::notice("Panel", "Set. The screen has to be restarted before it "
                            "changes: novad1 service restart.");
    }

    // The timeouts somebody actually wants, not every integer between them.
    static int next_timeout(int cur) {
        static const int kSteps[] = { 0, 15, 30, 60, 120, 300 };
        for (unsigned i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]) - 1; i++)
            if (cur == kSteps[i]) return kSteps[i + 1];
        return kSteps[0];
    }
};

// The MicroPython suite's labels, exactly. Somebody who has used that device
// should not have to relearn a word to change the same setting.
const char *const DisplaySettings::kLabels[DisplaySettings::ROWS] = {
    "Brightness", "Dim After", "Screen Off", "Invert", "Panel",
};

void open_display_settings(void) { gui::push<DisplaySettings>(); }

}  // namespace screens
}  // namespace nova
