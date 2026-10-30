// Desc: The small apps - see novagui_extras.h.
// File: novagui_extras.cpp
//
// Ported from the MicroPython kind:py apps (dice, counter, timer, baseconv, flashlight,
// totp). Each is a screen with one job and nothing competing with it, so each draws one
// big value and a line of what the controls do.
//
// WHAT CHANGED ON THE WAY:
//   * Dice rolls the die you choose (d4 to d100), and settles on the hardware RNG
//     instead of wherever its animation happened to stop.
//   * Counter counts on SELECT and clears on a HOLD. The MicroPython one cleared on a
//     plain press, which loses a count to a slip of the thumb.
//   * Torch is the SCREEN: the panel goes solid and the contrast goes to full. The
//     MicroPython one drove a WS2812, and this board has no LED on a pin of its own. If
//     one is wired it is driven too.
#include "novagui_extras.h"
#include "novagui.h"
#include "novagui_codes.h"
#include "novakeys.h"
#include "novacore.h"
#include "novaboard.h"
#include "novanotify.h"
#include "novatotp.h"
#include "display.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// --- Dice -------------------------------------------------------------------------------

static const uint8_t kDiceSides[] = { 4, 6, 8, 10, 12, 20, 100 };
#define DICE_KINDS 7

// What the last roll came to and what it was a roll of, outside the screen instance so a host
// test can read it back - the same reason g_gps_have and g_ir_seen live outside theirs.
static int g_dice_face;
static int g_dice_sides;

class DiceScreen : public Screen {
public:
    const char *title(void) const override { return "Dice"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Turn to pick the die.";
        out[1] = "SELECT rolls it.";
        return 2;
    }

    void enter(void) override { if (!started_) { started_ = true; kind_ = 1; face_ = 1; rolling_ = 0; } }

    bool tick(uint32_t dt) override {
        if (rolling_ <= 0) return false;
        rolling_ -= (int)dt;
        if (rolling_ <= 0) {
            rolling_ = 0;
            face_ = (int)(fw_random() % kDiceSides[kind_]) + 1;       // the RNG decides, not the animation
            g_dice_face = face_; g_dice_sides = kDiceSides[kind_];
        } else {
            // Faces flicker through while it rolls. Taken from the clock, not the RNG:
            // nobody is asking this part to be random.
            face_ = (int)((fw_millis() / 60u) % kDiceSides[kind_]) + 1;
        }
        return true;
    }

    bool animating(void) const override { return rolling_ > 0; }

    void draw(Canvas &c) override {
        char s[8];
        snprintf(s, sizeof(s), "d%d", (int)kDiceSides[kind_]);
        c.text(2, ui::TOP, s, 1);
        snprintf(s, sizeof(s), "%d", face_);
        c.text_centred(ui::TOP + ui::ROWH + 2, s, 1, 3);
        c.text(2, c.height() - ui::FH, rolling_ > 0 ? "rolling..." : "turn die  SEL roll", 1);
    }

    Action on_event(Event e) override {
        if (rolling_ > 0) return e == EV_BACK || e == EV_HOME ? Screen::on_event(e) : ui::ACT_STAY;
        if (e == EV_ROT_CW)  { kind_ = (kind_ + 1) % DICE_KINDS; face_ = 1; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { kind_ = (kind_ + DICE_KINDS - 1) % DICE_KINDS; face_ = 1; return ui::ACT_STAY; }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) { rolling_ = 450; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    bool started_;
    int  kind_, face_, rolling_;
};

// --- Counter ----------------------------------------------------------------------------

static int g_counter_n;             // for the host test, as with the dice

class CounterScreen : public Screen {
public:
    const char *title(void) const override { return "Counter"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT counts up.";
        out[1] = "Turn to count up or down.";
        out[2] = "Hold SELECT to clear it.";
        return 3;
    }

    void enter(void) override { n_ = 0; g_counter_n = 0; }

    void draw(Canvas &c) override {
        g_counter_n = n_;
        c.text(2, ui::TOP, "Tally", 1);
        char s[12];
        snprintf(s, sizeof(s), "%d", n_);
        c.text_centred(ui::TOP + ui::ROWH + 2, s, 1, strlen(s) <= 4 ? 3 : 2);     // shrink so big counts still fit
        c.text(2, c.height() - ui::FH, "turn +/-  SEL +1", 1);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW || e == EV_SELECT) { if (n_ < 99999) n_++; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW)                   { if (n_ > -9999) n_--; return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD)               { n_ = 0; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    int n_;
};

// --- Base converter ---------------------------------------------------------------------

static const uint16_t kBaseSteps[] = { 1, 16, 256, 4096 };
static int g_base_n;                // for the host test

class BaseConvScreen : public Screen {
public:
    const char *title(void) const override { return "Base Conv"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Turn changes the value.";
        out[1] = "SELECT changes the step.";
        return 2;
    }

    void draw(Canvas &c) override {
        g_base_n = n_;
        char row[24];
        int y = ui::TOP;
        snprintf(row, sizeof(row), "DEC  %u", (unsigned)n_);
        c.text(2, y, row, 1); y += ui::ROWH;
        snprintf(row, sizeof(row), "HEX  %04X", (unsigned)n_);
        c.text(2, y, row, 1); y += ui::ROWH;
        // Sixteen bits as 4 groups of 4, so it fits the panel and can be read.
        char bits[24];
        unsigned k = 0;
        for (int i = 15; i >= 0; i--) {
            bits[k++] = (char)('0' + ((n_ >> i) & 1));
            if (i && (i & 3) == 0) bits[k++] = ' ';
        }
        bits[k] = 0;
        c.text(2, y, "BIN", 1); y += ui::ROWH;
        c.text(2, y, bits, 1);
        snprintf(row, sizeof(row), "turn=%u  SEL step", (unsigned)kBaseSteps[si_]);
        c.text(2, c.height() - ui::FH, row, 1);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { n_ = (uint16_t)(n_ + kBaseSteps[si_]); return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { n_ = (uint16_t)(n_ - kBaseSteps[si_]); return ui::ACT_STAY; }
        if (e == EV_SELECT)  { si_ = (si_ + 1) % 4; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    uint16_t n_;
    int      si_;
};

// --- Timer ------------------------------------------------------------------------------
//
// Counts down while it is open. It does not run in the background the way the stopwatch
// does: what it would have to do at zero, beep from a screen that is not there, is the
// part that has no owner.

#define TIMER_STEP 15
#define TIMER_MAX  3600

static void beep(unsigned hz, unsigned ms) {
    int pin = board::pin(board::PIN_BUZZER);
    if (pin == board::PIN_NONE) return;
    fw_pwm_init((unsigned)pin, hz);
    fw_pwm_duty((unsigned)pin, 500);
    fw_busy_wait_us(ms * 1000u);
    fw_pwm_stop((unsigned)pin);
}

class TimerScreen : public Screen {
public:
    const char *title(void) const override { return "Timer"; }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "Turn sets the time.";
        out[1] = "SELECT starts, pauses, resumes.";
        out[2] = "BACK stops it.";
        out[3] = "It counts while this is open.";
        return 4;
    }

    void enter(void) override { if (!started_) { started_ = true; total_ = 60; left_ = 0; run_ = false; done_ = false; acc_ = 0; } }

    bool tick(uint32_t dt) override {
        if (!run_) return false;
        acc_ += dt;
        if (acc_ < 1000) return false;
        acc_ -= 1000;
        if (--left_ <= 0) {
            left_ = 0; run_ = false; done_ = true;
            beep(2200, 150);
            nova::notify::post("Timer done");
            nova::notify::alert();
        }
        return true;
    }

    bool animating(void) const override { return run_; }

    void draw(Canvas &c) override {
        int val = (run_ || left_ || done_) ? left_ : total_;
        char s[8];
        snprintf(s, sizeof(s), "%02d:%02d", val / 60, val % 60);
        c.text_centred(ui::TOP + 4, s, 1, 3);
        const char *foot = done_ ? "DONE   any key" : run_ ? "SEL pause  BACK stop"
                         : left_ ? "SEL resume BACK stop" : "turn set  SEL start";
        c.text(2, c.height() - ui::FH, foot, 1);
    }

    Action on_event(Event e) override {
        if (done_) {                                    // any key clears DONE
            done_ = false; left_ = 0;
            return (e == EV_BACK || e == EV_HOME) ? Screen::on_event(e) : ui::ACT_STAY;
        }
        if (e == EV_ROT_CW && !run_ && !left_)  { total_ = total_ + TIMER_STEP > TIMER_MAX ? TIMER_MAX : total_ + TIMER_STEP; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW && !run_ && !left_) { total_ = total_ - TIMER_STEP < TIMER_STEP ? TIMER_STEP : total_ - TIMER_STEP; return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            if (run_) run_ = false;                     // pause
            else { if (!left_) left_ = total_; run_ = true; acc_ = 0; }
            return ui::ACT_STAY;
        }
        if (e == EV_BACK && (run_ || left_)) { run_ = false; left_ = 0; return ui::ACT_STAY; }   // stop, back to set
        return Screen::on_event(e);
    }

private:
    bool     started_, run_, done_;
    int      total_, left_;
    uint32_t acc_;
};

// --- Torch ------------------------------------------------------------------------------

class TorchScreen : public Screen {
public:
    const char *title(void) const override { return "Torch"; }
    bool fullscreen(void) const override { return on_; }       // all of it, status bar too, when it is a torch

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT puts the screen on.";
        out[1] = "Turn sets how bright.";
        out[2] = "BACK turns it off.";
        return 3;
    }

    void enter(void) override { if (!started_) { started_ = true; pct_ = 100; on_ = false; } }
    void leave(void) override { off(); }

    void draw(Canvas &c) override {
        if (on_) { c.fill_rect(0, 0, c.width(), c.height(), 1); return; }
        c.text(2, ui::TOP, "Flashlight", 1);
        const int bx = 6, by = ui::TOP + ui::ROWH, bw = c.width() - 12;
        c.rect(bx, by, bw, 11, 1);
        c.fill_rect(bx + 2, by + 2, (bw - 4) * pct_ / 100, 7, 1);
        char s[16];
        snprintf(s, sizeof(s), "%d%%   off", pct_);
        c.text(2, by + 14, s, 1);
        c.text(2, c.height() - ui::FH, "turn level SEL light", 1);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { pct_ = pct_ + 10 > 100 ? 100 : pct_ + 10; apply(); return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { pct_ = pct_ - 10 < 10 ? 10 : pct_ - 10; apply(); return ui::ACT_STAY; }
        if (e == EV_SELECT) { on_ = !on_; apply(); gui::invalidate(); return ui::ACT_STAY; }
        if (e == EV_BACK && on_) { off(); gui::invalidate(); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    bool started_, on_;
    int  pct_;

    // The screen's own brightness, and an LED if somebody wired one. The panel's contrast
    // is the only dimmer it has, and the runner is told to keep it lit while the torch is on.
    void apply(void) {
        gui::hold_lit(on_);
        display().contrast(on_ ? (uint8_t)(pct_ * 255 / 100) : gui::contrast_setting());
        int led = board::pin(board::PIN_LED);
        if (led != board::PIN_NONE) {
            if (on_) { fw_pwm_init((unsigned)led, 1000); fw_pwm_duty((unsigned)led, (unsigned)(pct_ * 10)); }
            else     fw_pwm_stop((unsigned)led);
        }
    }
    void off(void) { if (on_) { on_ = false; apply(); } else gui::hold_lit(false); }
};

// --- One-time codes ---------------------------------------------------------------------
//
// Each account is a .txt under /nova/codes/totp holding a base32 secret, optionally as
// "Label: SECRET". The SECRETS ARE PLAIN FILES: nothing here encrypts them, and anyone
// who can read the filesystem can read them. The clock has to be right, and the RTC holds
// UTC, which is what the codes are computed from.

static const codes::Kind kTotp = { NOVA_CODES "/totp", ".txt", "TOTP" };

static char g_totp_new_name[CODES_NAME_MAX];
static char g_totp_shown[8];        // the code on the screen now, for the host test
static bool g_totp_add_req;

// The secret in a file: the first line that is not blank or a comment, from after its last
// colon when it has one.
static bool totp_read_secret(const char *name, char *out, unsigned cap) {
    char path[96];
    codes::path_for(kTotp, name, path, sizeof(path));
    static char buf[160];
    uint32_t n = fw_file_read(path, buf, sizeof(buf) - 1);
    buf[n] = 0;
    for (char *p = buf; *p; ) {
        char *e = strchr(p, '\n');
        char *line_end = e ? e : p + strlen(p);
        char *s = p;
        while (s < line_end && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        char *t = line_end;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r')) t--;
        if (s < t && *s != '#') {
            char *colon = nullptr;
            for (char *q = s; q < t; q++) if (*q == ':') colon = q;
            if (colon) { s = colon + 1; while (s < t && *s == ' ') s++; }
            unsigned k = 0;
            while (s < t && k + 1 < cap) out[k++] = *s++;
            out[k] = 0;
            return k > 0;
        }
        if (!e) break;
        p = e + 1;
    }
    out[0] = 0;
    return false;
}

static void totp_secret_typed(void *, const char *text) {
    char path[96], clean[CODES_NAME_MAX];
    if (!codes::clean_name(g_totp_new_name, clean, sizeof(clean))) { ui::notice("TOTP", "That is not a name."); return; }
    uint8_t probe[1];
    if (!totp::b32_decode(text, probe, 1)) { ui::notice("TOTP", "That is not a base32 secret."); return; }
    codes::ensure_dir(kTotp);
    char unique[CODES_NAME_MAX];
    codes::unique_name(kTotp, clean, unique, sizeof(unique));
    codes::path_for(kTotp, unique, path, sizeof(path));
    // Written upper-case with no spaces, which is how an authenticator app prints it.
    char norm[96];
    unsigned k = 0;
    for (const char *p = text; *p && k + 2 < sizeof(norm); p++) {
        if (*p == ' ' || *p == '-') continue;
        norm[k++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    }
    norm[k++] = '\n';
    if (!fw_file_write(path, norm, k)) { ui::notice("TOTP", "It could not be saved."); return; }
    g_totp_add_req = true;
}

static void totp_name_typed(void *, const char *text) {
    nova::copy(g_totp_new_name, sizeof(g_totp_new_name), text);
    ui::keyboard("Secret", "", false, totp_secret_typed, nullptr, nullptr);
}

class TotpScreen : public Screen {
public:
    const char *title(void) const override { return "TOTP"; }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "Turn picks an account.";
        out[1] = "SELECT adds one; hold deletes.";
        out[2] = "Needs the clock set (NTP).";
        out[3] = "Secrets are plain files.";
        return 4;
    }

    void enter(void) override {
        codes::scan(kTotp);
        last_ = 0xffffffffu;
        refresh();
    }

    bool tick(uint32_t) override {
        bool changed = false;
        if (g_totp_add_req) { g_totp_add_req = false; codes::scan(kTotp); last_ = 0xffffffffu; changed = true; }
        FwTime t;
        uint32_t now = fw_time_get(&t) ? totp::unix_from_civil(t.year, t.month, t.day, t.hour, t.minute, t.second) : 0;
        if (now != last_) { last_ = now; refresh(); changed = true; }
        return changed;
    }

    void draw(Canvas &c) override {
        if (!codes::list().n) {
            c.text_fit(2, ui::TOP, "No accounts yet.", 1, c.width() - 4, false);
            c.text_fit(2, ui::TOP + ui::ROWH, "SELECT adds one:", 1, c.width() - 4, false);
            c.text_fit(2, ui::TOP + 2 * ui::ROWH, "a name, then its", 1, c.width() - 4, false);
            c.text_fit(2, ui::TOP + 3 * ui::ROWH, "base32 secret.", 1, c.width() - 4, false);
            c.text_fit(2, ui::TOP + 4 * ui::ROWH, "Or put name.txt in", 1, c.width() - 4, false);
            c.text_fit(2, ui::TOP + 5 * ui::ROWH, "/nova/codes/totp", 1, c.width() - 4, false);
            return;
        }
        c.text_fit(2, ui::TOP, codes::selected(), 1, c.width() - 4, false);
        if (!clock_ok_) { c.text_centred(ui::TOP + ui::ROWH + 6, "clock not set", 1); return; }
        char disp[10];
        if (code_[0]) snprintf(disp, sizeof(disp), "%.3s %s", code_, code_ + 3); else nova::copy(disp, sizeof(disp), "------");
        c.text_centred(ui::TOP + ui::ROWH + 2, disp, 1, 2);
        const int bw = c.width() - 8, by = ui::TOP + 3 * ui::ROWH;
        c.rect(2, by, bw, 5, 1);
        c.fill_rect(3, by + 1, (bw - 2) * left_ / 30, 3, 1);
        char s[20];
        snprintf(s, sizeof(s), "%d/%d  %ds", codes::list().sel + 1, codes::list().n, left_);
        c.text(2, c.height() - ui::FH, s, 1);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { codes::move(1);  refresh(); return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { codes::move(-1); refresh(); return ui::ACT_STAY; }
        if (e == EV_SELECT)  { ui::keyboard("Account", "", false, totp_name_typed, nullptr, nullptr); return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD) {
            const char *n = codes::selected();
            if (n) codes::ask_delete(kTotp, n, delete_done, this);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    uint32_t last_;
    bool     clock_ok_;
    int      left_;
    char     code_[8];

    static void delete_done(void *) { g_totp_add_req = true; }     // rescan on the next tick

    void refresh(void) {
        code_[0] = 0; left_ = 0; g_totp_shown[0] = 0;
        FwTime t;
        clock_ok_ = fw_time_get(&t) != 0;
        const char *name = codes::selected();
        if (!clock_ok_ || !name) return;
        uint32_t now = totp::unix_from_civil(t.year, t.month, t.day, t.hour, t.minute, t.second);
        left_ = 30 - (int)(now % 30);
        char secret[96];
        if (totp_read_secret(name, secret, sizeof(secret))) {
            if (!totp::code_b32(secret, now, code_)) nova::copy(code_, sizeof(code_), "ERR");
        }
        nova::copy(g_totp_shown, sizeof(g_totp_shown), code_);
    }
};

// --- the App table's entry points ----------------------------------------------------------

void open_dice(void)     { gui::push<DiceScreen>(); }
void open_counter(void)  { gui::push<CounterScreen>(); }
void open_timer(void)    { gui::push<TimerScreen>(); }
void open_baseconv(void) { gui::push<BaseConvScreen>(); }
void open_torch(void)    { gui::push<TorchScreen>(); }
void open_totp(void)     { gui::push<TotpScreen>(); }

}  // namespace screens
}  // namespace nova
