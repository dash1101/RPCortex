// Desc: The settings tree — the five groups, from the home layout to the serial number.
// File: novagui_settings.cpp
#include "novagui_settings.h"
#include "novagui_wifi.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"
#include "novaboard.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// --- shared ---------------------------------------------------------------------
//
// Some of what this tree changes belongs to the OS rather than to this package,
// and the two do not spell a boolean the same way.
//
// The OS compares its own settings with strcmp against "true" — WiFi.Auto in
// net.cpp and every row of the shell's `settings` panel — so a package writing
// "on" for one of those stores a value the OS reads as FALSE, silently and
// permanently. nova::reg_set_bool writes "on"/"off", which is right for this
// suite's keys and wrong for theirs, so the two have separate spellings here.
static void set_os_bool(const char *key, bool on) {
    nova::reg_set(key, on ? "true" : "false");
}

// "never", "45s", "5m". A timeout of zero is not zero seconds, it is off, and a
// row reading "0s" says the opposite of what it means.
static void say_secs(char *out, unsigned cap, int s) {
    if (s <= 0)      nova::copy(out, cap, "never");
    else if (s < 60) snprintf(out, cap, "%ds", s);
    else if (s % 60) snprintf(out, cap, "%dm%ds", s / 60, s % 60);
    else             snprintf(out, cap, "%dm", s / 60);
}

// Step to the next value in a small set, wrapping. Past the end goes back to the
// start rather than sticking, so a row can always be got back to where it was
// without a second control to go the other way.
static int next_in(const int *steps, int n, int cur) {
    for (int i = 0; i < n - 1; i++) if (cur == steps[i]) return steps[i + 1];
    return steps[0];
}

// --- the settings list ------------------------------------------------------------
//
// Every group below is this list with a different set of rows: a label on the
// left, what it is set to on the right, turn to move and SELECT to change the
// one under the cursor.
//
// One implementation rather than five. Five hand-written lists drift — one grows
// a scrollbar, one truncates the value instead of the label, one forgets to wrap
// at the bottom — and a settings tree where each screen behaves slightly
// differently is the thing that makes a device feel assembled rather than made.
//
// Nothing here is pure virtual, for the reason novaui.h gives: a pure virtual
// generates a reference to __cxa_pure_virtual, which the firmware does not
// export. A default that does nothing costs one branch.
class SettingsList : public Screen {
public:
    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "SELECT changes the row.";
        out[1] = "Changes are saved on BACK.";
        return 2;
    }

    // Put the cursor at the top. Called by the open_* function AFTER the push,
    // because push_commit runs enter() before it hands the screen back and
    // enter() must not zero the cursor — see the note there.
    void begin(void) { sel_ = 0; top_ = 0; }

    void enter(void) override {
        dirty_ = false;
        load();
        // The pool slot arrives holding whatever the last screen left in it, so
        // the cursor is clamped rather than trusted. It is NOT reset: enter()
        // also runs when a sub-screen pops back to here, and putting somebody
        // back on row zero every time they set a PIN or picked an app is the
        // difference between a tree that remembers where you were and one that
        // makes you find your place again.
        const int n = count();
        if (sel_ < 0 || sel_ >= n) sel_ = 0;
        if (top_ < 0 || top_ >= n) top_ = 0;
    }

    void leave(void) override {
        // One flash write for however many rows changed, on the way out. A
        // settings screen that saved on every turn of the knob would write to
        // flash six times to change six values, and each write is about ten
        // milliseconds of a device that is meant to feel immediate.
        if (!dirty_) return;
        store();
        nova::reg_save();
        dirty_ = false;
    }

    void draw(Canvas &c) override {
        const int n    = count();
        const int rows = ui::rows_for(c);
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

        // The scrollbar lane is only taken when there is something to scroll, so
        // a short group gets the full width for its labels.
        const bool scrolls = n > rows;
        const int  right   = scrolls ? c.width() - (ui::SB_W + 1) : c.width();

        char v[24];
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= n) break;
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);

            v[0] = 0;
            value(idx, v, sizeof(v));
            const int w = v[0] ? c.text_width(v, 1, false) : 0;
            // The LABEL is what gets cut when the two do not both fit. A
            // truncated value is a setting whose state cannot be read, which
            // defeats the row; a truncated label is still recognisable from the
            // first few characters and from where it sits in the group.
            c.text_fit(3, y, label(idx), on ? 0 : 1, right - w - 8, false);
            if (v[0]) c.text(right - w - 2, y, v, on ? 0 : 1);
        }

        if (scrolls)
            c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP, top_, rows, n);
    }

    Action on_event(Event e) override {
        const int n = count();
        if (n <= 0) return Screen::on_event(e);
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + n - 1) % n; return ui::ACT_STAY; }
        if (e == EV_SELECT)  return activate(sel_);
        return Screen::on_event(e);
    }

protected:
    int  sel_, top_;
    bool dirty_;

    virtual int count(void) const { return 0; }
    virtual const char *label(int i) const { (void)i; return ""; }
    virtual void value(int i, char *out, unsigned cap) const { (void)i; if (cap) out[0] = 0; }
    virtual Action activate(int i) { (void)i; return ui::ACT_STAY; }

    // Read the registry into members on the way in, write them back on the way
    // out. Reading once rather than per row per frame keeps a still screen at
    // no supervisor calls at all, and a call is about 296 cycles.
    virtual void load(void) {}
    virtual void store(void) {}
};

// --- the app picker ----------------------------------------------------------------
//
// Which apps the home screen shows, and which of them are pinned in front of the
// folders. ONE screen for both: the list is the same list and the gesture is the
// same gesture, and the only difference is which way round a tick reads.

// The list being edited, held outside the screen. A screen slot is 384 bytes and
// this is a quarter of one, there is only ever one picker open, and the buffer
// has to outlive nothing at all.
static char g_picker_csv[NOVA_VAL_MAX];

class AppPicker : public SettingsList {
public:
    // Set before the push. A screen is placement-constructed with no arguments,
    // so what it is editing arrives the way ModuleScreen's module does.
    static const char *g_key;
    static const char *g_name;
    // Apps.NovaD1_Hidden stores what is NOT shown, so its tick is the absence of
    // an entry; Apps.NovaD1_Favorites stores what IS pinned. Same list, opposite
    // sense, one flag rather than two screens.
    static bool        g_tick_is_absence;

    const char *title(void) const override { return g_name; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "SELECT ticks and unticks.";
        out[1] = "Changes are saved on BACK.";
        return 2;
    }

    void draw(Canvas &c) override {
        const int n    = count();
        const int rows = ui::rows_for(c);
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
        const int right = c.width() - (ui::SB_W + 1);

        const gui::App *a = gui::apps();
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= n) break;
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
            // A box with a cross in it rather than a lit row, because the row
            // being lit already means "the cursor is here" and one mark cannot
            // carry two meanings on a panel with no second colour.
            c.text(3, y, ticked(idx) ? "[x]" : "[ ]", on ? 0 : 1);
            c.text_fit(3 + 4 * ui::ADV, y, a[idx].label, on ? 0 : 1, right - 4 * ui::ADV - 6, false);
        }
        c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP, top_, rows, n);
    }

protected:
    int count(void) const override { return (int)gui::app_count(); }

    void load(void) override {
        nova::copy(g_picker_csv, sizeof(g_picker_csv), nova::reg(g_key, ""));
    }
    void store(void) override { nova::reg_set(g_key, g_picker_csv); }

    Action activate(int i) override {
        const char *key = gui::apps()[i].key;
        const bool listed = nova::csv_has(g_picker_csv, key);
        const bool ok = listed ? nova::csv_remove(g_picker_csv, sizeof(g_picker_csv), key)
                               : nova::csv_add(g_picker_csv, sizeof(g_picker_csv), key);
        if (!ok) {
            // csv_add refuses rather than overflowing, and the only way it can
            // refuse here is that the list is full. Said out loud, because a
            // registry value stops at 96 characters and a tick that silently
            // does not take is the worst possible way to find that out.
            ui::notice(g_name, "That is as many as this setting will hold. "
                               "A saved value stops at 96 characters.");
            return ui::ACT_STAY;
        }
        dirty_ = true;
        return ui::ACT_STAY;
    }

private:
    bool ticked(int i) const {
        const bool listed = nova::csv_has(g_picker_csv, gui::apps()[i].key);
        return g_tick_is_absence ? !listed : listed;
    }
};

const char *AppPicker::g_key;
const char *AppPicker::g_name;
bool        AppPicker::g_tick_is_absence;

static void open_picker(const char *key, const char *name, bool tick_is_absence) {
    AppPicker::g_key = key;
    AppPicker::g_name = name;
    AppPicker::g_tick_is_absence = tick_is_absence;
    AppPicker *s = gui::push<AppPicker>();
    if (s) s->begin();
}

// --- Home --------------------------------------------------------------------------
//
// What the home screen is: how it is laid out, which apps are on it, which of
// those are pinned in front, what the device calls itself, and how it interrupts.

static const char *const kStyles[] = { "folders", "gallery", "menu" };
constexpr int STYLE_COUNT = 3;

static void home_name_typed(void *, const char *text) {
    // An empty name is a mistake rather than an edit. The status bar falls back
    // to this key at home, so a device named "" has a blank title bar and looks
    // broken rather than looking renamed.
    if (!text || !text[0]) return;
    nova::reg_set(NOVA_KEY_PREFIX "Name", text);
    nova::reg_save();
}

class HomeSettings : public SettingsList {
public:
    const char *title(void) const override { return "Home"; }

protected:
    enum { R_STYLE = 0, R_APPS, R_FAVS, R_NAME, R_NOTIFY, R_LED, R_BUZZ, R_COUNT };

    int count(void) const override { return R_COUNT; }
    const char *label(int i) const override { return kLabels[i]; }

    void load(void) override {
        const char *s = nova::reg(NOVA_KEY_PREFIX "HomeStyle", kStyles[0]);
        style_ = 0;
        for (int i = 0; i < STYLE_COUNT; i++) if (nova::ieq(s, kStyles[i])) style_ = i;
        notify_ = nova::reg_bool(NOVA_KEY_PREFIX "Notify", true);
        led_    = nova::reg_bool(NOVA_KEY_PREFIX "Notify_LED", true);
        buzz_   = nova::reg_bool(NOVA_KEY_PREFIX "Notify_Haptic", true);
        nova::ellipsize(name_, sizeof(name_), nova::reg(NOVA_KEY_PREFIX "Name", "Nova D1"), 11);
    }

    void store(void) override {
        nova::reg_set(NOVA_KEY_PREFIX "HomeStyle", kStyles[style_]);
        nova::reg_set_bool(NOVA_KEY_PREFIX "Notify", notify_);
        nova::reg_set_bool(NOVA_KEY_PREFIX "Notify_LED", led_);
        nova::reg_set_bool(NOVA_KEY_PREFIX "Notify_Haptic", buzz_);
    }

    void value(int i, char *out, unsigned cap) const override {
        switch (i) {
            case R_STYLE:  nova::copy(out, cap, kStyles[style_]); break;
            case R_APPS:
            case R_FAVS:   nova::copy(out, cap, ">"); break;
            case R_NAME:   nova::copy(out, cap, name_); break;
            case R_NOTIFY: nova::copy(out, cap, notify_ ? "on" : "off"); break;
            case R_LED:    nova::copy(out, cap, led_ ? "on" : "off"); break;
            default:       nova::copy(out, cap, buzz_ ? "on" : "off"); break;
        }
    }

    Action activate(int i) override {
        switch (i) {
            case R_STYLE:
                // The home screen re-reads this in its own enter(), which runs
                // when the last screen above it pops — so the new layout is
                // there by the time anyone gets back to it, with nothing here
                // having to reach into the runner to say so.
                style_ = (style_ + 1) % STYLE_COUNT;
                dirty_ = true;
                break;
            case R_APPS:
                open_picker(NOVA_KEY_PREFIX "Hidden", "Apps", true);
                break;
            case R_FAVS:
                open_picker(NOVA_KEY_PREFIX "Favorites", "Favourites", false);
                break;
            case R_NAME:
                // Pre-filled, so changing one character of a name does not mean
                // typing the whole thing again on a keyboard driven by one knob.
                ui::keyboard("Device name", nova::reg(NOVA_KEY_PREFIX "Name", "Nova D1"),
                             false, home_name_typed, nullptr, nullptr);
                break;
            case R_NOTIFY: notify_ = !notify_; dirty_ = true; break;
            case R_LED:    led_    = !led_;    dirty_ = true; break;
            default:       buzz_   = !buzz_;   dirty_ = true; break;
        }
        return ui::ACT_STAY;
    }

private:
    static const char *const kLabels[R_COUNT];

    int  style_;
    bool notify_, led_, buzz_;
    char name_[14];
};

// The MicroPython suite's own words where it had the row. "Apps" was its
// "Manage Apps", and somebody who has used that device should not have to
// relearn a label to reach the same screen.
//
// Favourites, Name and Buzz are ours: v1 had no row for any of them. Buzz is
// the haptic and is NOT v1's "Chime", which was a tone from the buzzer — a
// different feature, and one with nothing behind it here.
const char *const HomeSettings::kLabels[HomeSettings::R_COUNT] = {
    "Layout", "Manage Apps", "Favourites", "Name", "Notify", "Alert LED", "Buzz",
};

void open_set_home(void) {
    HomeSettings *s = gui::push<HomeSettings>();
    if (s) s->begin();
}

// --- Network -----------------------------------------------------------------------
//
// The radio, and the two things that happen over it without being asked.

class NetworkSettings : public SettingsList {
public:
    const char *title(void) const override { return "Network"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Radio off is the kill switch:";
        out[1] = "it is the OS latch, so it also";
        out[2] = "stops the shell joining.";
        return 3;
    }

protected:
    enum { R_LINK = 0, R_SAVED, R_RADIO, R_AUTO, R_COUNT };

    int count(void) const override { return R_COUNT; }
    const char *label(int i) const override { return kLabels[i]; }

    void load(void) override {
        // The radio latch is the OS's, not this package's, so it is read back
        // rather than remembered — `radio off` from the shell moves it too.
        locked_ = nova::reg_bool("System.RadioLock", false);
        auto_   = nova::reg_bool("WiFi.Auto", false);
        linked_ = fw_net_connected() != 0;
        ssid_[0] = 0;
        if (linked_) fw_net_ssid(ssid_, sizeof(ssid_));
    }

    void store(void) override { set_os_bool("WiFi.Auto", auto_); }

    void value(int i, char *out, unsigned cap) const override {
        switch (i) {
            case R_LINK: {
                if (!linked_) { nova::copy(out, cap, "not joined"); break; }
                nova::ellipsize(out, cap, ssid_, 12);
                break;
            }
            case R_SAVED: {
                // Counted here rather than cached, because the WiFi screen can
                // add or forget one while this screen is underneath it.
                int n = 0;
                for (int i = 0; i < 8; i++) {
                    char key[24];
                    snprintf(key, sizeof(key), "WiFi.Net%d_SSID", i);
                    if (nova::reg(key, "")[0]) n++;
                }
                snprintf(out, cap, "%d", n);
                break;
            }
            case R_RADIO: nova::copy(out, cap, locked_ ? "off" : "on"); break;
            case R_AUTO:  nova::copy(out, cap, auto_ ? "on" : "off"); break;
            default:      nova::copy(out, cap, ">"); break;
        }
    }

    Action activate(int i) override {
        char out[128];
        switch (i) {
            case R_LINK:
                // The WiFi screen owns joining, scanning and the signal; this
                // row is the way in rather than a second copy of any of it.
                screens::open_wifi();
                break;
            case R_SAVED:
                screens::open_networks();
                break;
            case R_RADIO:
                // THROUGH THE SHELL, NOT THE REGISTRY. The latch has to be set
                // by the OS command because that command also takes the radio
                // DOWN, and it is checked inside net.cpp where every caller
                // passes — including the background join at boot. The
                // MicroPython suite kept its own flag instead, and `wifi scan`
                // from the shell walked straight past incognito and brought the
                // radio back up.
                if (fw_shell_run(locked_ ? "radio on" : "radio off", out, sizeof(out)) != 0) {
                    ui::notice("Radio", out[0] ? out
                               : "Refused. Locking the radios needs an admin session.");
                } else {
                    locked_ = !locked_;
                }
                break;
            default:
                auto_ = !auto_;
                dirty_ = true;
                break;
        }
        return ui::ACT_STAY;
    }

private:
    static const char *const kLabels[R_COUNT];

    bool locked_, auto_, linked_;
    char ssid_[FW_NET_SSID_MAX];
};

// v1 called the first row WiFi and it opened the whole WiFi screen; "Link" was
// ours and said less. Saved is a second row because the saved-networks manager
// is its own screen here.
//
// Sync Clock has MOVED, to System -> Clock. v1 had it under Network because NTP
// needs the network, which split the clock across two groups: the zone and the
// time in one place, the thing that sets them in another. It is a clock action
// that happens to need a radio.
//
// Not carried over: LoRa radio, LoRa MHz and Web Panel. There is no SX1276
// driver and no web panel in this build, and a row that changes nothing teaches
// somebody the device is broken.
const char *const NetworkSettings::kLabels[NetworkSettings::R_COUNT] = {
    "WiFi", "Saved", "Radio", "Join at boot",
};

void open_set_network(void) {
    NetworkSettings *s = gui::push<NetworkSettings>();
    if (s) s->begin();
}

// --- Security ----------------------------------------------------------------------
//
// The lock on the screen, and what the radios are giving away.

static const char *const kKinds[] = { "none", "pin", "password" };
constexpr int KIND_COUNT = 3;

// The timeouts somebody would actually choose, not every integer between them.
static const int kLockSteps[] = { 0, 30, 60, 300, 900 };
constexpr int LOCK_STEPS = 5;

// What the lock ACTUALLY is right now.
//
// Lock type stores a PREFERENCE and keeps its last value after the code is
// cleared, so the row went on saying "PIN" for a device with no PIN on it. This
// reports None when nothing is stored, because that is the fact that matters.
static const char *lock_state(void) {
    const char *kind = nova::reg(NOVA_KEY_PREFIX "Lock_Kind", "none");
    if (nova::ieq(kind, "password"))
        return nova::reg(NOVA_KEY_PREFIX "Pass", "")[0] ? "Password" : "None";
    if (nova::ieq(kind, "pin"))
        return nova::reg(NOVA_KEY_PREFIX "PIN", "")[0] ? "PIN" : "None";
    return "None";
}

static void pin_typed(void *, const char *pin) {
    nova::reg_set(NOVA_KEY_PREFIX "PIN", pin);
    nova::reg_set(NOVA_KEY_PREFIX "Lock_Kind", "pin");
    nova::reg_save();
}

static void pass_typed(void *, const char *text) {
    nova::reg_set(NOVA_KEY_PREFIX "Pass", text ? text : "");
    // An emptied password is a lock removed, not a lock with nothing in it.
    // Leaving the type on "password" would give a device that says it is locked
    // and has nothing to check, which is the state lock_state() exists to catch.
    nova::reg_set(NOVA_KEY_PREFIX "Lock_Kind", (text && text[0]) ? "password" : "none");
    nova::reg_save();
}

class SecuritySettings : public SettingsList {
public:
    const char *title(void) const override { return "Security"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "The lock guards the screen,";
        out[1] = "not the shell or the files.";
        out[2] = "'novad1 pin clear' undoes it.";
        return 3;
    }

protected:
    enum { R_STATE = 0, R_KIND, R_CODE, R_AUTO, R_GHOST, R_COUNT };

    int count(void) const override { return R_COUNT; }
    const char *label(int i) const override { return kLabels[i]; }

    void load(void) override {
        const char *k = nova::reg(NOVA_KEY_PREFIX "Lock_Kind", kKinds[0]);
        kind_ = 0;
        for (int i = 0; i < KIND_COUNT; i++) if (nova::ieq(k, kKinds[i])) kind_ = i;
        secs_   = nova::reg_int(NOVA_KEY_PREFIX "LockSec", 0);
        locked_ = nova::reg_bool("System.RadioLock", false);
    }

    void store(void) override {
        nova::reg_set(NOVA_KEY_PREFIX "Lock_Kind", kKinds[kind_]);
        nova::reg_set_int(NOVA_KEY_PREFIX "LockSec", secs_);
    }

    void value(int i, char *out, unsigned cap) const override {
        switch (i) {
            case R_STATE: nova::copy(out, cap, lock_state()); break;
            case R_KIND:  nova::copy(out, cap, kKinds[kind_]); break;
            case R_CODE:  nova::copy(out, cap, ">"); break;
            case R_AUTO:  say_secs(out, cap, secs_); break;
            default:      nova::copy(out, cap, locked_ ? "on" : "off"); break;
        }
    }

    Action activate(int i) override {
        switch (i) {
            case R_STATE:
                break;                              // a readout
            case R_KIND:
                kind_ = (kind_ + 1) % KIND_COUNT;
                dirty_ = true;
                // Turning the type to None CLEARS BOTH CODES.
                //
                // None is the third type rather than a separate button, so
                // choosing it has to do what a "clear lock" row would. Both go,
                // not only the one matching the previous type: leaving the other
                // behind means flipping the type back re-locks a device somebody
                // had just unlocked, with a code they may not remember setting.
                if (nova::ieq(kKinds[kind_], "none")) {
                    nova::reg_set(NOVA_KEY_PREFIX "PIN", "");
                    nova::reg_set(NOVA_KEY_PREFIX "Pass", "");
                }
                break;
            case R_CODE:
                // One row rather than a Set-PIN row and a Set-password row, only
                // one of which ever applies. With the type on None there is no
                // code to set, so it says so instead of opening an editor whose
                // result would be thrown away.
                if (nova::ieq(kKinds[kind_], "none"))
                    ui::notice("Lock", "The lock type is None. Set it to PIN or "
                                       "Password to choose a code.");
                else if (nova::ieq(kKinds[kind_], "password"))
                    ui::keyboard("Set password", "", true, pass_typed, nullptr, nullptr);
                else
                    ui::pinpad("Set PIN", pin_typed, nullptr, nullptr);
                break;
            case R_AUTO: {
                ui::Slider *s = gui::push<ui::Slider>();
                if (s) s->set_stops("Auto-lock", secs_, kLockSteps, LOCK_STEPS,
                                    ui::SL_SECONDS, on_auto_slide, this);
                break;
            }
            default:
                // A readout, deliberately. This is the same OS latch the Network
                // screen's Radio row owns, and two rows driving one switch is how
                // a settings tree ends up disagreeing with itself. What belongs
                // here is the answer to "is anything transmitting", which is the
                // question Security is being asked.
                break;
        }
        return ui::ACT_STAY;
    }

    // The auto-lock timeout, scrubbed on the slider and saved with the rest on
    // the way out. Routed through the owning screen so store() still writes it.
    static void on_auto_slide(void *ctx, int v) {
        SecuritySettings *s = (SecuritySettings *)ctx;
        s->secs_ = v;
        s->dirty_ = true;
    }

private:
    static const char *const kLabels[R_COUNT];

    int  kind_, secs_;
    bool locked_;
};

// v1's words, including its hyphen. Its fifth row was a "Privacy" sub-group
// holding Incognito, Random ID and a "What leaks" screen; two of those three
// have nothing behind them here — there is no MAC API on the ABI and no privacy
// screen — and a sub-menu with one row in it is worse than the row. So Incognito
// stays where it can be reached in one press, and the group comes back when
// there is something to put in it.
const char *const SecuritySettings::kLabels[SecuritySettings::R_COUNT] = {
    "Lock", "Lock type", "Change code", "Auto-Lock", "Incognito",
};

void open_set_security(void) {
    SecuritySettings *s = gui::push<SecuritySettings>();
    if (s) s->begin();
}

// --- Timezone ----------------------------------------------------------------------
//
// Its own screen rather than a cycling row.
//
// There are twenty-seven civil offsets between -12 and +14, and reaching one of
// them by pressing SELECT twenty-seven times is not a control. Turn adjusts.
//
// It writes System.TZ_Offset, which is the key the OS's own `date` and its
// settings panel read — so setting it here moves every clock on the device at
// once rather than only this suite's.
class TimezoneScreen : public Screen {
public:
    const char *title(void) const override { return "Timezone"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Whole hours only. Nothing here";
        out[1] = "reads a half-hour offset.";
        return 2;
    }

    void enter(void) override {
        off_ = nova::reg_int("System.TZ_Offset", 0);
        if (off_ < LO || off_ > HI) off_ = 0;
        dirty_ = false;
    }

    void leave(void) override {
        if (!dirty_) return;
        nova::reg_set_int("System.TZ_Offset", off_);
        nova::reg_save();
    }

    void draw(Canvas &c) override {
        c.text(2, ui::TOP, "hours from UTC", 1);
        // Sized for an int rather than for two digits. The value is clamped to
        // -12..14 on the way in, but the compiler cannot see that from here and
        // a buffer sized to what the range allows is one somebody widens the
        // range past later.
        char lbl[20];
        snprintf(lbl, sizeof(lbl), "UTC%c%d", off_ < 0 ? '-' : '+', off_ < 0 ? -off_ : off_);
        c.text_centred(c.height() / 2 - ui::FH / 2, lbl, 1, 2, false);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { off_ = off_ < HI ? off_ + 1 : LO; dirty_ = true; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { off_ = off_ > LO ? off_ - 1 : HI; dirty_ = true; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    static constexpr int LO = -12;
    static constexpr int HI = 14;

    int  off_;
    bool dirty_;
};

// --- System ------------------------------------------------------------------------
//
// How fast the machine runs, what time it thinks it is, and the two buttons
// nobody should be able to press by accident.

// The speeds worth offering, rather than every megahertz between them. `pulse`
// accepts 48 to 400; these are the four a person would actually pick, and one of
// them is each chip's own default so the row always has somewhere familiar to
// land.
static const int kMhz[] = { 100, 125, 150, 200 };
constexpr int MHZ_STEPS = 4;

// An answer from something that has already run, put up on the NEXT frame.
//
// AN ACTION BEHIND ui::confirm MUST NOT PUSH A SCREEN. confirm calls the
// callback and then returns ACT_BACK, and the pop that follows takes the TOP
// screen off — which, if the callback pushed one, is the new screen rather than
// the question. A notice raised that way appears and vanishes in the same frame
// while the question it answered stays up. So the message is parked here and the
// screen underneath puts it on once the question has gone.
//
// Everything else in this file pushes straight from activate(), which is safe:
// activate returns ACT_STAY and nothing pops behind it.
static const char *g_said_title;
static char        g_said[112];

static void say_later(const char *title, const char *msg) {
    g_said_title = title;
    nova::copy(g_said, sizeof(g_said), msg && msg[0] ? msg : "Done.");
}

static void do_reboot(void *) { fw_reboot(); }

static void do_reset(void *) {
    // regreset, NOT factoryreset.
    //
    // factoryreset asks its question on the serial console, through a read that
    // spins until somebody types there. Run from the screen that is the UI task
    // waiting forever on a terminal nobody is looking at — the panel freezes and
    // the only way out is a keyboard. regreset takes --yes and returns.
    //
    // It clears the registry and leaves files and accounts alone, which is the
    // honest scope of a settings reset and is what this row claims to be.
    char out[112];
    if (fw_shell_run("regreset --yes", out, sizeof(out)) != 0) {
        say_later("Reset", out[0] ? out : "Refused. Resetting the registry needs an admin session.");
        return;
    }
    fw_reboot();
}

// Step the CPU clock, and make it stick past a restart.
//
// A free function rather than a member because the Clock group is the only
// caller now and System used to be — a static tucked inside one of two screens
// that both want it is how a second copy gets written.
static void bump_cpu_clock(void) {
    const unsigned now = fw_clock_hz() / 1000000u;
    int want = kMhz[0];
    for (int i = 0; i < MHZ_STEPS; i++)
        if ((unsigned)kMhz[i] == now) { want = kMhz[(i + 1) % MHZ_STEPS]; break; }

    char line[24], out[112];
    snprintf(line, sizeof(line), "pulse set %d", want);
    if (fw_shell_run(line, out, sizeof(out)) != 0) {
        ui::notice("CPU clock", out[0] ? out
                   : "Refused. Changing the clock needs an admin session.");
        return;
    }
    // And keep it after a restart. Setting the clock without setting the boot
    // clock is a setting that undoes itself overnight, which reads as the row
    // never having worked.
    snprintf(line, sizeof(line), "pulse boot %d", want);
    fw_shell_run(line, nullptr, 0);
}

// --- Clock --------------------------------------------------------------------------
//
// Time, zone, speed and the sync, on one screen.
//
// v1 kept a Clock group for the same reason and put Sync Clock under Network
// instead, because NTP needs the network. That split the clock across two
// groups: Set Time and Timezone in one place, the thing that sets them from a
// server in another. Sync is a clock action that happens to need a radio, so it
// is here with the rest of them.

// --- Set Time ---------------------------------------------------------------------
//
// The clock, set by hand. v1 had this and v2 dropped it, which left a handheld
// with only Sync Clock — and Sync needs a network. A Nova D1 out in a field with
// no WiFi had no way to set its clock at all, which is exactly where it matters
// most.
//
// It drives the shell's own `date set`, so there is one setter on the device and
// this is a face for it, not a second path into the RTC. The fields are edited
// one at a time with the encoder; SELECT steps to the next and commits on the
// last, the way the PIN screen walks its digits.
class SetTimeScreen : public Screen {
public:
    void begin(void) {
        FwTime t;
        if (fw_time_get(&t) == 1 && t.year >= 2000) {
            v_[F_Y] = t.year; v_[F_MO] = t.month; v_[F_D] = t.day;
            v_[F_H] = t.hour; v_[F_MI] = t.minute;
        } else {
            // The clock is running but not right (kboot seeds a placeholder), so
            // start somewhere sane rather than at 1970.
            v_[F_Y] = 2026; v_[F_MO] = 1; v_[F_D] = 1; v_[F_H] = 0; v_[F_MI] = 0;
        }
        f_ = F_Y;
    }

    const char *title(void) const override { return "Set Time"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Turn to change the field,";
        out[1] = "SELECT for the next. The last";
        out[2] = "one sets the clock.";
        return 3;
    }

    void draw(Canvas &c) override {
        char date[16], time[8];
        snprintf(date, sizeof(date), "%04d-%02d-%02d", v_[F_Y], v_[F_MO], v_[F_D]);
        snprintf(time, sizeof(time), "%02d:%02d", v_[F_H], v_[F_MI]);

        const int y1 = ui::TOP + 4;
        c.text_centred(y1, date, 1, 2, false);
        c.text_centred(y1 + 18, time, 1, 2, false);

        // Underline the field being edited, positioned under its digits. The
        // date row is "YYYY-MM-DD" at scale 2; each glyph is FW*2+2 wide. The
        // field starts are counted in glyphs from the left of the centred text.
        underline_field(c, date, time, y1, y1 + 18);

        c.text_centred(c.height() - ui::FH, f_ == F_LAST ? "SELECT sets it" : "SELECT: next", 1);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { bump(+1); return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { bump(-1); return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            if (f_ < F_LAST) { f_ = f_ + 1; return ui::ACT_STAY; }
            return commit();
        }
        if (e == EV_SELECT_HOLD) return commit();
        return Screen::on_event(e);
    }

private:
    enum { F_Y = 0, F_MO, F_D, F_H, F_MI, F_COUNT, F_LAST = F_MI };
    int v_[F_COUNT];
    int f_;

    static int days_in_month(int y, int m) {
        static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
        return (m >= 1 && m <= 12) ? d[m - 1] : 31;
    }

    void bump(int dir) {
        switch (f_) {
            case F_Y:  v_[F_Y] = wrap(v_[F_Y] + dir, 2020, 2099); break;
            case F_MO: v_[F_MO] = wrap(v_[F_MO] + dir, 1, 12); break;
            case F_D:  v_[F_D] = wrap(v_[F_D] + dir, 1, days_in_month(v_[F_Y], v_[F_MO])); break;
            case F_H:  v_[F_H] = wrap(v_[F_H] + dir, 0, 23); break;
            case F_MI: v_[F_MI] = wrap(v_[F_MI] + dir, 0, 59); break;
        }
        // A day left dangling by a month change (31st, then to February) is
        // pulled back to that month's last day so the value is never impossible.
        int dim = days_in_month(v_[F_Y], v_[F_MO]);
        if (v_[F_D] > dim) v_[F_D] = dim;
    }

    static int wrap(int val, int lo, int hi) {
        if (val < lo) return hi;
        if (val > hi) return lo;
        return val;
    }

    // Draw the cursor bar under whichever field is live.
    void underline_field(Canvas &c, const char *date, const char *time, int yd, int yt) const {
        // Glyph offset (in characters) of each field's first digit, and its width.
        int off, wid, y;
        const char *s;
        switch (f_) {
            case F_Y:  s = date; off = 0; wid = 4; y = yd; break;
            case F_MO: s = date; off = 5; wid = 2; y = yd; break;
            case F_D:  s = date; off = 8; wid = 2; y = yd; break;
            case F_H:  s = time; off = 0; wid = 2; y = yt; break;
            default:   s = time; off = 3; wid = 2; y = yt; break;
        }
        int total = c.text_width(s, 2, false);
        int left = (c.width() - total) / 2;
        int glyph = c.text_width("0", 2, false) + 2;   // scale-2 advance
        int x = left + off * glyph;
        c.hline(x, y + ui::FH * 2 + 1, wid * glyph - 2, 1);
    }

    Action commit(void) {
        char line[40], out[80];
        snprintf(line, sizeof(line), "date set %04d-%02d-%02d %02d:%02d:00",
                 v_[F_Y], v_[F_MO], v_[F_D], v_[F_H], v_[F_MI]);
        fw_shell_run(line, out, sizeof(out));
        ui::notice("Set Time", out[0] ? out : "Clock set.");
        return ui::ACT_BACK;
    }
};

class ClockSettings : public SettingsList {
public:
    const char *title(void) const override { return "Clock"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "CPU MHz reads the clock, not";
        out[1] = "a saved preference — the shell";
        out[2] = "can move it too.";
        return 3;
    }

    bool tick(uint32_t dt) override {
        (void)dt;
        if (!g_said[0]) return false;
        ui::notice(g_said_title ? g_said_title : "Nova D1", g_said);
        g_said[0] = 0;
        return true;
    }

protected:
    enum { R_SET = 0, R_TZ, R_24H, R_CPU, R_SYNC, R_COUNT };

    int count(void) const override { return R_COUNT; }
    const char *label(int i) const override { return kLabels[i]; }

    void load(void) override { h24_ = nova::reg_bool(NOVA_KEY_PREFIX "Clock24", true); }
    void store(void) override { nova::reg_set_bool(NOVA_KEY_PREFIX "Clock24", h24_); }

    void value(int i, char *out, unsigned cap) const override {
        switch (i) {
            case R_SET: nova::copy(out, cap, ">"); break;
            case R_TZ: {
                int off = nova::reg_int("System.TZ_Offset", 0);
                snprintf(out, cap, "UTC%c%d", off < 0 ? '-' : '+', off < 0 ? -off : off);
                break;
            }
            case R_24H: nova::copy(out, cap, h24_ ? "on" : "off"); break;
            case R_CPU:
                // Read from the clock every time it is drawn, never from a
                // stored preference: this row, `pulse set` in the shell and the
                // boot clock all move the same number, and only one of them
                // would ever have written a key. A row claiming 200 on a board
                // sitting at 150 is worse than no row.
                snprintf(out, cap, "%uMHz", (unsigned)(fw_clock_hz() / 1000000u));
                break;
            default: nova::copy(out, cap, ">"); break;
        }
    }

    Action activate(int i) override {
        switch (i) {
            case R_SET: { SetTimeScreen *s = gui::push<SetTimeScreen>(); if (s) s->begin(); break; }
            case R_TZ:  gui::push<TimezoneScreen>(); break;
            case R_24H: h24_ = !h24_; dirty_ = true; break;
            case R_CPU: bump_cpu_clock(); break;
            default: {
                char out[112];
                if (fw_shell_run("ntp sync", out, sizeof(out)) != 0 && !out[0])
                    nova::copy(out, sizeof(out), "No answer from the time server.");
                ui::notice("Clock", out[0] ? out : "The clock is set.");
                break;
            }
        }
        return ui::ACT_STAY;
    }

private:
    static const char *const kLabels[R_COUNT];
    bool h24_;
};

const char *const ClockSettings::kLabels[ClockSettings::R_COUNT] = {
    "Set Time", "Timezone", "24-hour", "CPU", "Sync Clock",
};

static void open_clock_settings(void) {
    ClockSettings *s = gui::push<ClockSettings>();
    if (s) s->begin();
}

class SystemSettings : public SettingsList {
public:
    // Matches its row, which is "Settings" now — see the catalogue. novashots
    // fails the build if the two ever disagree again.
    const char *title(void) const override { return "Settings"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Clock holds the time, the zone";
        out[1] = "and the CPU speed. Versions is";
        out[2] = "what this device is.";
        return 3;
    }

    bool tick(uint32_t dt) override {
        (void)dt;
        if (!g_said[0]) return false;
        ui::notice(g_said_title ? g_said_title : "Nova D1", g_said);
        g_said[0] = 0;
        return true;
    }

protected:
    enum { R_CLOCK = 0, R_VERSIONS, R_REBOOT, R_RESET, R_COUNT };

    int count(void) const override { return R_COUNT; }
    const char *label(int i) const override { return kLabels[i]; }

    void load(void) override {}
    void store(void) override {}

    void value(int i, char *out, unsigned cap) const override {
        (void)i;
        nova::copy(out, cap, ">");
    }

    Action activate(int i) override {
        switch (i) {
            case R_CLOCK:    open_clock_settings(); break;
            case R_VERSIONS: open_set_device(); break;
            case R_REBOOT:
                ui::confirm("Restart the device now?", "Reboot", do_reboot, nullptr);
                break;
            default:
                ui::confirm("Put every setting back to its default and restart?",
                            "Reset", do_reset, nullptr);
                break;
        }
        return ui::ACT_STAY;
    }

private:
    static const char *const kLabels[R_COUNT];

    bool h24_;

};

// v1's shape: Clock and Versions are their own screens under System rather than
// rows beside Reboot. Its reasoning was that nothing at the top level should
// scroll, and it holds here — the clock rows are three settings about one thing
// and belong together.
//
// Verbose and SD Card are NOT carried over. Nothing in this build reads either;
// Settings.Verbose_Boot is declared by the OS itself and has no reader there
// either, which is an OS gap rather than a port gap. A row that changes nothing
// is worse than an absent one, because it teaches somebody the device is broken.
const char *const SystemSettings::kLabels[SystemSettings::R_COUNT] = {
    "Clock", "Versions", "Reboot", "Reset settings",
};

void open_set_system(void) {
    SystemSettings *s = gui::push<SystemSettings>();
    if (s) s->begin();
}

// --- Device ------------------------------------------------------------------------
//
// Everything that identifies this particular device, in one place and read-only.
//
// These numbers were scattered across four screens and a serial cable in the
// MicroPython suite. The first question when something behaves oddly is always
// "which of these am I actually running", and it should not take four screens to
// answer.

// Pull "  Total : 1024 KB" out of `df`. Found by its LABEL rather than by
// counting numbers, because df prints a warning of its own once the disk is
// nearly full and the percentage in it would then be read as the free space.
static uint32_t kb_after(const char *text, const char *label) {
    const char *p = strstr(text, label);
    if (!p) return 0;
    p += strlen(label);
    while (*p && (*p < '0' || *p > '9')) { if (*p == '\n') return 0; p++; }
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0');
    return v;
}

// The shell's answer, parked outside the screen: half a kilobyte is more than a
// pool slot holds and it is only wanted for the moment it takes to read it.
static char g_listing[512];

class DeviceScreen : public Screen {
public:
    // "Versions", matching the row that opens it and the name v1 used. It said
    // "Device" while the row said "Versions" — a header that disagrees with the
    // row you pressed reads as having landed somewhere else.
    const char *title(void) const override { return "Versions"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Turn to scroll. Nothing here";
        out[1] = "can be changed.";
        return 2;
    }

    void enter(void) override {
        top_ = 0;
        acc_ = 0;

        if (!fw_unique_id(uid_, sizeof(uid_))) nova::copy(uid_, sizeof(uid_), "?");

        // Two shell round trips, once, on the way in — never from draw(). The
        // filesystem size has no ABI call of its own, and the package's own
        // version lives in a header only the loader can read, so both come back
        // through the shell rather than being copied into this file where they
        // would drift the first time either changed.
        g_listing[0] = 0;
        fw_shell_run("df", g_listing, sizeof(g_listing));
        flash_kb_      = kb_after(g_listing, "Total");
        flash_free_kb_ = kb_after(g_listing, "Free");

        nova::copy(ver_, sizeof(ver_), "?");
        g_listing[0] = 0;
        fw_shell_run("pkg list", g_listing, sizeof(g_listing));
        const char *p = strstr(g_listing, "novad1");
        if (p) {
            p += 6;
            while (*p == ' ') p++;
            unsigned n = 0;
            while (p[n] && p[n] != ' ' && p[n] != '\n' && n + 1 < sizeof(ver_)) n++;
            if (n) { memcpy(ver_, p, n); ver_[n] = 0; }
        }
    }

    bool tick(uint32_t dt) override {
        // Once a second, and only for the two rows that move. A panel redrawing
        // an uptime whose last digit changes sixty times a second costs a great
        // deal to say very little.
        acc_ += dt;
        if (acc_ < 1000) return false;
        acc_ = 0;
        return true;
    }

    void draw(Canvas &c) override {
        const int rows = ui::rows_for(c);
        if (top_ > R_COUNT - rows) top_ = R_COUNT - rows;
        if (top_ < 0) top_ = 0;
        const int right = c.width() - (ui::SB_W + 1);

        char v[24];
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= R_COUNT) break;
            const int y = ui::TOP + i * ui::ROWH;
            value(idx, v, sizeof(v));
            // Narrow, and right-aligned in its own column. The unique id is
            // sixteen characters and at the normal advance it would run into its
            // own label; dropping each glyph's blank columns is what makes the
            // longest row on this screen fit the shortest panel.
            const int w = c.text_width(v, 1, true);
            c.text_fit(0, y, kLabels[idx], 1, right - w - 4, false);
            c.text(right - w, y, v, 1, 1, true);
        }
        c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP, top_, rows, R_COUNT);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { top_++; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { if (top_ > 0) top_--; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    enum { R_BOARD = 0, R_ID, R_OS, R_NAME, R_PKG, R_UP, R_HEAP, R_FLASH, R_COUNT };
    static const char *const kLabels[R_COUNT];

    int      top_;
    uint32_t acc_;
    uint32_t flash_kb_, flash_free_kb_;
    char     uid_[20];
    char     ver_[14];

    void value(int i, char *out, unsigned cap) const {
        switch (i) {
            case R_BOARD: nova::copy(out, cap, board::board_id()); break;
            case R_ID:    nova::copy(out, cap, uid_); break;
            case R_OS:    nova::copy(out, cap, nova::reg("Settings.Version", "?")); break;
            case R_NAME:  nova::copy(out, cap, nova::reg("System.Codename", "?")); break;
            case R_PKG:   nova::copy(out, cap, ver_); break;
            case R_UP:    say_uptime(out, cap); break;
            case R_HEAP:
                snprintf(out, cap, "%u/%uKB", (unsigned)(fw_heap_free() / 1024u),
                         (unsigned)(fw_heap_total() / 1024u));
                break;
            default:
                if (!flash_kb_) nova::copy(out, cap, "--");
                else snprintf(out, cap, "%u/%uKB", (unsigned)flash_free_kb_,
                              (unsigned)flash_kb_);
                break;
        }
    }

    // Coarse on purpose. Uptime is read to answer "has this been up since I last
    // touched it", and seconds on a device that has been running for two days
    // are noise moving in the corner of the screen.
    static void say_uptime(char *out, unsigned cap) {
        uint32_t s = fw_millis() / 1000u;
        if (s < 60)     snprintf(out, cap, "%us", (unsigned)s);
        else if (s < 3600) snprintf(out, cap, "%um", (unsigned)(s / 60));
        else if (s < 86400) snprintf(out, cap, "%uh %um",
                                     (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
        else snprintf(out, cap, "%ud %uh", (unsigned)(s / 86400),
                      (unsigned)((s % 86400) / 3600));
    }
};

const char *const DeviceScreen::kLabels[DeviceScreen::R_COUNT] = {
    "Board", "ID", "OS", "Release", "Nova D1", "Up", "Heap", "Flash",
};

void open_set_device(void) { gui::push<DeviceScreen>(); }

}  // namespace screens
}  // namespace nova
