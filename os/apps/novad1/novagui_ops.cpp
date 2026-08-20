// Desc: The operations screens — power, the health check, repair, and the command list.
// File: novagui_ops.cpp
#include "novagui_ops.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"
#include "novaboard.h"
#include "novamodtab.h"
#include "novabootcheck.h"
#include "novapower.h"
#include "novalog.h"
#include "display.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// --- running a command without stopping the screen ----------------------------------
//
// fw_shell_run is synchronous, and a real command takes tens of milliseconds at
// best and several seconds at worst — `novad1 setup` writes to flash, `wifi
// autoconnect` waits on a network. Called from on_event it would block the UI
// loop with the PREVIOUS frame still on the panel, so the device would look dead
// for exactly as long as the work took and there would be nothing on screen to
// say otherwise. So the command runs on a task of its own and the screen polls a
// flag from tick().
//
// ONE AT A TIME, and the reason is in the firmware rather than here: there is a
// single output capture. A second fw_shell_run while one is in flight still RUNS
// the command, but the buffer comes back empty — so a screen that did not guard
// against it would report "no output" for a command that printed plenty.

constexpr unsigned OUT_MAX   = 1024;   // about fifty lines of shell output
constexpr int      MAX_LINES = 60;     // the cap the MicroPython screen used

static char          g_cmd[RPC_SHELL_LINE_MAX];
static char          g_out[OUT_MAX];
static volatile bool g_busy;           // a command is in flight
static volatile bool g_done;           // ...and it has finished
static volatile int  g_rc;

static int shell_task(void *arg) {
    (void)arg;
    g_rc = fw_shell_run(g_cmd, g_out, OUT_MAX);
    // The flag LAST, after everything the reader will look at. The reader only
    // touches g_out once it has seen this, so the order the stores are made in
    // is the whole of the synchronisation.
    g_busy = false;
    g_done = true;
    return 0;
}

// Start `line`. False when something else is still running, or when there was no
// task to be had — both of which the caller has to say out loud rather than
// leaving a screen that waits forever.
static bool run_async(const char *line) {
    if (g_busy) return false;
    g_out[0] = 0;
    g_rc     = 0;
    g_done   = false;
    nova::copy(g_cmd, sizeof(g_cmd), line);
    g_busy = true;
    // Four kilobytes, the same as the GUI task itself. Not for the two lines
    // above it: an ABI call runs firmware on the PACKAGE's stack, so this budget
    // has to cover the whole shell command underneath. It is also why the
    // curated list keeps clear of anything that opens a TLS connection.
    if (fw_task_spawn("d1cmd", shell_task, nullptr, 4096) < 0) {
        g_busy = false;
        return false;
    }
    return true;
}

// One displayable row of the output: where it starts in g_out and how long it is.
//
// Offsets into the captured text rather than a wrapped copy of it. Wrapping into
// a second buffer would cost another kilobyte of bss for text already in memory,
// and wrapping IN PLACE is not safe — a break in the middle of a long word
// consumes no source byte and still writes a terminator, so the write position
// overtakes the read position on the first one.
struct OutLine {
    uint16_t at;
    uint8_t  len;
};

static OutLine g_line[MAX_LINES];
static int     g_nlines;

// Break the captured text into rows of at most `cols`, at spaces where there is
// one. Blank lines are dropped, as the MicroPython screen dropped them: shell
// output is spaced for an eighty-column terminal, and a blank row is a sixth of
// this panel.
static void split_output(int cols) {
    g_nlines = 0;
    if (cols < 4)   cols = 4;
    if (cols > 200) cols = 200;         // len is a byte

    unsigned i = 0;
    while (g_out[i] && g_nlines < MAX_LINES) {
        // Step over the terminators, and over the carriage returns a command
        // printed for a terminal that is not attached.
        if (g_out[i] == '\n' || g_out[i] == '\r') { i++; continue; }

        unsigned end = i;
        while (g_out[end] && g_out[end] != '\n' && g_out[end] != '\r') end++;

        unsigned start = i;
        while (start < end && g_nlines < MAX_LINES) {
            unsigned take = end - start;
            if (take > (unsigned)cols) {
                take = (unsigned)cols;
                // Break at the last space rather than mid-word — but only if
                // there is one. A single word longer than the panel has to be
                // cut somewhere, and cutting it beats an empty row followed by
                // the same problem.
                unsigned sp = take;
                while (sp > 0 && g_out[start + sp] != ' ') sp--;
                if (sp > 0) take = sp;
            }
            g_line[g_nlines].at  = (uint16_t)start;
            g_line[g_nlines].len = (uint8_t)take;
            g_nlines++;
            start += take;
            while (start < end && g_out[start] == ' ') start++;
        }
        i = end;
    }
}

// --- the output pane ------------------------------------------------------------
//
// One screen, shared by Repair, Commands and the incognito toggle. Deliberately
// NOT a terminal: there is a transcript and no prompt, because everything that
// reaches here was chosen from a list.

class OutputScreen : public Screen {
public:
    // `title` is HELD, not copied, so it has to outlive the screen. Every caller
    // passes a literal, which is the only lifetime that is obviously right here.
    void begin(const char *title, const char *line) {
        title_  = title;
        state_  = ST_WAIT;
        top_    = 0;
        spin_   = 0;
        acc_    = 0;
        frames_ = 0;
        rc_     = 0;
        split_  = false;
        nova::copy(cmd_, sizeof(cmd_), line ? line : "");
    }

    const char *title(void) const override { return title_; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Turn to scroll the output.";
        out[1] = "SELECT goes home when done.";
        return 2;
    }

    bool animating(void) const override { return state_ == ST_WAIT || state_ == ST_RUN; }

    bool tick(uint32_t dt) override {
        if (state_ == ST_WAIT) {
            // LET ONE FRAME REACH THE PANEL FIRST. This screen is pushed from a
            // menu press, and the push happens after tick returns — so starting
            // the command on the frame that asked for it means the menu is still
            // on the glass while the work runs, which is the "device looks
            // frozen" report this whole arrangement exists to answer.
            if (frames_ < 1) { frames_++; return true; }
            state_ = run_async(cmd_) ? ST_RUN : ST_BUSY;
            return true;
        }
        if (state_ == ST_RUN) {
            if (g_done) {
                rc_    = g_rc;
                state_ = ST_DONE;
                return true;
            }
            // The spinner, about eight times a second. Faster says nothing more
            // and every frame is a write to the panel.
            acc_ += dt;
            if (acc_ < 120) return false;
            acc_ = 0;
            spin_++;
            return true;
        }
        return false;
    }

    void draw(Canvas &c) override {
        if (state_ == ST_WAIT || state_ == ST_RUN) {
            c.text(2, ui::TOP + ui::ROWH, "Working...", 1);
            c.spinner(c.width() - 10, c.height() - 10, spin_, 1);
            return;
        }
        if (state_ == ST_BUSY) {
            c.text(2, ui::TOP, "Another command is", 1);
            c.text(2, ui::TOP + ui::ROWH, "still running. Try", 1);
            c.text(2, ui::TOP + 2 * ui::ROWH, "again in a moment.", 1);
            return;
        }

        if (!split_) { split_output(c.cols() - 1); split_ = true; }

        // One row is held back for the footer, so "it has finished" is stated
        // rather than inferred from a spinner having stopped.
        const int rows = ui::rows_for(c) - 1;
        if (top_ > g_nlines - rows) top_ = g_nlines - rows;
        if (top_ < 0)               top_ = 0;

        if (!g_nlines) {
            // Not necessarily silence. An empty buffer also means the firmware's
            // one capture was already taken, in which case the command RAN and
            // only its output was lost — so this says what is known rather than
            // reporting a blank answer as the answer.
            c.text(2, ui::TOP, "no output", 1);
        } else {
            const bool scrolls = g_nlines > rows;
            char row[34];
            for (int i = 0; i < rows; i++) {
                const int idx = top_ + i;
                if (idx >= g_nlines) break;
                unsigned n = g_line[idx].len;
                if (n > sizeof(row) - 1) n = sizeof(row) - 1;
                memcpy(row, g_out + g_line[idx].at, n);
                row[n] = 0;
                c.text(2, ui::TOP + i * ui::ROWH, row, 1);
            }
            if (scrolls)
                c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP,
                            c.height() - ui::TOP - ui::ROWH, top_, rows, g_nlines);
        }

        const int y = c.height() - ui::ROWH;
        c.hline(0, y - 1, c.width(), 1);
        c.text(2, y, verdict(), 1);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { top_++; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { if (top_ > 0) top_--; return ui::ACT_STAY; }
        // A finished command is a dead end otherwise: BACK walks back up however
        // many menus it took to get here. One press out.
        if (e == EV_SELECT && state_ == ST_DONE) return ui::ACT_HOME;
        return Screen::on_event(e);
    }

private:
    enum { ST_WAIT = 0, ST_RUN, ST_DONE, ST_BUSY };

    const char *title_;
    // Sixty-four rather than RPC_SHELL_LINE_MAX. The pane runs lines this file
    // wrote, not lines anybody typed, and a screen has 384 bytes to live in.
    char        cmd_[64];
    int         state_, top_, frames_, rc_;
    uint32_t    spin_, acc_;
    bool        split_;

    // What happened, from the command's own exit status rather than from
    // reading its output. The MicroPython screen learned this one the hard way:
    // scanning the text for the word "error" reported a successful update as
    // failed the moment its release notes mentioned fixing one.
    const char *verdict(void) const {
        if (rc_ < 0) return "refused - OK=home";
        return rc_ ? "failed - OK=home" : "done - OK=home";
    }
};

static void push_output(const char *title, const char *line) {
    OutputScreen *s = gui::push<OutputScreen>();
    if (s) s->begin(title, line);
}

// --- carrying out something that was agreed to ---------------------------------------
//
// The drastic rows all go through ui::confirm, and a confirm's yes callback
// cannot be where the work happens. It runs while the question is still the top
// screen and the question pops itself immediately afterwards — so a callback
// that PUSHED anything would have that pop take the pushed screen instead of the
// question, and a callback that stopped the machine would do it with the
// question still on the glass and nothing said about why.
//
// So the callback only records what was agreed to. The menu underneath is the
// top screen again by the time the next tick comes round; it paints what is
// about to happen and does it on the frame after that.
//
// The record is a FILE-STATIC rather than a member of the menu, and that is not
// tidiness: pop() calls enter() on the screen it reveals, so a member set by the
// callback would be cleared by that enter() before any tick could read it.

enum Pending { PEND_NONE = 0, PEND_REBOOT, PEND_SLEEP, PEND_SHUTDOWN };

static int g_pending;
static int g_pend_frames;

static void pend(int what) { g_pending = what; g_pend_frames = 0; }
static void pend_reboot(void *)   { pend(PEND_REBOOT); }
static void pend_sleep(void *)    { pend(PEND_SLEEP); }
static void pend_shutdown(void *) { pend(PEND_SHUTDOWN); }

static const char *pending_word(void) {
    switch (g_pending) {
        case PEND_REBOOT:   return "Rebooting";
        case PEND_SLEEP:    return "Sleeping";
        case PEND_SHUTDOWN: return "Shutting down";
        default:            return nullptr;
    }
}

// Do it, once the word above has actually reached the panel. Returns true while
// something is pending, so the screen holding it keeps repainting.
static bool pending_tick(void) {
    if (!g_pending) return false;

    // Two frames. A tick only marks the buffer dirty; the push to the panel
    // happens after tick returns. Stopping the machine on the frame that asked
    // for the notice means the notice is never sent, and the last thing anybody
    // saw was the menu.
    if (g_pend_frames < 2) { g_pend_frames++; return true; }

    const int what = g_pending;
    g_pending = PEND_NONE;

    if (what == PEND_REBOOT) {
        log::write("reboot from the power menu");
        fw_reboot();                    // does not return
        return true;
    }

    // Both stopping modes wake on the encoder's own switch. It is wired active
    // low with a pull-up — see novainput — so the wake is a LOW level and not a
    // high one, which is the sort of thing that reads as "the board never woke
    // up" rather than as an inverted argument.
    const int wake = board::pin(board::PIN_ENC_SW);

    // Dark before the clocks stop, and a full repaint after. The page diff is
    // against what was last SENT, and the panel has been through a reset in
    // between, so it no longer holds what the diff believes it holds.
    display().power(false);

    // DEVICE-UNCONFIRMED, and the risk is worth naming rather than discovering.
    // The firmware's sleep path resets core 1 before stopping the clocks, and a
    // package's task is spawned with no affinity — so the GUI task may be the
    // thing running on the core that is about to be reset. Whether this survives
    // being called from here has not been observed on hardware. There is no
    // useful guard: fw_core_id is explicitly not stable for an unaffined task,
    // so a check against it could be wrong in either direction.
    const int rc = (what == PEND_SHUTDOWN) ? fw_power_dormant(0, wake, 0)
                                           : fw_power_sleep(0, wake, 0);

    display().power(true);
    display().invalidate();

    if (rc != 0)
        ui::notice("Power", "The board would not sleep, so it is still running.");
    return true;
}

// Over the list rather than instead of it. Replacing the whole body reads as a
// different screen having appeared, which is not what happened.
static void draw_pending(Canvas &c) {
    const char *word = pending_word();
    if (!word) return;
    const int y = c.height() / 2 - ui::ROWH / 2;
    c.fill_rect(0, y - 3, c.width(), ui::ROWH + 5, 0);
    c.rect(0, y - 3, c.width(), ui::ROWH + 5, 1);
    c.text_centred(y, word, 1);
}

// --- screen off -------------------------------------------------------------------

class ScreenOff : public Screen {
public:
    const char *title(void) const override { return "Screen off"; }
    bool fullscreen(void) const override { return true; }

    void enter(void) override {
        // A real panel-off command, where the runner's idle tiers only ever
        // change CONTRAST — see the note beside them, which is right about the
        // automatic path: a panel powered down and a panel that failed to come
        // up look identical. What makes it safe here is that somebody chose it,
        // and the screen that turned the panel off is the one that turns it back
        // on, from leave() as well as from a gesture — so there is no way out of
        // this screen that leaves the glass dark.
        display().power(false);
    }

    void leave(void) override {
        display().power(true);
        display().invalidate();
    }

    void draw(Canvas &c) override {
        // Drawn even though nothing can see it. If the panel did not take the
        // power-down — an I2C write acknowledged and ignored is the failure this
        // hardware specialises in — then what is on the glass says what is
        // happening and how to get out of it, rather than looking wedged.
        c.text_centred(c.height() / 2 - ui::FH, "Screen off", 1);
        c.text_centred(c.height() / 2 + 2, "press to wake", 1);
    }

    Action on_event(Event e) override {
        // Any gesture. When the runner's own idle timer has already taken the
        // level down, the first press is spent waking that and never arrives
        // here — which is the same "the first press means show me" rule the rest
        // of the device follows, so it is not a surprise when it happens.
        (void)e;
        return ui::ACT_BACK;
    }
};

// --- the lock ------------------------------------------------------------------------
//
// The Security group has had four rows about a lock since the port started, and
// until now nothing on the device ever read what they wrote. Apps.NovaD1_PIN,
// Lock_Kind and LockSec were set by that screen and by `novad1 pin`, and no code
// anywhere asked for them: the settings were real, the lock was not.
//
// Two hooks in the UI leaf were built for this and were unused, which is where
// the shape below comes from rather than from taste. novaui.h on fullscreen():
// "The splash and the lock screen." novagui.cpp on modal(): "a lock that HOME
// escapes is not a lock."
//
// WHAT IT IS, said plainly, because the scope decides the design. It guards the
// PANEL against somebody picking the device up — not the shell, not the files,
// not a thief. The code is in the registry in the clear, `novad1 pin clear` over
// a cable removes it, and it does NOT engage at boot. That last one is a
// deliberate limit rather than an oversight: this runs as a boot service on
// boards whose buttons are not always finished, and a device that comes up
// demanding six digits it has no working switch to take is a device with no way
// in at all. The help says so, so the promise matches the behaviour.

// What the lock ACTUALLY is right now, as opposed to what is preferred.
//
// Lock type keeps its last value after the code is cleared, so a device with no
// PIN on it still reads "pin" in Lock_Kind. Everything that asks whether this
// device is locked has to ask THIS, or the settings row says PIN while nothing
// ever asks for one — and, worse, the runner would arm a lock with nothing to
// check against and brick the panel.
const char *lock_state(void) {
    const char *kind = nova::reg(NOVA_KEY_PREFIX "Lock_Kind", "none");
    if (nova::ieq(kind, "password"))
        return nova::reg(NOVA_KEY_PREFIX "Pass", "")[0] ? "Password" : "None";
    if (nova::ieq(kind, "pin"))
        return nova::reg(NOVA_KEY_PREFIX "PIN", "")[0] ? "PIN" : "None";
    return "None";
}

bool lock_armed(void) { return !nova::ieq(lock_state(), "None"); }

static bool     g_locked;           // the screen is up
static bool     g_lock_pass;        // the code just entered was right
static char     g_lock_note[24];    // and what to say when it was not
static int      g_lock_tries;
static unsigned g_lock_depth;       // where on the stack the lock screen sits

bool lock_active(void) { return g_locked; }

// Forget that the lock was ever up. For the runner's own start, which resets the
// screen stack to nothing — a lock left marked active across that would be a
// device with no power menu and a go_home() floor pointing at a slot that no
// longer holds a lock screen, for the rest of the session.
void lock_forget(void) {
    g_locked     = false;
    g_lock_depth = 1;
    g_lock_pass  = false;
    g_lock_tries = 0;
    g_lock_note[0] = 0;
}

// The lowest depth go_home() may pop to. THE LOCK IS THE FLOOR.
//
// A screen pushed OVER the lock can ask to go home, and one of them does: the
// on-screen keyboard returns ACT_HOME from EV_HOME, and it is the password
// lock's entry screen. Without a floor, HOME on that keyboard popped the lock
// with everything else — leaving a device unlocked with g_locked still set,
// which is a permanent unlock AND a power menu that never comes back, because
// both of those read g_locked. A reboot was the only way out.
unsigned lock_floor(void) { return g_locked ? g_lock_depth : 1; }

// The code, checked. Constant-time comparison would be theatre on a registry
// value anyone with a cable can read; what matters is that a wrong answer says
// so and a right one gets out.
static void lock_typed(void *, const char *text) {
    const bool pass = nova::ieq(lock_state(), "Password");
    const char *want = nova::reg(pass ? NOVA_KEY_PREFIX "Pass" : NOVA_KEY_PREFIX "PIN", "");
    if (text && want[0] && !strcmp(text, want)) {
        g_lock_pass = true;
        g_lock_note[0] = 0;
        return;
    }
    g_lock_tries++;
    // Counted and shown. Not enforced — there is nothing to lock somebody OUT
    // of that a reboot would not undo, and a wrong count that pretends otherwise
    // is worse than none. It is there so a device found with eleven failures on
    // it has something to say about that.
    snprintf(g_lock_note, sizeof(g_lock_note), "Wrong. %d tried", g_lock_tries);
}

class LockScreen : public Screen {
public:
    const char *title(void) const override { return "Locked"; }

    // Both of these, and both for the reason the leaf gives. Fullscreen because
    // the status bar is the runner's and a locked device should not be showing
    // which screen it was left on; modal because HOME is otherwise a guaranteed
    // way out of every screen, and a guaranteed way out of a lock is not a lock.
    bool fullscreen(void) const override { return true; }
    bool modal(void) const override { return true; }

    void draw(Canvas &c) override {
        char t[12];
        nova::time_hhmm(t, sizeof(t));
        c.text_centred(6, t, 1, 2, false);

        char name[20];
        nova::ellipsize(name, sizeof(name), nova::reg(NOVA_KEY_PREFIX "Name", "Nova D1"), 20);
        c.text_centred(28, name, 1);

        // The padlock, drawn rather than spelled: at this size a shackle over a
        // body reads from across a room and the word does not.
        const int cx = c.width() / 2;
        c.rect(cx - 5, 42, 10, 8, 1);
        c.line(cx - 3, 42, cx - 3, 39, 1);
        c.line(cx + 3, 42, cx + 3, 39, 1);
        c.hline(cx - 3, 38, 7, 1);

        c.text_centred(c.height() - ui::FH - 1,
                       g_lock_note[0] ? g_lock_note : "SELECT to unlock", 1);
    }

    // Popping itself from tick, which is how the splash gets off the stack too.
    // It cannot be done from the callback: ui::pinpad's screen returns ACT_BACK
    // after calling it, and the pop that follows takes the TOP screen — which
    // would be this one if it had already popped itself, and then the pinpad
    // would be left standing over an unlocked device.
    bool tick(uint32_t) override {
        // Two ways off this screen, and the second one was missing. The ordinary
        // way is a correct code (g_lock_pass). The other is the lock being cleared
        // from under it: `novad1 pin clear` over serial sets Lock_Kind to none
        // while this screen is up, and it leaves lock_armed() false. Without the
        // second half of this test the screen stayed, and with the code now empty
        // nothing entered on the pad could ever match it — a device locked out of
        // itself with a reboot the only way back in, which is the exact failure a
        // lock exists to not be. So a lock that is no longer armed lets go here.
        if (!g_lock_pass && lock_armed()) return false;
        g_lock_pass  = false;
        g_locked     = false;      // before the pop: go_home's floor reads it
        g_lock_depth = 1;
        g_lock_tries = 0;
        gui::pop();
        return true;
    }

    Action on_event(Event e) override {
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            if (nova::ieq(lock_state(), "Password"))
                ui::keyboard("Password", nullptr, true, lock_typed, nullptr, nullptr);
            else
                ui::pinpad("PIN", lock_typed, nullptr, nullptr);
            return ui::ACT_STAY;
        }
        // Everything else, including BACK and HOME, does nothing at all. Not
        // Screen::on_event — its default is exactly the two ways out this
        // screen exists to close.
        return ui::ACT_STAY;
    }
};

// Put it up. Safe to call from anywhere and every time: it refuses when there is
// nothing to check against, when it is already up, and when the screen on top is
// in the middle of something it may not be interrupted during — a staged update
// holding the panel is the case that exists today.
void lock_engage(void) {
    if (g_locked || !lock_armed()) return;
    ui::Screen *s = gui::top();
    if (s && s->modal()) return;
    g_lock_note[0] = 0;
    g_lock_pass    = false;
    if (!gui::push<LockScreen>()) return;    // a full stack; better unlocked than wedged
    g_locked    = true;
    g_lock_depth = gui::depth();
}

// --- what the buttons do here -----------------------------------------------------
//
// Every Screen has had a help() since the port started — fifty-odd of them, one
// line per control, and novaui.h describes the whole arrangement: "Holding HOME
// shows these instead, and the status bar marks a screen that has them with a
// '?'. Lists went from five rows to six the day that changed."
//
// The '?' was drawn. Nothing ever showed the lines. Holding HOME opens the power
// menu, and it always has, so the marker promised a screen that did not exist —
// on nearly every screen on the device, which makes it the largest dead button
// in the suite by some distance. Fifty screens' worth of good text was dead
// code lighting a mark nobody could act on.
//
// There is no spare gesture to give it: turn, press, back, home, and the two
// holds are all spoken for. So it goes where the one from-anywhere gesture
// already leads, as the FIRST row of the power menu — which is also the safest
// row there, and the power menu's whole shape is built around the cursor landing
// on something nobody minds pressing.

// The screen the help is ABOUT. Captured before the power menu is pushed,
// because after the push the top of the stack is the menu. It is the screen
// directly underneath, so it outlives the menu by construction — and the menu is
// the only thing that reads it.
static const ui::Screen *g_help_of;

class HelpScreen : public ui::Screen {
public:
    const char *title(void) const override { return "Controls"; }

    void draw(Canvas &c) override {
        const char *lines[8];
        const int n = g_help_of ? g_help_of->help(lines, 8) : 0;
        if (n <= 0) {
            c.text_centred(ui::TOP + ui::ROWH, "Turn, press, and BACK.", 1);
            c.text_centred(ui::TOP + 2 * ui::ROWH, "Nothing else here.", 1);
            return;
        }
        const int rows = ui::rows_for(c);
        for (int i = 0; i < n && i < rows; i++)
            c.text_fit(2, ui::TOP + i * ui::ROWH, lines[i], 1, c.width() - 4, false);
    }
};

static Action power_help(void *, int) {
    gui::push<HelpScreen>();
    return ui::ACT_STAY;
}

// --- Power ----------------------------------------------------------------------
//
// Holding HOME opens this from anywhere, which decides the whole shape of it.
// Nothing on it may be destructive in one press: the safe row is first, so the
// cursor lands there, and each of the three that stop the machine asks first.

// The OS's own latch, not a flag of this package's.
//
// The MicroPython suite kept its own and everything that was not Nova D1 code
// walked straight past it — `wifi scan` from the shell brought the radio back up
// while the screen still said incognito. `radio off` puts the lock underneath
// the network layer, where there is no path around it and it survives a reboot.
static bool incognito_on(void) {
    return nova::reg_is("System.RadioLock", "true", false);
}

static char g_incognito[20];

static Action power_screen_off(void *, int) {
    gui::push<ScreenOff>();
    return ui::ACT_STAY;
}

static Action power_incognito(void *, int) {
    // Through the shell rather than by writing the key, because taking the radio
    // down is more than a setting: the wireless chip's firmware is unloaded, and
    // `radio` is the one place that knows the order to do it in.
    push_output("Incognito", incognito_on() ? "radio on" : "radio off");
    return ui::ACT_STAY;
}

// The runner's own comment above EV_HOME_HOLD says holding HOME puts "lock and
// shutdown" one gesture away. Shutdown was here; lock was not, and had nothing
// behind it to reach. This is that row.
static Action power_lock(void *, int) {
    if (!lock_armed()) {
        ui::notice("Lock", "No code is set. Settings -> Security -> Lock type, "
                           "then Change code.");
        return ui::ACT_STAY;
    }
    // ACT_STAY, and the Power menu is left underneath on purpose. Going home
    // first would mean popping the screen whose on_event is running this
    // callback — pop() destroys it, and Menu::on_event carries on through `this`
    // afterwards. So the lock goes on top and unlocking comes back here, which
    // is where somebody was.
    lock_engage();
    return ui::ACT_STAY;
}

static Action power_sleep(void *, int) {
    // Asked BEFORE the question rather than after it, because a confirm's yes
    // callback has no way to put a notice on screen. A sleep with no wake pin
    // and no deadline is refused by the firmware anyway; saying why here is
    // better than a question that leads to a refusal.
    if (board::pin(board::PIN_ENC_SW) == board::PIN_NONE) {
        ui::notice("Sleep", "No encoder switch is wired, so there would be "
                            "nothing to wake it.");
        return ui::ACT_STAY;
    }
    ui::confirm("Sleep until a button? USB drops.", "Sleep", pend_sleep, nullptr);
    return ui::ACT_STAY;
}

static Action power_reboot(void *, int) {
    ui::confirm("Restart the device now?", "Reboot", pend_reboot, nullptr);
    return ui::ACT_STAY;
}

static Action power_shutdown(void *, int) {
    if (board::pin(board::PIN_ENC_SW) == board::PIN_NONE) {
        ui::notice("Shut down", "No encoder switch is wired, so there would be "
                                "nothing to wake it.");
        return ui::ACT_STAY;
    }
    // "Shut down" and not "power off", because there is no power off on this
    // board and calling it one would be a promise the hardware cannot keep.
    // This is the deepest stop it has: the processor is dormant until the switch
    // is pressed, and the board is still on a rail the whole time.
    ui::confirm("Stop the board until a button?", "Shut down", pend_shutdown, nullptr);
    return ui::ACT_STAY;
}

// The safest first, so the row the cursor lands on is one nobody minds pressing.
// Controls is safer still than Screen off and answers the question somebody
// holding an unfamiliar screen is most likely to have, so it takes the front.
static const ui::MenuItem kPowerItems[] = {
    { "Controls",    power_help,       nullptr },
    { "Screen off",  power_screen_off, nullptr },
    { "Lock",        power_lock,       nullptr },
    { g_incognito,   power_incognito,  nullptr },
    { "Sleep",       power_sleep,      nullptr },
    { "Reboot",      power_reboot,     nullptr },
    { "Shut down",   power_shutdown,   nullptr },
};

#define POWER_ROWS ((int)(sizeof(kPowerItems) / sizeof(kPowerItems[0])))

class PowerScreen : public ui::Menu {
public:
    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "Controls explains the screen";
        out[1] = "underneath this one.";
        out[2] = "Sleep and Shut down stop the";
        out[3] = "processor until a button.";
        return 4;
    }

    void enter(void) override {
        // The one row whose label is a reading rather than a name, so it says
        // which way pressing it goes. The MicroPython row read "Incognito" in
        // both states, which made it look like a dead button once stealth was
        // already on — the one place somebody would go to turn it off.
        nova::copy(g_incognito, sizeof(g_incognito),
                   incognito_on() ? "Incognito  on" : "Incognito  off");
        // refresh, so answering No to "Shut down" leaves the cursor on Shut
        // down rather than moving it to Screen off — which reads as the
        // question having been about something else.
        refresh("Power", kPowerItems, POWER_ROWS);
    }

    bool animating(void) const override { return g_pending != PEND_NONE; }
    bool tick(uint32_t dt) override { (void)dt; return pending_tick(); }

    void draw(Canvas &c) override {
        ui::Menu::draw(c);
        draw_pending(c);
    }
};

void open_power(void) {
    // Cleared here rather than trusted: the slot this lands in has been used
    // before and bss is not re-zeroed between pushes.
    g_pending = PEND_NONE;
    // BEFORE the push, while the top of the stack is still the screen somebody
    // was looking at. That screen is what Controls is about.
    g_help_of = gui::top();
    // Just the push. Making room for it is the RUNNER's job and not this
    // function's, and the difference is not tidiness: open_power is also the
    // catalogue row's OpenFn, called from inside Gallery::on_event, so a
    // go_home() here would pop the Gallery whose method is still executing and
    // return into a destroyed object. The runner's own loop has no screen method
    // on the stack and can make room safely — see EV_HOME_HOLD in run().
    gui::push<PowerScreen>();
}

// --- Sys Check ----------------------------------------------------------------------
//
// The startup check, on demand and standing still.
//
// It RE-RUNS the checks rather than reading back what the boot found, for two
// reasons. The boot screen pops itself when its animation ends and its results
// are private to that instance, so there is nothing left to read; and somebody
// opening this has usually just changed something and wants to know what the
// device finds NOW, which is the more useful answer anyway.
//
// THE TWO LISTS HAVE TO STAY IN STEP. A check added to novabootcheck belongs
// here as well. The honest fix is one set of checks both screens call, which
// needs an entry point in novabootcheck.h that does not exist yet.

constexpr int CHECKS = BootCheckScreen::STEPS;

static const char *const kCheckNames[CHECKS] = {
    "Display",
    "Storage",
    "Memory",
    "Controls",
    "Clock",
    "Power",
    "Network",
    "Modules",
    "Ready",
};

#define C_BAD  0
#define C_GOOD 1
#define C_SKIP 2

class SysCheckScreen : public Screen {
public:
    const char *title(void) const override { return "Sys Check"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT shows the detail.";
        out[1] = "Hold SELECT to run it again.";
        out[2] = "'n/a' is not a failure.";
        return 3;
    }

    // No enter(). enter() runs again every time something pushed from here pops,
    // and restarting nine checks — one of which talks to every address on the
    // I2C bus — because a detail view was dismissed is work nobody asked for.
    void begin(void) { restart(); }

    bool animating(void) const override { return step_ < CHECKS; }

    bool tick(uint32_t dt) override {
        (void)dt;
        if (step_ >= CHECKS) return false;
        // ONE STEP PER FRAME, the same discipline the boot screen uses. A check
        // that blocks then shows as a pause on the row it is actually on, which
        // is information, rather than as a stalled list.
        run_check(step_);
        step_++;
        if (step_ >= CHECKS) log_summary();
        return true;
    }

    void draw(Canvas &c) override {
        const int rows = ui::rows_for(c);
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= CHECKS) break;
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, c.width() - ui::SB_W - 1, ui::ROWH, 1, true);

            c.text_fit(3, y, kCheckNames[idx], on ? 0 : 1, 60, false);
            // The mark right-aligned in its own column, so the eye runs down one
            // edge rather than hunting along each row.
            const char *m = mark(res_[idx]);
            const int w = c.text_width(m, 1, false);
            c.text(c.width() - ui::SB_W - 3 - w, y, m, on ? 0 : 1);
        }
        c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP,
                    top_, rows, CHECKS);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % CHECKS; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + CHECKS - 1) % CHECKS; return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD) { restart(); return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            if (res_[sel_] < 0) return ui::ACT_STAY;      // nothing to show yet
            // The body is copied by notice(); the title is held, and these are
            // literals, so both lifetimes are right.
            char body[80];
            snprintf(body, sizeof(body), "%s\n%s", word(res_[sel_]), detail_[sel_]);
            ui::notice(kCheckNames[sel_], body);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int    sel_, top_, step_;
    int8_t res_[CHECKS];              // -1 not run, 0 bad, 1 good, 2 not applicable
    char   detail_[CHECKS][22];

    void restart(void) {
        sel_ = 0;
        top_ = 0;
        step_ = 0;
        for (int i = 0; i < CHECKS; i++) { res_[i] = -1; detail_[i][0] = 0; }
    }

    static const char *mark(int8_t r) {
        switch (r) {
            case C_GOOD: return "ok";
            case C_BAD:  return "fail";
            case C_SKIP: return "n/a";
            default:     return "..";
        }
    }

    static const char *word(int8_t r) {
        switch (r) {
            case C_GOOD: return "Good.";
            case C_BAD:  return "A problem.";
            case C_SKIP: return "Not applicable.";
            default:     return "Not run.";
        }
    }

    void log_summary(void) {
        int bad = 0;
        for (int i = 0; i < CHECKS; i++) if (res_[i] == C_BAD) bad++;
        char line[48];
        snprintf(line, sizeof(line), "sys check: %d problem(s)", bad);
        log::write(line);
    }

    void run_check(int i);
};

void SysCheckScreen::run_check(int i) {
    char *d = detail_[i];
    const unsigned cap = sizeof(detail_[0]);

    switch (i) {
        case 0: {   // Display — it is up, or none of this would be readable
            Display &dp = display();
            res_[i] = dp.ready() ? C_GOOD : C_BAD;
            if (dp.ready()) snprintf(d, cap, "%s 0x%02x", dp.kind_name(), dp.address());
            else            nova::copy(d, cap, "no panel");
            break;
        }
        case 1: {   // Storage — the tree is there
            nova::paths_init();
            const bool ok = fw_file_exists(NOVA_ROOT) != 0;
            res_[i] = ok ? C_GOOD : C_BAD;
            nova::copy(d, cap, ok ? NOVA_ROOT : "no /nova");
            break;
        }
        case 2: {   // Memory — free, and the largest single block
            const unsigned free_kb = (unsigned)(fw_heap_free() / 1024u);
            const unsigned big_kb  = (unsigned)(fw_heap_largest() / 1024u);
            snprintf(d, cap, "%u KB, max %u", free_kb, big_kb);
            // Sixteen is not a round number: below it there is not enough for a
            // TLS connection or a second package.
            res_[i] = free_kb >= 16 ? C_GOOD : C_BAD;
            break;
        }
        case 3: {   // Controls — the encoder and the buttons have pins
            const int a  = board::pin(board::PIN_ENC_A);
            const int sw = board::pin(board::PIN_ENC_SW);
            if (a == board::PIN_NONE || sw == board::PIN_NONE) {
                res_[i] = C_BAD;
                nova::copy(d, cap, "not wired");
            } else {
                // Whether it TURNS cannot be known without somebody turning it,
                // so this reports what is configured and claims nothing more.
                res_[i] = C_GOOD;
                snprintf(d, cap, "EC11 on %d/%d", a, board::pin(board::PIN_ENC_B));
            }
            break;
        }
        case 4: {   // Clock — set, or running from nothing
            FwTime t;
            if (fw_time_get(&t)) {
                res_[i] = C_GOOD;
                snprintf(d, cap, "%02d:%02d %d-%02d-%02d", t.hour, t.minute,
                         t.year, t.month, t.day);
            } else {
                // Not a failure. A device with no RTC and no network has no way
                // to know the time and works perfectly well without it.
                res_[i] = C_SKIP;
                nova::copy(d, cap, "not set");
            }
            break;
        }
        case 5: {   // Power — USB, a battery, or no way to tell
            switch (power::source()) {
                case power::PWR_USB:
                    res_[i] = C_GOOD;
                    nova::copy(d, cap, "USB");
                    break;
                case power::PWR_BATTERY: {
                    // Knowing it is on the cell is not knowing how full it is:
                    // that needs the battery divider, which the profile leaves
                    // unset. Without one there is no percentage to print, and
                    // "battery -1%" is not an answer.
                    int p = power::percent();
                    if (p < 0) {
                        res_[i] = C_SKIP;
                        nova::copy(d, cap, "battery, no sense pin");
                    } else {
                        res_[i] = power::low() ? C_BAD : C_GOOD;
                        snprintf(d, cap, "battery %d%%", p);
                    }
                    break;
                }
                default:
                    res_[i] = C_SKIP;
                    nova::copy(d, cap, "not known");
                    break;
            }
            break;
        }
        case 6: {   // Network — joined, or not
            if (fw_net_connected()) {
                char ssid[34];
                fw_net_ssid(ssid, sizeof(ssid));
                res_[i] = C_GOOD;
                nova::ellipsize(d, cap, ssid, cap - 1);
            } else {
                // Also not a failure. Most of what this device does needs no
                // network, and a red mark at every boot teaches people to ignore
                // the colour.
                res_[i] = C_SKIP;
                nova::copy(d, cap, "not joined");
            }
            break;
        }
        case 7: {   // Modules — how many answered
            modules_scan();
            int found = 0, wired = 0;
            const Module *m = modules();
            for (unsigned k = 0; k < module_count(); k++) {
                if (!module_wired(m[k])) continue;
                wired++;
                if (module_presence(m[k]) == MOD_PRESENT) found++;
            }
            snprintf(d, cap, "%d of %d answered", found, wired);
            res_[i] = C_GOOD;
            break;
        }
        default: {  // Ready — and what to say when it is not
            int bad = 0;
            for (int k = 0; k < CHECKS - 1; k++) if (res_[k] == C_BAD) bad++;
            res_[i] = bad ? C_BAD : C_GOOD;
            if (bad) snprintf(d, cap, "%d problem%s", bad, bad == 1 ? "" : "s");
            else     nova::copy(d, cap, "nothing wrong");
            break;
        }
    }
}

void open_check(void) {
    SysCheckScreen *s = gui::push<SysCheckScreen>();
    if (s) s->begin();
}

// --- Repair ----------------------------------------------------------------------
//
// The things somebody would otherwise drop to the shell for when the device is
// misbehaving, one press each. Every row says what it did afterwards, because
// "the button did nothing" and "the button worked and nothing needed doing" are
// the same picture otherwise.

static Action fix_rescan(void *, int) {
    // Synchronous, as the Hardware screen's own rescan is: a probe is a short
    // write to each address on the bus, not a command that can sit waiting on
    // the world.
    modules_scan();
    int found = 0, wired = 0;
    const Module *m = modules();
    for (unsigned k = 0; k < module_count(); k++) {
        if (!module_wired(m[k])) continue;
        wired++;
        if (module_presence(m[k]) == MOD_PRESENT) found++;
    }
    char body[64];
    snprintf(body, sizeof(body), "%d of %d wired modules answered.", found, wired);
    ui::notice("Rescan", body);
    return ui::ACT_STAY;
}

static Action fix_display(void *, int) {
    // Off and on again, which is the fix for a panel showing torn or frozen
    // rows: a write that arrived half finished leaves the controller's page
    // pointer somewhere the driver does not think it is, and there is nothing
    // in the protocol to ask where it got to.
    //
    // The pause blocks this screen for four tenths of a second, deliberately —
    // the controller needs the gap, the panel is dark for the whole of it, and
    // fw_task_sleep_ms yields, so nothing else on the device is held up.
    Display &d = display();
    d.power(false);
    fw_task_sleep_ms(400);
    d.power(true);
    d.invalidate();
    ui::notice("Display", "The panel was switched off and on again.");
    return ui::ACT_STAY;
}

static Action fix_clear_log(void *, int) {
    log::clear();
    ui::notice("Log", "The event log was cleared.");
    return ui::ACT_STAY;
}

static Action fix_wifi_join(void *, int)   { push_output("WiFi", "wifi autoconnect"); return ui::ACT_STAY; }
static Action fix_wifi_state(void *, int)  { push_output("WiFi", "wifi status"); return ui::ACT_STAY; }
static Action fix_setup(void *, int)       { push_output("Setup", "novad1 setup"); return ui::ACT_STAY; }

static Action fix_restart(void *, int) {
    // This one does not get to report back, and it cannot be made to. `novad1
    // refresh` stops and restarts the GUI service, so the task that owns this
    // pane is gone before the command it started has finished — the screen
    // people actually see next is the boot check on the way back up.
    push_output("Restart", "novad1 refresh");
    return ui::ACT_STAY;
}

static Action fix_reboot(void *, int) {
    ui::confirm("Restart the device now?", "Reboot", pend_reboot, nullptr);
    return ui::ACT_STAY;
}

static const ui::MenuItem kFixItems[] = {
    { "Rescan modules",   fix_rescan,     nullptr },
    { "Reset the panel",  fix_display,    nullptr },
    { "Reconnect WiFi",   fix_wifi_join,  nullptr },
    { "WiFi status",      fix_wifi_state, nullptr },
    { "Restart the screen", fix_restart,  nullptr },
    { "Run setup again",  fix_setup,      nullptr },
    { "Clear the log",    fix_clear_log,  nullptr },
    { "Reboot",           fix_reboot,     nullptr },
};

#define FIX_ROWS ((int)(sizeof(kFixItems) / sizeof(kFixItems[0])))

class RepairScreen : public ui::Menu {
public:
    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Each row says what it did.";
        out[1] = "Nothing here deletes anything.";
        return 2;
    }

    // refresh, not set: every row here pushes a notice, an output pane or a
    // confirmation, and each of those pops back through enter(). set() would
    // put the cursor on "Rescan modules" after every one of them.
    void enter(void) override { refresh("Repair", kFixItems, FIX_ROWS); }

    bool animating(void) const override { return g_pending != PEND_NONE; }
    bool tick(uint32_t dt) override { (void)dt; return pending_tick(); }

    void draw(Canvas &c) override {
        ui::Menu::draw(c);
        draw_pending(c);
    }
};

void open_repair(void) {
    g_pending = PEND_NONE;
    gui::push<RepairScreen>();
}

// --- Commands ----------------------------------------------------------------------
//
// A FIXED LIST rather than a prompt, and in the MicroPython suite that was a
// decision rather than a step on the way to one. Typing a command here means the
// on-screen keyboard — one encoder, ten columns, four rows, and the better part
// of ten seconds for a word. A list of the dozen answers anybody actually wants
// is quicker than typing any one of them and cannot be mistyped.
//
// It also keeps the list to things that are safe to run by accident. Everything
// here REPORTS; nothing here reconfigures. Anything that changes the wiring or
// removes something is left to the shell, where there is a real keyboard and
// somebody who went looking for it.

static Action cmd_row(void *, int index);

static const ui::MenuItem kCmdItems[] = {
    { "System info",  cmd_row, nullptr },
    { "Nova status",  cmd_row, nullptr },
    { "Memory",       cmd_row, nullptr },
    { "Reclaimable",  cmd_row, nullptr },
    { "Storage",      cmd_row, nullptr },
    { "Uptime",       cmd_row, nullptr },
    { "Tasks",        cmd_row, nullptr },
    { "Pins",         cmd_row, nullptr },
    { "WiFi status",  cmd_row, nullptr },
    { "Radio lock",   cmd_row, nullptr },
    { "Nova logs",    cmd_row, nullptr },
    { "Version",      cmd_row, nullptr },
};

// The line each row runs, in the same order. Two arrays rather than a struct
// carrying both, because MenuItem is what the widget takes and wrapping it would
// mean copying every row into one at open time.
static const char *const kCmdLines[] = {
    "sysinfo",
    "novad1 status",
    "meminfo",
    "freeup",
    "df",
    "uptime",
    "ps",
    "novad1 pins",
    "wifi status",
    "radio status",
    "novad1 logs",
    "ver",
};

#define CMD_ROWS ((int)(sizeof(kCmdItems) / sizeof(kCmdItems[0])))

static_assert(sizeof(kCmdItems) / sizeof(kCmdItems[0])
              == sizeof(kCmdLines) / sizeof(kCmdLines[0]),
              "every Commands row needs a line to run");

static Action cmd_row(void *, int index) {
    if (index < 0 || index >= CMD_ROWS) return ui::ACT_STAY;
    // The label is the pane's title. It is a literal in the table above, so
    // holding the pointer is safe, and it names the answer rather than the
    // command — which is what somebody who chose it from a list is looking for.
    push_output(kCmdItems[index].label, kCmdLines[index]);
    return ui::ACT_STAY;
}

class CommandsScreen : public ui::Menu {
public:
    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "A fixed list, not a prompt.";
        out[1] = "Everything here only reports.";
        out[2] = "The shell has the rest.";
        return 3;
    }

    // Twelve rows, and every one of them opens an output pane you then back
    // out of. set() here meant finding your place again after each command.
    void enter(void) override { refresh("Commands", kCmdItems, CMD_ROWS); }
};

void open_commands(void) { gui::push<CommandsScreen>(); }

}  // namespace screens
}  // namespace nova
