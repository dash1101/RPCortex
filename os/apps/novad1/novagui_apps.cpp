// Desc: The Shell, Scripts, App Store and Updates screens — the four that run commands.
// File: novagui_apps.cpp
#include "novagui_apps.h"
#include "novagui.h"
#include "novaapps.h"
#include "novakeys.h"
#include "novacore.h"
#include "novanotify.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// --- running a command without stopping the screen --------------------------------
//
// fw_shell_run is synchronous and `pkg install` is a download. Called from
// on_event it holds the UI loop until the network answers, and a panel that
// stops redrawing is indistinguishable from a device that has died — which is
// the failure this whole section exists to prevent.
//
// So a command runs on a task of its own and the screen polls a flag from
// tick(). That is also the only shape the firmware permits: a package command
// dispatched from inside another package command on the same task overwrites
// the outer call's way home, and the shell refuses it. Running one in a task of
// its own is the sanctioned route.
//
// ONE JOB AT A TIME, across all three screens. There is a single capture buffer
// in the OS: a second command started while the first holds it still RUNS, and
// hands back nothing. That reads as a command which printed nothing rather than
// as one that was never heard, so it is refused here instead.

enum { JOB_IDLE = 0, JOB_RUN, JOB_DONE };

static volatile uint8_t g_job_state;
static const void      *g_job_owner;       // the screen that asked, for the reap below
static char             g_job_line[128];
static char            *g_job_out;
static uint32_t         g_job_cap;

// The command's exit status, kept rather than dropped.
//
// Only the Updates screen reads it, and it needs it for one distinction the
// text cannot make: `safeboot` prints "restarting to run '...'" and then
// restarts, so being still alive to read that line means something refused the
// command instead. A status of zero and a device that is still here is the
// signal, and no amount of parsing the output gives it.
static volatile int     g_job_rc;

// The one text buffer: every job captures into it, and the pane below shows it
// in place rather than taking a copy.
//
// 2.5 KB is what `pkg search` needs with room to spare — fifteen packages come
// back as about 1.4 KB of listing. A longer index is TRUNCATED and the screen
// shows what fitted; out_capture_overflowed is not on the ABI, so a package
// cannot tell a full buffer from a short answer and must not pretend to.
#define APPS_OUT_BYTES 2560
static char g_apps_out[APPS_OUT_BYTES];

static int job_task(void *) {
    if (g_job_out && g_job_cap) g_job_out[0] = 0;
    g_job_rc = fw_shell_run(g_job_line, g_job_out, g_job_cap);
    // LAST, and after both the output and the status are written. The UI task
    // reads them only once it has seen this, so the order of these lines is
    // what makes the handover safe without a lock — the same single-writer rule
    // the input queue is built on.
    g_job_state = JOB_DONE;
    gui::invalidate();
    return 0;
}

// Start `line`, capturing into `out`. False when something is already running,
// which the caller has to say out loud rather than silently doing nothing.
static bool job_start(const void *owner, const char *line, char *out, uint32_t cap) {
    if (g_job_state != JOB_IDLE) return false;
    nova::copy(g_job_line, sizeof(g_job_line), line);
    g_job_out   = out;
    g_job_cap   = cap;
    g_job_owner = owner;
    g_job_state = JOB_RUN;
    // Four kilobytes, matching the GUI task. The command's own frames come out
    // of the firmware's call reserve rather than this, so the number only
    // matters on a build with no sandbox — where the shell runs on this stack
    // instead and a task minimum would not survive a TLS handshake.
    if (fw_task_spawn("d1-cmd", job_task, nullptr, 4096) < 0) {
        g_job_state = JOB_IDLE;
        return false;
    }
    return true;
}

// Has this screen's job finished? Taking the result frees the runner.
//
// A job whose owner has gone — the screen was popped while it ran — is reaped
// by whoever asks next and reported as not theirs. Without that the runner
// would stay claimed for good, and nothing on the device could run another
// command until the next reboot.
static bool job_take(const void *owner) {
    if (g_job_state != JOB_DONE) return false;
    bool mine = (g_job_owner == owner);
    g_job_owner = nullptr;
    g_job_state = JOB_IDLE;
    return mine;
}

// What the last job returned. Only meaningful straight after job_take said the
// result was yours; nothing clears it, because nothing else looks at it.
static int job_rc(void) { return g_job_rc; }

// How long a spinner frame lasts. Slow enough to read as turning rather than
// as flickering, and it is the only thing on these screens that animates.
#define SPIN_MS 140

// --- reading what a command said ----------------------------------------------------
//
// Three screens need the same two sentences out of a block of console output:
// what went wrong, and what the last word on the subject was. Both are here
// rather than in whichever screen wanted them first, because a rule that lives
// in two places is a rule that eventually disagrees with itself.

// Step over a terminal tag — "[@] ", "[:] ", "[?] ", "[!] " — and over the
// second one that names the command inside it, "[!] [pkg] ...". Both are for a
// console, not for a panel nine pixels tall.
static const char *untag(const char *s, const char *end) {
    if (!(s + 4 <= end && s[0] == '[' && s[1] && s[2] == ']' && s[3] == ' ')) return s;
    s += 4;
    // The second one ONLY on a line that carried the first. A tagged line can
    // name the command inside it — "[!] [pkg] ..." — and that is console
    // furniture too. Conditional, because a data line is not guaranteed to be
    // free of brackets and stripping one off a description would be silent.
    if (s < end && s[0] == '[') {
        const char *close = s;
        while (close < end && *close != ']') close++;
        if (close + 1 < end && *close == ']' && close[1] == ' ') s = close + 2;
    }
    return s;
}

static void apps_copy_span(const char *s, const char *end, char *out, unsigned cap) {
    unsigned n = 0;
    while (s + n < end && n + 1 < cap) n++;
    memcpy(out, s, n);
    out[n] = 0;
}

// The last thing a command said, untagged.
//
// The LAST line, not the first. A search that found the index and matched
// nothing prints its heading before saying so, and the heading is the one line
// that explains nothing.
static void last_line(const char *text, char *out, unsigned cap) {
    out[0] = 0;
    const char *best = nullptr, *best_end = nullptr;
    for (const char *p = text; *p; ) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        if (e > p) { best = p; best_end = e; }
        p = *e ? e + 1 : e;
    }
    if (best) apps_copy_span(untag(best, best_end), best_end, out, cap);
}

// The first line the command tagged as an error, untagged. False when it
// reported none.
//
// The FIRST rather than the last: "No network. Connect first." is the reason
// and "No package list could be fetched." is only what followed from it, and
// the reason is the one somebody can act on.
static bool first_error(const char *text, char *out, unsigned cap) {
    for (const char *p = text; *p; ) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        if (e - p > 4 && p[0] == '[' && p[1] == '!' && p[2] == ']' && p[3] == ' ') {
            apps_copy_span(untag(p, e), e, out, cap);
            return out[0] != 0;
        }
        p = *e ? e + 1 : e;
    }
    return false;
}

// Why something did not work, in the command's own words. Its first error when
// it tagged one, and otherwise the last thing it said — which covers a command
// that reported a problem without tagging it as one.
static void reason(const char *text, char *out, unsigned cap) {
    if (!first_error(text, out, cap)) last_line(text, out, cap);
}

// Strip the spaces off both ends, in place. A command typed on the on-screen
// keyboard collects them easily — SPC is a key somebody presses while thinking
// — and " ls" is not a command.
static void trim(char *s) {
    unsigned n = 0;
    while (s[n] == ' ') n++;
    if (n) memmove(s, s + n, strlen(s + n) + 1);
    unsigned len = (unsigned)strlen(s);
    while (len && s[len - 1] == ' ') s[--len] = 0;
}

// --- the text pane -----------------------------------------------------------------
//
// One screen for two things that are the same thing: showing a file, and
// showing what a command printed. Both are a block of text longer than the
// panel with no structure worth parsing, and the encoder scrolls it.
//
// It indexes g_apps_out IN PLACE — the newlines become terminators — rather than
// copying. The alternative is a second buffer the same size, for a screen that
// is only ever up while nothing else is using the first one.

#define PANE_LINES 48
static const char *g_apps_line[PANE_LINES];
static int         g_lines;

static void pane_index(void) {
    g_lines = 0;
    char *p = g_apps_out;
    while (*p && g_lines < PANE_LINES) {
        g_apps_line[g_lines++] = p;
        while (*p && *p != '\n') p++;
        if (!*p) break;
        *p++ = 0;
        // A file written on a PC ends its lines with a carriage return too, and
        // a stray 0x0d draws as a glyph rather than as nothing.
        if (p - 2 >= g_apps_out && p[-2] == '\r') p[-2] = 0;
        // Every printed line ends in a newline, so the terminator after the
        // last one is not a blank final row.
        if (!*p) break;
    }
}

class PaneScreen : public Screen {
public:
    // begin() rather than enter(), for the reason in open_shell below.
    void begin(const char *title) { title_ = title; top_ = 0; }

    const char *title(void) const override { return title_; }

    void draw(Canvas &c) override {
        if (g_lines <= 0) {
            c.text_centred(ui::TOP + ui::ROWH, "(nothing to show)", 1);
            return;
        }
        const int rows = ui::rows_for(c);
        if (top_ > g_lines - rows) top_ = g_lines - rows;
        if (top_ < 0) top_ = 0;

        const bool scrolls = g_lines > rows;
        const int  right   = scrolls ? c.width() - (ui::SB_W + 1) : c.width();
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= g_lines) break;
            c.text_fit(2, ui::TOP + i * ui::ROWH, g_apps_line[idx], 1, right - 2, false);
        }
        if (scrolls)
            c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP,
                        top_, rows, g_lines);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { top_++; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { if (top_ > 0) top_--; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    const char *title_;
    int         top_;
};

// Show whatever is in g_apps_out. The title must outlive the screen, so it is a
// literal or a static — never a local.
static void pane_show(const char *title) {
    pane_index();
    PaneScreen *s = gui::push<PaneScreen>();
    if (s) s->begin(title);
}

// --- Shell ---------------------------------------------------------------------------
//
// The OS shell on the panel. Not a terminal emulator: there is no cursor to
// move and no character grid to maintain, only a transcript and one editable
// line — which is all an encoder and two buttons can drive, and it means the
// expensive part, the keyboard, exists only while somebody is typing.
//
// The transcript is a FIXED RING. An unbounded one is a leak that shows up only
// after a long session, which is the worst kind to find on a device meant to be
// left running.

#define LOG_LINES 40
#define LOG_COLS  22               // 21 characters and a terminator: the panel's width
static char g_log[LOG_LINES][LOG_COLS];
static int  g_log_n;

// The last few commands, newest first.
//
// The keyboard is forty keys turned one detent at a time, so retyping `pkg
// list` costs about as much attention as the command was worth. Sixty-four is
// the keyboard's own limit; a shorter one here would silently truncate
// something somebody had already typed in full.
#define HIST_MAX 6
static char g_hist[HIST_MAX][64];
static int  g_hist_n;

static void log_emit(const char *text) {
    if (g_log_n < LOG_LINES) { nova::copy(g_log[g_log_n++], LOG_COLS, text); return; }
    // Full: slide up by one. Under a kilobyte moved once per line of output,
    // against ring arithmetic that every draw would have to unpick — the
    // MicroPython screen made the same trade and for the same reason.
    memmove(g_log[0], g_log[1], (LOG_LINES - 1) * LOG_COLS);
    nova::copy(g_log[LOG_LINES - 1], LOG_COLS, text);
}

// Wrapped once, on the way in. Re-splitting the whole transcript at every draw
// would repeat the same work thirty times a second for an answer that cannot
// change: the panel is not going to get wider while it is being read.
static void log_emit_wrapped(const char *text) {
    char        store[LOG_COLS * 6];
    const char *lines[6];
    int cols = gui::canvas().cols() - 1;
    if (cols > LOG_COLS - 1) cols = LOG_COLS - 1;
    if (cols < 1) cols = 1;
    const int n = ui::wrap(text, cols, store, sizeof(store), lines, 6);
    if (n <= 0) { log_emit(""); return; }
    for (int i = 0; i < n; i++) log_emit(lines[i]);
}

static void hist_add(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    // Already there: move it to the front rather than keeping two copies. A
    // history of six that spends three of them on the same command is a history
    // of three.
    int at = -1;
    for (int i = 0; i < g_hist_n; i++)
        if (!strcmp(g_hist[i], cmd)) { at = i; break; }
    if (at < 0) {
        if (g_hist_n < HIST_MAX) g_hist_n++;
        at = g_hist_n - 1;                  // full: the oldest falls off the shift
    }
    for (int i = at; i > 0; i--) memcpy(g_hist[i], g_hist[i - 1], sizeof(g_hist[0]));
    nova::copy(g_hist[0], sizeof(g_hist[0]), cmd);
}

class ShellScreen : public Screen {
public:
    void begin(void) {
        top_     = 0;
        follow_  = true;
        waiting_ = false;
        phase_   = 0;
        pending_[0] = 0;
        // The transcript belongs to the device rather than to the visit, so
        // coming back finds what was there. Only a first run needs the greeting.
        if (!g_log_n) {
            log_emit("RPCortex shell.");
            log_emit("Hold SELECT to type.");
        }
    }

    const char *title(void) const override { return "Shell"; }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "Hold SELECT to type.";
        out[1] = "SELECT runs the line, or";
        out[2] = "offers the last few commands.";
        out[3] = "Turn scrolls. BACK clears.";
        return 4;
    }

    // A command in flight is the one time this screen moves, and the frame rate
    // has to keep up with the spinner or it reads as a device that has stopped.
    bool animating(void) const override { return waiting_; }

    bool tick(uint32_t dt) override {
        if (!waiting_) return false;
        phase_ += dt;
        if (job_take(this)) {
            waiting_ = false;
            absorb();
            follow_ = true;
        }
        return true;
    }

    void draw(Canvas &c) override {
        // One row is kept for the prompt, so what has been typed is on the
        // panel however far the transcript is scrolled back.
        const int vis = ui::rows_for(c) - 1;
        const int n   = g_log_n;
        if (follow_ || top_ > n - vis) top_ = n - vis;
        if (top_ < 0) top_ = 0;

        const bool scrolls = n > vis;
        for (int i = 0; i < vis; i++) {
            const int idx = top_ + i;
            if (idx >= n) break;
            c.text(2, ui::TOP + i * ui::ROWH, g_log[idx], 1);
        }
        if (scrolls)
            c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP,
                        c.height() - ui::TOP - ui::ROWH, top_, vis, n);

        // The prompt sits on the last row under a rule, so it reads as an input
        // rather than as more output.
        const int py = c.height() - ui::ROWH;
        c.hline(0, py - 1, c.width(), 1);
        if (waiting_) {
            c.text(2, py, "running", 1);
            c.spinner(c.width() - 8, py + 1, phase_ / SPIN_MS, 1);
            return;
        }
        // Keep the END of a long command visible: that is where the cursor
        // conceptually is, and the part still being decided about.
        const int   avail = c.cols() - 3;
        const int   len   = (int)strlen(pending_);
        const char *tail  = (len > avail) ? pending_ + (len - avail) : pending_;
        char line[LOG_COLS + 4];
        snprintf(line, sizeof(line), "> %s", tail);
        c.text(2, py, line, 1);
    }

    Action on_event(Event e) override {
        if (e == EV_SELECT_HOLD) { open_keyboard(); return ui::ACT_STAY; }
        if (e == EV_SELECT)      { activate(); return ui::ACT_STAY; }
        if (e == EV_ROT_CW)      { top_++; follow_ = false; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW)     { if (top_ > 0) top_--; follow_ = false; return ui::ACT_STAY; }
        if (e == EV_BACK) {
            // BACK clears a half-typed command first. Leaving would throw it
            // away silently, and there is no other way to undo a keyboard entry
            // short of typing the whole thing again.
            if (pending_[0]) { pending_[0] = 0; return ui::ACT_STAY; }
            return ui::ACT_BACK;
        }
        return Screen::on_event(e);
    }

    const char *pending(void) const { return pending_; }

    void set_pending(const char *text) {
        nova::copy(pending_, sizeof(pending_), text ? text : "");
        trim(pending_);
        follow_ = true;
    }

    void open_keyboard(void) {
        ui::keyboard("Command", pending_, false, typed, nullptr, this);
    }

private:
    char     pending_[64];        // the keyboard's own limit, so nothing truncates
    int      top_;
    bool     follow_;             // pinned to the newest output
    bool     waiting_;
    uint32_t phase_;

    // THE KEYBOARD'S TEXT DOES NOT OUTLIVE THIS CALL. It points into the
    // keyboard, and the keyboard pops as soon as this returns.
    static void typed(void *ctx, const char *text) {
        ((ShellScreen *)ctx)->set_pending(text);
    }

    // SELECT runs what is typed. With nothing typed it offers the last few
    // commands instead, which is the gesture that otherwise did nothing at all
    // and is the common case on a device where typing costs forty detents.
    void activate(void);

    void submit(void) {
        if (waiting_ || !pending_[0]) return;
        char echo[sizeof(pending_) + 4];
        snprintf(echo, sizeof(echo), "> %s", pending_);
        log_emit_wrapped(echo);
        if (!job_start(this, pending_, g_apps_out, APPS_OUT_BYTES)) {
            log_emit_wrapped("(another command is still running)");
            return;
        }
        hist_add(pending_);
        pending_[0] = 0;
        waiting_    = true;
        phase_      = 0;
        follow_     = true;
    }

    // Fold the captured output into the transcript. Destroys g_apps_out, which is
    // this screen's to destroy once job_take has handed it over.
    static void absorb(void) {
        if (!g_apps_out[0]) {
            // An empty capture is not an empty answer. There is one capture
            // buffer in the OS, and a command that ran while something else
            // held it prints to the console and hands back nothing.
            log_emit("(no output)");
            return;
        }
        char *p = g_apps_out;
        while (*p) {
            char *e = p;
            while (*e && *e != '\n') e++;
            const bool more = (*e == '\n');
            *e = 0;
            if (e > p && e[-1] == '\r') e[-1] = 0;
            log_emit_wrapped(p);
            if (!more) break;
            p = e + 1;
        }
    }
};

// The history, as a list. Built before the push because a MenuItem array has to
// outlive the menu that points at it, and because the context is the screen
// underneath rather than anything the menu owns.
static ui::MenuItem g_hist_items[HIST_MAX];

static Action hist_choose(void *ctx, int index) {
    if (index < 0 || index >= g_hist_n) return ui::ACT_STAY;
    ((ShellScreen *)ctx)->set_pending(g_hist[index]);
    // BACK to the shell with the line ready, NOT run. A history that fires the
    // moment it is touched turns one mis-click into whatever the command was,
    // and some of the commands on this device transmit.
    return ui::ACT_BACK;
}

class HistoryMenu : public ui::Menu {
public:
    void enter(void) override { set("Recent", g_hist_items, g_hist_n); }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Choosing one fills the prompt.";
        out[1] = "SELECT on the shell runs it.";
        return 2;
    }
};

void ShellScreen::activate(void) {
    if (pending_[0]) { submit(); return; }
    if (g_hist_n == 0) { open_keyboard(); return; }
    for (int i = 0; i < g_hist_n; i++) {
        g_hist_items[i].label = g_hist[i];
        g_hist_items[i].fn    = hist_choose;
        g_hist_items[i].ctx   = this;
    }
    gui::push<HistoryMenu>();
}

// begin() rather than enter(), and the difference matters here.
//
// enter() runs again every time a screen above this one pops — including the
// keyboard. Anything set from the keyboard's callback would therefore be wiped
// by the enter() that follows it, and the command somebody had just typed would
// vanish on the way back. A screen with state that has to survive a child is
// constructed with begin(), the way the keyboard itself is.
void open_shell(void) {
    ShellScreen *s = gui::push<ShellScreen>();
    if (s) s->begin();
}

// --- Scripts ---------------------------------------------------------------------
//
// The files under /nova/scripts, and the three things worth doing to one.
//
// A script here is a file of shell command lines run in order, which is exactly
// what the OS's own `script` command already does — control flow, variables and
// all. Running one is therefore `script <path>` rather than a second
// interpreter living in this package, which would drift from core/rps.cpp the
// first time either of them changed.
//
// ONE THING DOES NOT SURVIVE THAT ROUTE: `capture NAME command` inside a script
// comes back empty. There is a single capture buffer, the fw_shell_run that
// started the script is holding it, and rps quietly falls back to running the
// line uncaptured. The script still runs and every other statement behaves; the
// variable is just blank.

#define SCRIPT_FILES_MAX 20
#define SCRIPT_NAME_MAX  28

static char g_files[SCRIPT_FILES_MAX][SCRIPT_NAME_MAX];
static int  g_files_n;
static int  g_files_sel;                   // where the cursor was, across a visit
static char g_script_name[SCRIPT_NAME_MAX];
static char g_script_path[80];
static bool g_script_gone;                 // deleted, so its menu should close
static ui::MenuItem g_file_items[SCRIPT_FILES_MAX];
static ui::MenuItem g_action_items[3];
static char g_confirm_q[64];

static void scripts_scan(void) {
    // Twice, at most. The listing is read by index, and an entry added or
    // removed between two calls shifts every index after it — so the count is
    // taken first and checked again at the end, and a listing that moved
    // underneath is read once more rather than reported with a gap in it.
    for (int attempt = 0; attempt < 2; attempt++) {
        g_files_n = 0;
        const int n = fw_dir_count(NOVA_SCRIPTS);
        if (n <= 0) return;
        for (int i = 0; i < n && g_files_n < SCRIPT_FILES_MAX; i++) {
            FwDirEntry e;
            if (fw_dir_entry(NOVA_SCRIPTS, (unsigned)i, &e) != 1) break;
            if (e.is_dir) continue;
            nova::copy(g_files[g_files_n], SCRIPT_NAME_MAX, e.name);
            g_files_n++;
        }
        if (fw_dir_count(NOVA_SCRIPTS) == n) return;
    }
}

static Action script_open(void *ctx, int index);

// One script's actions. It hosts the run as well, because a run is a job and a
// job needs somewhere to poll from — and this is the screen that is still up
// when the command comes back.
class ScriptMenu : public ui::Menu {
public:
    void enter(void) override {
        g_action_items[0].label = "Run";
        g_action_items[1].label = "View";
        g_action_items[2].label = "Delete";
        for (int i = 0; i < 3; i++) g_action_items[i].ctx = this;
        g_action_items[0].fn = act_run;
        g_action_items[1].fn = act_view;
        g_action_items[2].fn = act_delete;
        set(g_script_name, g_action_items, 3);
        waiting_ = false;
        phase_   = 0;
    }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "A script is shell command";
        out[1] = "lines, run in order.";
        out[2] = "Delete asks first.";
        return 3;
    }

    bool animating(void) const override { return waiting_; }

    bool tick(uint32_t dt) override {
        // The file is gone, so the menu of things to do to it is too. Popping
        // from tick is what the splash does; the runner re-reads the top screen
        // after every tick for exactly this.
        if (g_script_gone) { g_script_gone = false; gui::pop(); return true; }
        if (!waiting_) return false;
        phase_ += dt;
        if (job_take(this)) {
            waiting_ = false;
            if (!g_apps_out[0])
                nova::copy(g_apps_out, APPS_OUT_BYTES,
                           "The script ran.\nIts output could not\nbe captured: something\nelse held the buffer.");
            pane_show("Output");
        }
        return true;
    }

    void draw(Canvas &c) override {
        if (waiting_) {
            c.text_centred(ui::TOP + ui::ROWH, "Running", 1);
            c.text_centred(ui::TOP + 2 * ui::ROWH, g_script_name, 1);
            c.spinner(c.width() / 2 - 2, ui::TOP + 4 * ui::ROWH, phase_ / SPIN_MS, 1);
            return;
        }
        ui::Menu::draw(c);
    }

private:
    bool     waiting_;
    uint32_t phase_;

    static Action act_run(void *ctx, int) {
        ScriptMenu *m = (ScriptMenu *)ctx;
        char line[sizeof(g_script_path) + 8];
        snprintf(line, sizeof(line), "script %s", g_script_path);
        if (!job_start(m, line, g_apps_out, APPS_OUT_BYTES)) {
            ui::notice("Busy", "Another command is still running. Try again in a moment.");
            return ui::ACT_STAY;
        }
        m->waiting_ = true;
        m->phase_   = 0;
        return ui::ACT_STAY;
    }

    static Action act_view(void *, int) {
        // Straight into the shared buffer: nothing else can be using it, since
        // a job would have this screen showing its spinner instead of a menu.
        // A script longer than the buffer is shown as far as it fitted.
        const uint32_t n = fw_file_read(g_script_path, g_apps_out, APPS_OUT_BYTES - 1);
        g_apps_out[n] = 0;
        if (!n) nova::copy(g_apps_out, APPS_OUT_BYTES, "(empty, or it could not be read)");
        pane_show(g_script_name);
        return ui::ACT_STAY;
    }

    static Action act_delete(void *, int) {
        snprintf(g_confirm_q, sizeof(g_confirm_q), "Delete %s?", g_script_name);
        ui::confirm(g_confirm_q, "Delete", do_delete, nullptr);
        return ui::ACT_STAY;
    }

    static void do_delete(void *) {
        fw_file_remove(g_script_path);
        g_script_gone = true;
    }
};

static Action script_open(void *, int index) {
    if (index < 0 || index >= g_files_n) return ui::ACT_STAY;
    g_files_sel = index;
    // Copied rather than pointed at: the list is rescanned every time this
    // screen is entered, and after a delete the row this index used to name is
    // a different script.
    nova::copy(g_script_name, sizeof(g_script_name), g_files[index]);
    nova::path_join(g_script_path, sizeof(g_script_path), NOVA_SCRIPTS, g_script_name);
    gui::push<ScriptMenu>();
    return ui::ACT_STAY;
}

class ScriptsScreen : public ui::Menu {
public:
    // enter(), not begin(): this runs again every time the action menu or a
    // confirmation pops, which is what makes a deleted script leave the list
    // without anything having to remember to say so.
    void enter(void) override {
        scripts_scan();
        for (int i = 0; i < g_files_n; i++) {
            g_file_items[i].label = g_files[i];
            g_file_items[i].fn    = script_open;
            g_file_items[i].ctx   = nullptr;
        }
        set("Scripts", g_file_items, g_files_n);
        if (g_files_sel >= g_files_n) g_files_sel = g_files_n - 1;
        if (g_files_sel < 0) g_files_sel = 0;
        select(g_files_sel);
    }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT opens what can be";
        out[1] = "done with a script: run,";
        out[2] = "view, delete.";
        return 3;
    }

    void draw(Canvas &c) override {
        if (g_files_n == 0) {
            // Where they go, rather than the widget's own "Nothing here". An
            // empty list whose only message is that it is empty leaves somebody
            // with nothing to do about it.
            c.text_centred(ui::TOP + ui::ROWH, "No scripts yet", 1);
            c.text_centred(ui::TOP + 2 * ui::ROWH, NOVA_SCRIPTS, 1);
            return;
        }
        ui::Menu::draw(c);
    }

    Action on_event(Event e) override {
        const Action a = ui::Menu::on_event(e);
        g_files_sel = selected();
        return a;
    }
};

void open_scripts(void) { gui::push<ScriptsScreen>(); }

// --- App Store -----------------------------------------------------------------------
//
// The package repository, browsed on the device. `pkg` does all of the work —
// this screen runs it and reads back what it printed, which is the same text a
// person sees on the serial console.
//
// Reading a command's OUTPUT rather than fetching an index of its own is the
// whole design. The MicroPython store downloaded and parsed index.json itself,
// and so had to keep up with the repo format, the URL, the TLS roots and the
// hash checking; here every one of those is the package manager's problem and
// this screen only has to recognise a row.
//
// The rows point INTO g_apps_out, so nothing may run while the list is on screen —
// which is why the steps below are a state machine and why the list is not
// drawn at all while a job is rewriting the buffer underneath it.

#define STORE_MAX 24

struct StoreRow {
    const char *name;
    const char *ver;
    const char *desc;
};
static StoreRow g_apps_rows[STORE_MAX];
static int      g_rows_n;

// What is installed, packed as name-then-version pairs of terminated strings. A
// fixed table of twenty-four names and versions would cost twice this for a
// list that is usually four long.
static char     g_inst[480];
static unsigned g_inst_len;

// One row of `pkg search` or `pkg list`: two spaces, then a name, a version and
// — for search — a description.
//
// The field widths in pkgrepo.cpp are MINIMUMS, so a long name is not truncated
// and a fixed column offset would read the wrong thing the first time somebody
// published one. Fields are taken by whitespace instead. The two-space indent
// is what separates a row from a tagged status line ("[:] Available
// packages:"), which is more reliable than matching on the heading's text.
static void store_parse(void) {
    g_rows_n = 0;
    char *p = g_apps_out;
    while (*p && g_rows_n < STORE_MAX) {
        char *line = p;
        while (*p && *p != '\n') p++;
        char *next = *p ? p + 1 : p;
        *p = 0;
        p  = next;

        if (line[0] != ' ' || line[1] != ' ' || !line[2] || line[2] == ' ') continue;

        char *name = line + 2;
        char *q    = name;
        while (*q && *q != ' ') q++;
        if (!*q) continue;                       // a name and nothing else is not a row
        *q++ = 0;
        while (*q == ' ') q++;
        char *ver = q;
        while (*q && *q != ' ') q++;
        if (*q) { *q++ = 0; while (*q == ' ') q++; }
        if (!ver[0]) continue;

        g_apps_rows[g_rows_n].name = name;
        g_apps_rows[g_rows_n].ver  = ver;
        g_apps_rows[g_rows_n].desc = q;
        g_rows_n++;
    }
}

// Fold what `pkg list` just produced into the packed form, because the text it
// came from is about to be reused by the search.
static void inst_pack(void) {
    g_inst_len = 0;
    for (int i = 0; i < g_rows_n; i++) {
        const unsigned n = (unsigned)strlen(g_apps_rows[i].name);
        const unsigned v = (unsigned)strlen(g_apps_rows[i].ver);
        if (g_inst_len + n + v + 2 > sizeof(g_inst)) break;
        memcpy(g_inst + g_inst_len, g_apps_rows[i].name, n + 1); g_inst_len += n + 1;
        memcpy(g_inst + g_inst_len, g_apps_rows[i].ver,  v + 1); g_inst_len += v + 1;
    }
    g_rows_n = 0;
}

static const char *inst_ver(const char *name) {
    const char *p   = g_inst;
    const char *end = g_inst + g_inst_len;
    while (p < end) {
        const char *v = p + strlen(p) + 1;
        if (v >= end) break;
        if (!strcmp(p, name)) return v;
        p = v + strlen(v) + 1;
    }
    return nullptr;
}

// What the device already has, against what the repo is offering.
//
// A plain string comparison rather than a version comparison: repo_version_cmp
// lives in the firmware and is not on the ABI, and "the repo is offering
// something else" is a true statement either way. It is deliberately not called
// "newer", because this cannot tell.
static const char *store_mark(int row) {
    const char *have = inst_ver(g_apps_rows[row].name);
    if (!have) return "";
    return strcmp(have, g_apps_rows[row].ver) ? " ^" : " *";
}

class StoreScreen : public Screen {
public:
    void begin(void) {
        sel_ = top_ = 0;
        phase_ = 0;
        waiting_ = false;
        step_ = STEP_INSTALLED;
        pending_ = true;
        name_[0] = 0;
        msg_[0]  = 0;
    }

    const char *title(void) const override { return "App Store"; }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "SELECT installs the package.";
        out[1] = "* installed, ^ the repo has";
        out[2] = "a different version.";
        out[3] = "Hold SELECT to refetch.";
        return 4;
    }

    bool animating(void) const override { return waiting_ || pending_; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        if (waiting_) {
            if (!job_take(this)) return true;
            waiting_ = false;
            finished();
            return true;
        }
        if (!pending_) return false;
        // The runner may be busy with somebody else's command — the console can
        // hold the capture too — so a step keeps asking rather than failing.
        if (job_start(this, command(), g_apps_out, APPS_OUT_BYTES)) {
            pending_ = false;
            waiting_ = true;
        }
        return true;
    }

    void draw(Canvas &c) override {
        if (waiting_ || pending_) {
            c.text_centred(ui::TOP + ui::ROWH, busy_text(), 1);
            if (name_[0]) c.text_centred(ui::TOP + 2 * ui::ROWH, name_, 1);
            c.spinner(c.width() / 2 - 2, ui::TOP + 4 * ui::ROWH, phase_ / SPIN_MS, 1);
            return;
        }
        if (g_rows_n == 0) {
            c.text_centred(ui::TOP, "No package list", 1);
            static char store[96];
            const char *lines[3];
            const int n = ui::wrap(msg_[0] ? msg_ : "Nothing came back from pkg.",
                                   c.cols() - 1, store, sizeof(store), lines, 3);
            for (int i = 0; i < n; i++)
                c.text(2, ui::TOP + (i + 1) * ui::ROWH, lines[i], 1);
            c.text_centred(c.height() - ui::FH, "SELECT fetches it", 1);
            return;
        }

        // The description of whatever is under the cursor gets the bottom row,
        // so a name and a version can share a line without the one sentence
        // that says what the package IS being the thing that is dropped.
        const int rows = ui::rows_for(c) - 1;
        if (sel_ >= g_rows_n) sel_ = g_rows_n - 1;
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

        const int right = c.width() - ui::SB_W - 1;
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= g_rows_n) break;
            const int  y  = ui::TOP + i * ui::ROWH;
            const bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);

            char ver[16];
            snprintf(ver, sizeof(ver), "%s%s", g_apps_rows[idx].ver, store_mark(idx));
            const int vw = c.text_width(ver, 1, false);
            c.text_fit(3, y, g_apps_rows[idx].name, on ? 0 : 1, right - vw - 6, false);
            c.text(right - vw - 2, y, ver, on ? 0 : 1);
        }
        c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP,
                    c.height() - ui::TOP - ui::ROWH, top_, rows, g_rows_n);

        const int py = c.height() - ui::ROWH;
        c.hline(0, py - 1, c.width(), 1);
        // Narrow, because this is a sentence rather than a label and a quarter
        // more of it fits when the blank columns come out.
        c.text_fit(2, py, g_apps_rows[sel_].desc, 1, c.width() - 4, true);
    }

    Action on_event(Event e) override {
        if (waiting_ || pending_) return Screen::on_event(e);

        if (g_rows_n == 0) {
            // Nothing to browse: the one useful action is to go and get a list.
            if (e == EV_SELECT || e == EV_SELECT_HOLD) { refresh(); return ui::ACT_STAY; }
            return Screen::on_event(e);
        }
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % g_rows_n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + g_rows_n - 1) % g_rows_n; return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD) { refresh(); return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            const char *have = inst_ver(g_apps_rows[sel_].name);
            const char *verb = !have ? "Install"
                             : strcmp(have, g_apps_rows[sel_].ver) ? "Replace" : "Reinstall";
            snprintf(g_confirm_q, sizeof(g_confirm_q), "%s %s %s?",
                     verb, g_apps_rows[sel_].name, g_apps_rows[sel_].ver);
            ui::confirm(g_confirm_q, verb, install_yes, this);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    enum { STEP_INSTALLED = 0, STEP_REPO, STEP_INSTALL, STEP_UPDATE };

    int      step_;
    int      sel_, top_;
    uint32_t phase_;
    bool     waiting_;
    bool     pending_;            // the step's command has still to be started
    char     name_[24];           // what is being installed, once the rows are gone
    char     msg_[64];            // why the list is empty, in pkg's own words

    const char *command(void) {
        static char line[96];
        switch (step_) {
            case STEP_INSTALLED: return "pkg list";
            case STEP_UPDATE:    return "pkg update";
            case STEP_INSTALL:
                snprintf(line, sizeof(line), "pkg install %s", name_);
                return line;
            default:             return "pkg search";
        }
    }

    const char *busy_text(void) const {
        switch (step_) {
            case STEP_INSTALL: return "Installing";
            case STEP_UPDATE:  return "Fetching the list";
            default:           return "Reading the repo";
        }
    }

    void finished(void) {
        switch (step_) {
            case STEP_INSTALLED:
                // The installed set first, so the search can mark its rows as
                // it builds them rather than needing a second pass.
                store_parse();
                inst_pack();
                step_    = STEP_REPO;
                pending_ = true;
                break;

            case STEP_REPO:
                store_parse();
                if (g_rows_n == 0) keep_reason();
                sel_ = top_ = 0;
                name_[0] = 0;
                break;

            case STEP_UPDATE:
                step_    = STEP_INSTALLED;
                pending_ = true;
                break;

            default:                            // STEP_INSTALL
                // What pkg said, in full. It is the only place a checksum
                // mismatch, an out-of-memory or "needs an admin account" is
                // spelled out, and a one-word "failed" throws all of it away.
                if (!g_apps_out[0])
                    nova::copy(g_apps_out, APPS_OUT_BYTES,
                               "The install ran.\nIts output could not\nbe captured: something\nelse held the buffer.");
                pane_show("Install");
                // Re-read both lists on the way back, so the row picks up its
                // mark without anybody having to leave and return.
                step_    = STEP_INSTALLED;
                pending_ = true;
                break;
        }
    }

    // Why the list is empty, in pkg's own words — usually "No package list. Run
    // 'pkg update' first." on a device that has never fetched one, which is the
    // whole answer and is worth more than a guess of our own.
    void keep_reason(void) { last_line(g_apps_out, msg_, sizeof(msg_)); }

    void refresh(void) {
        step_    = STEP_UPDATE;
        pending_ = true;
        phase_   = 0;
        name_[0] = 0;
        g_rows_n = 0;
    }

    static void install_yes(void *ctx) {
        StoreScreen *s = (StoreScreen *)ctx;
        // COPIED BEFORE THE JOB STARTS. Every row points into the buffer the
        // install is about to overwrite, so the name has to leave it first —
        // and the rows are dropped in the same breath, because a row that
        // points at rewritten text draws as whatever landed there.
        nova::copy(s->name_, sizeof(s->name_), g_apps_rows[s->sel_].name);
        g_rows_n    = 0;
        s->step_    = STEP_INSTALL;
        s->pending_ = true;
        s->phase_   = 0;
    }
};

void open_store(void) {
    StoreScreen *s = gui::push<StoreScreen>();
    if (s) s->begin();
}

// --- Updates ---------------------------------------------------------------------------
//
// The device updating itself from its own screen, with no cable attached.
//
// A PACKAGE CANNOT INSTALL ITSELF. `pkg install novad1` asks apps_busy_pid
// whether anything is executing the image, finds this GUI holding it, and
// refuses — "'novad1' is running right now (task N)" — after paying for the
// whole download. Relocating over code a task is parked inside leaves the copy
// on flash and the copy in RAM permanently disagreeing, so the refusal is right
// and there is no flag that turns it off.
//
// The one moment novad1 is not resident is a MAINTENANCE BOOT. `safeboot` sets
// System.SafeBoot and restarts; main.cpp then skips pkg_load_installed,
// jobs_start_services and jobs_run_startup, and runs the staged command with
// nothing else on the machine. Nothing holds the image, and the heap is as
// unfragmented as it ever gets — which matters, because a package loads its
// read-only half in ONE allocation.
//
// So the sequence is:
//
//   1. pkg update     refresh the index. A STALE index is why an install
//                     reports a checksum mismatch that reads like corruption.
//   2. pkg list       what is installed
//   3. pkg search     what the repo is offering
//   4. update check   the same question for the firmware, read only
//   ... the person agrees ...
//   5. autonomy status, then write /nova/update.rps
//   6. safeboot script /nova/update.rps    stages it and restarts
//   7. the maintenance boot runs the script: install, then reboot
//   8. the device comes back on a normal boot, new version, service running
//
// TWO SCRIPT LINES, NOT A CHAIN. The staged command goes through
// shell_run_line_now, which is run_segment: pipes and redirection, but not `;`,
// `&&` or `||` — those are split in run_line, and only the interactive prompt
// calls that. `safeboot pkg install novad1 && reboot` asks pkg to install three
// packages, two of them named "&&" and "reboot". A .rps file gets each of its
// lines run separately, which is the thing actually wanted.
//
// AND THE REBOOT IS UNCONDITIONAL. rps throws away a command's status unless it
// is used as a condition, so the `reboot` line runs whether the install worked
// or not. That is the behaviour to want: a failed install must not leave the
// device parked in maintenance mode with a frozen panel and a login prompt only
// a serial cable can reach. It comes back on the old version, working, and the
// next start says the update did not take.
//
// THE ONE THING THAT WOULD STOP IT is a login prompt. session_boot() runs
// BEFORE the staged command in main.cpp, so a device that asks for a login
// stops there and the install never happens. That device also never starts this
// GUI on its own — jobs_start_services is after the same login — so in practice
// it is somebody who ran `d1 gui` over a cable. `autonomy status` is checked at
// the moment the update is asked for rather than up front, because it is the
// answer to "why not" and not a standing fact worth a row of a six-row panel.

#define UPD_SCRIPT NOVA_ROOT "/update.rps"
#define UPD_STAGE  "safeboot script " UPD_SCRIPT

// How long the goodbye stays up before the restart is asked for. The panel is
// the only thing this person is looking at, and a device that goes dark without
// having said why is the failure this screen exists to avoid.
#define UPD_HOLD_MS 900

// What the maintenance boot runs. Comments and all — rps skips a line whose
// first non-blank character is '#', and the next person to read this file off a
// device deserves to know what wrote it.
static const char kUpdScript[] =
    "# Nova D1 update. Runs on a maintenance boot, where nothing has loaded\n"
    "# the package and it can be replaced. The reboot is unconditional: a\n"
    "# failed install must still come back to a device somebody can use.\n"
    "pkg install novad1\n"
    "reboot\n";

// repo_version_cmp, which lives in the firmware and is not on the package ABI.
// The same rules, so the two cannot quietly disagree: components compared as
// numbers rather than as text, a missing component counting as zero so "2.1"
// and "2.1.0" are equal, and trailing junk sorting newer.
static int upd_ver_cmp(const char *a, const char *b) {
    while (*a || *b) {
        long va = 0, vb = 0;
        while (*a >= '0' && *a <= '9') { va = va * 10 + (*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { vb = vb * 10 + (*b - '0'); b++; }
        if (va != vb) return va < vb ? -1 : 1;
        if (*a == '.') a++; else if (*a) return 1;
        if (*b == '.') b++; else if (*b) return -1;
    }
    return 0;
}

// What the two versions add up to. UPD_AHEAD is a real state and not a rounding
// of "up to date": a hand-built package installed over a cable is newer than
// anything the repo has, and offering to replace it with an older one is how
// somebody loses the build they were testing.
enum { UPD_UNKNOWN = 0, UPD_CURRENT, UPD_NEWER, UPD_AHEAD, UPD_REFUSED };

static int upd_state(const char *have, const char *avail) {
    if (!have || !have[0] || !avail || !avail[0]) return UPD_UNKNOWN;
    const int c = upd_ver_cmp(avail, have);
    if (c > 0) return UPD_NEWER;
    if (c < 0) return UPD_AHEAD;
    return UPD_CURRENT;
}

// The version of a named row, after store_parse has indexed the buffer. The
// pointer is INTO g_apps_out and dies with the next command, so every caller
// copies before it runs another.
static const char *upd_row_ver(const char *name) {
    for (int i = 0; i < g_rows_n; i++)
        if (!strcmp(g_apps_rows[i].name, name)) return g_apps_rows[i].ver;
    return nullptr;
}

// --- the OS half, which is read only -------------------------------------------------
//
// `update check`'s VERDICT is read rather than recomputed, and that is not
// laziness. The OS version is frozen at v2.0.0 for the whole beta and what
// moves is a build number, so the firmware compares "2.0.0.<build>" — while the
// row it prints for the installed side reads "v2.0.0 (build 315)", with a
// leading v that any numeric comparison reads as a zeroth component. Comparing
// the two strings as printed reports an update forever. The command already did
// the comparison correctly; this reads its answer.

static int upd_os_state(const char *text) {
    if (!text || !text[0])                   return UPD_UNKNOWN;
    if (strstr(text, "already rolled back")) return UPD_REFUSED;
    if (strstr(text, "up to date"))          return UPD_CURRENT;
    if (strstr(text, "update is available")) return UPD_NEWER;
    return UPD_UNKNOWN;
}

// The value on a "  Installed  v2.0.0 (build 315)" row, terminated and trimmed.
static bool upd_os_row(const char *text, const char *label, char *out, unsigned cap) {
    out[0] = 0;
    const unsigned ll = (unsigned)strlen(label);
    for (const char *p = text; *p; ) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        if (p[0] == ' ' && p[1] == ' ' && (unsigned)(e - p) > 2 + ll &&
            !strncmp(p + 2, label, ll)) {
            const char *v = p + 2 + ll;
            while (v < e && *v == ' ') v++;
            while (e > v && e[-1] == ' ') e--;
            apps_copy_span(v, e, out, cap);
            return out[0] != 0;
        }
        p = *e ? e + 1 : e;
    }
    return false;
}

// "v2.0.0 (build 315)" as "2.0.0.315", which is the form the firmware actually
// compares and the same shape as the Available row underneath it. Two rows that
// line up are worth the dozen lines it takes; one reading as prose beside a
// version number reads as two unrelated facts.
static void upd_os_compact(const char *row, char *out, unsigned cap) {
    out[0] = 0;
    const char *p = row;
    while (*p == ' ') p++;
    if (*p == 'v' || *p == 'V') p++;
    unsigned n = 0;
    while (p[n] && p[n] != ' ' && n + 1 < cap) n++;
    memcpy(out, p, n);
    out[n] = 0;

    const char *b = strstr(row, "build ");
    if (!b) return;
    b += 6;
    unsigned m = 0;
    while (b[m] >= '0' && b[m] <= '9') m++;
    if (!m || n + 1 + m + 1 > cap) return;
    out[n] = '.';
    memcpy(out + n + 1, b, m);
    out[n + 1 + m] = 0;
}

// --- staging, shared with `novad1 selfupdate` ------------------------------------------

// Will a maintenance boot reach the staged command, as somebody who can run it?
//
// TWO HALVES, and the second is the one that is easy to miss.
//
// `autonomy status` says whether the device boots without a prompt, and names
// who it boots as. That matters because session_boot() runs BEFORE the staged
// command in main.cpp, so a device that asks for a login stops there with the
// install never run and the panel frozen on whatever it last drew.
//
//   [@] [autonomy] On, as 'root'.
//   [:] [autonomy] Off — this device asks for a login.
//   [?] [autonomy] Set to 'x', which no longer exists. It will ask instead.
//
// Then the staged lines run AS THAT PERSON, and `pkg install` and `reboot` are
// both admin-only. Nothing asks whether a named user is an admin — but `whoami`
// answers it for the current one, which is the same question as long as the two
// are the same person:
//
//   root  (admin)
//   guest
//
// So both: the same name, and an admin. The case this closes is an admin who
// logged in over a cable while autonomy names a guest — staging would succeed,
// and the maintenance boot would then refuse both lines of the script and stop
// at a prompt with not even the reboot left to get out of it.
bool update_autonomy_ok(const char *status, const char *whoami) {
    if (!status || !whoami) return false;

    const char *at = strstr(status, "On, as '");
    if (!at) return false;
    at += 8;
    const char *end = strchr(at, '\'');
    if (!end || end == at) return false;

    if (!strstr(whoami, "(admin)")) return false;
    const char *me = whoami;
    while (*me == ' ') me++;
    unsigned n = 0;
    while (me[n] && me[n] != ' ' && me[n] != '\n') n++;

    return n && n == (unsigned)(end - at) && !strncmp(me, at, n);
}

bool update_write_script(void) {
    nova::paths_init();          // /nova, on a device that has never had one
    // Not /tmp: fs_layout_check sweeps every loose file out of it at boot, and
    // this file's whole job is to still be there after a restart.
    return fw_file_write(UPD_SCRIPT, kUpdScript,
                         (uint32_t)(sizeof(kUpdScript) - 1)) != 0;
}

const char *update_stage_line(void) { return UPD_STAGE; }

// Remember what is being replaced, so the next start can say whether it took.
static void upd_remember(const char *from, const char *to) {
    nova::reg_set(NOVA_KEY_PREFIX "UpdFrom", from ? from : "");
    nova::reg_set(NOVA_KEY_PREFIX "UpdTo",   to   ? to   : "");
    nova::reg_save();            // the point of writing it is to survive the reboot
}

// What happened to the update staged last time.
//
// update_report_boot() in the firmware does this for a FIRMWARE update on the
// boot after it lands, keyed on System.Update_To. There is no equivalent for a
// package and adding one is a firmware change, so this is the package's own
// version of the same idea: two registry keys written just before the restart,
// read once at the next start, and cleared however it turned out.
//
// The cost on an ordinary start is one registry read. The `pkg list` behind it
// only happens when an update was actually staged.
void update_report_start(void) {
    // Sixteen, matching the version field pkg itself carries. A notification is
    // 48 characters and two versions plus the words between them have to fit
    // inside it without the sentence being cut in half.
    char to[16];
    nova::copy(to, sizeof(to), nova::reg(NOVA_KEY_PREFIX "UpdTo", ""));
    if (!to[0]) return;

    char from[16];
    nova::copy(from, sizeof(from), nova::reg(NOVA_KEY_PREFIX "UpdFrom", ""));

    // Safe here and nowhere else: this runs from begin(), before the loop turns
    // and before any screen exists to be holding the buffer.
    char now[16] = "";
    fw_shell_run("pkg list", g_apps_out, APPS_OUT_BYTES);
    store_parse();
    if (const char *v = upd_row_ver("novad1")) nova::copy(now, sizeof(now), v);
    g_rows_n = 0;
    g_apps_out[0] = 0;

    char line[notify::TEXT_MAX];
    if (now[0] && !strcmp(now, to))
        snprintf(line, sizeof(line), "Updated: %s to %s", from, to);
    else if (now[0])
        snprintf(line, sizeof(line), "Update to %s did not take", to);
    else
        snprintf(line, sizeof(line), "Update to %s: pkg said nothing", to);
    notify::post(line);

    upd_remember("", "");
    fw_file_remove(UPD_SCRIPT);  // spent, and a stale one would confuse the next
}

// --- the screen ------------------------------------------------------------------------

class UpdateScreen : public Screen {
public:
    void begin(void) {
        step_    = S_REFRESH;
        pending_ = true;
        waiting_ = false;
        phase_   = 0;
        hold_    = 0;
        d1_      = UPD_UNKNOWN;
        os_      = UPD_UNKNOWN;
        have_[0] = avail_[0] = 0;
        os_have_[0] = os_avail_[0] = 0;
        auto_[0] = 0;
        msg_[0]  = 0;
    }

    const char *title(void) const override { return "Updates"; }

    int help(const char **out, int max) const override {
        if (max < 5) return 0;
        out[0] = "SELECT takes the update. The";
        out[1] = "device restarts to fit it and";
        out[2] = "comes back on its own.";
        out[3] = "With none to take, SELECT reads";
        out[4] = "the whole message. Hold: recheck.";
        return 5;
    }

    // The restart is coming and BACK during it would leave the person watching
    // a screen that says nothing while the device goes down anyway. Under a
    // second and a half, and only at the very end.
    bool modal(void) const override { return step_ == S_HOLD || step_ == S_STAGE; }

    bool animating(void) const override {
        return waiting_ || pending_ || step_ == S_HOLD;
    }

    bool tick(uint32_t dt) override {
        phase_ += dt;

        if (step_ == S_HOLD) {
            hold_ += dt;
            if (hold_ < UPD_HOLD_MS) return true;
            step_    = S_STAGE;
            pending_ = true;
        }

        if (waiting_) {
            if (!job_take(this)) return true;
            waiting_ = false;
            finished();
            return true;
        }
        if (!pending_) return false;
        // The runner may be busy with somebody else's command — the Shell holds
        // the same capture — so a step keeps asking rather than giving up.
        if (job_start(this, command(), g_apps_out, APPS_OUT_BYTES)) {
            pending_ = false;
            waiting_ = true;
        }
        return true;
    }

    void draw(Canvas &c) override {
        if (step_ == S_HOLD || step_ == S_STAGE) { draw_goodbye(c); return; }

        if (waiting_ || pending_) {
            c.text_centred(ui::TOP + ui::ROWH, busy_text(), 1);
            c.spinner(c.width() / 2 - 2, ui::TOP + 3 * ui::ROWH, phase_ / SPIN_MS, 1);
            return;
        }

        row(c, 0, "Nova D1", have_[0] ? have_ : "?", false);
        row(c, 1, sub_label(d1_), d1_ == UPD_NEWER ? avail_ : "", d1_ == UPD_NEWER);
        row(c, 2, "OS", os_have_[0] ? os_have_ : "?", false);
        row(c, 3, sub_label(os_), os_ == UPD_NEWER ? os_avail_ : "", false);

        // The foot: two lines, which is what is left under four rows on the
        // shortest panel, and enough for a sentence rather than a label.
        const int fy = c.height() - 2 * ui::ROWH;
        c.hline(0, fy - 2, c.width(), 1);
        static char store[128];
        const char *lines[2];
        const int n = ui::wrap(footer(), c.cols() - 1, store, sizeof(store), lines, 2);
        for (int i = 0; i < n; i++) c.text(2, fy + i * ui::ROWH, lines[i], 1);
    }

    Action on_event(Event e) override {
        if (step_ == S_HOLD || step_ == S_STAGE) return ui::ACT_STAY;
        if (waiting_ || pending_) return Screen::on_event(e);

        if (e == EV_SELECT_HOLD) { begin(); return ui::ACT_STAY; }
        if (e == EV_SELECT && d1_ == UPD_NEWER) {
            snprintf(g_confirm_q, sizeof(g_confirm_q),
                     "Update Nova D1 to %s? It restarts.", avail_);
            ui::confirm(g_confirm_q, "Update", stage_yes, this);
            return ui::ACT_STAY;
        }
        // Nothing to install and something to explain: the foot has two lines
        // and a reason quoted back from a command is usually longer than that.
        // A sentence cut in half is worse than one that takes a press to read.
        if (e == EV_SELECT && msg_[0]) {
            ui::notice("Updates", msg_);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    // S_DONE is the resting state and has no command of its own.
    enum { S_REFRESH = 0, S_LIST, S_REPO, S_OS, S_DONE, S_ASK, S_WHO, S_HOLD, S_STAGE };

    uint32_t phase_, hold_;
    uint8_t  step_;
    int8_t   d1_, os_;
    bool     waiting_, pending_;
    char     have_[16],    avail_[16];
    char     os_have_[20], os_avail_[20];
    char     auto_[48];      // `autonomy status`, kept while `whoami` overwrites the buffer
    char     msg_[112];      // why not, in the command's own words where there are any

    const char *command(void) const {
        switch (step_) {
            case S_REFRESH: return "pkg update";
            case S_LIST:    return "pkg list";
            case S_REPO:    return "pkg search";
            case S_OS:      return "update check";
            case S_ASK:     return "autonomy status";
            case S_WHO:     return "whoami";
            default:        return UPD_STAGE;
        }
    }

    const char *busy_text(void) const {
        switch (step_) {
            case S_REFRESH: return "Fetching the list";
            case S_OS:      return "Asking about the OS";
            case S_ASK:
            case S_WHO:     return "Getting ready";
            default:        return "Checking";
        }
    }

    static const char *sub_label(int state) {
        switch (state) {
            case UPD_NEWER:   return "  update";
            case UPD_CURRENT: return "  up to date";
            case UPD_AHEAD:   return "  ahead of the repo";
            case UPD_REFUSED: return "  rolled back before";
            default:          return "  not known";
        }
    }

    void row(Canvas &c, int i, const char *label, const char *value, bool mark) {
        const int y     = ui::TOP + i * ui::ROWH;
        const int right = c.width() - 2;
        if (mark) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
        const int w = value[0] ? c.text_width(value, 1, false) : 0;
        c.text_fit(3, y, label, mark ? 0 : 1, right - w - 6, false);
        if (value[0]) c.text(right - w - 2, y, value, mark ? 0 : 1);
    }

    // One sentence, and the most useful one there is right now. A problem beats
    // an offer, because an offer made on a stale index is an offer that fails.
    //
    // TWO LINES is all there is under four rows, which is about forty
    // characters. The written ones are inside that; a message quoted back from
    // a command is not, so SELECT opens the whole of it — see on_event.
    const char *footer(void) const {
        if (msg_[0])            return msg_;
        if (d1_ == UPD_NEWER)   return "SELECT installs it, then restarts.";
        if (os_ == UPD_NEWER)   return "OS: 'update install' in Shell.";
        if (os_ == UPD_REFUSED) return "A newer OS failed to start before.";
        if (d1_ == UPD_CURRENT && os_ == UPD_CURRENT) return "Everything is up to date.";
        return "Hold SELECT to look again.";
    }

    void draw_goodbye(Canvas &c) {
        c.text_centred(ui::TOP, "Restarting", 1);
        char what[28];
        snprintf(what, sizeof(what), "to install %s", avail_);
        c.text_centred(ui::TOP + ui::ROWH, what, 1);
        static char store[128];
        const char *lines[4];
        const int n = ui::wrap("Nothing loads on the next start, so the update can "
                               "go in. It comes back on its own.",
                               c.cols() - 1, store, sizeof(store), lines, 4);
        for (int i = 0; i < n; i++)
            c.text(2, ui::TOP + (i + 2) * ui::ROWH + 2, lines[i], 1);
    }

    void finished(void) {
        switch (step_) {
            case S_REFRESH:
                // The only step that needs the network, so its failure is the
                // one worth keeping — and a stale index is why an install
                // reports a checksum mismatch that reads like corruption.
                //
                // The chain carries on regardless. A cached list is still worth
                // showing; it just cannot be trusted to be current, and saying
                // both is better than showing nothing.
                first_error(g_apps_out, msg_, sizeof(msg_));
                step_    = S_LIST;
                pending_ = true;
                break;

            case S_LIST:
                store_parse();
                if (const char *v = upd_row_ver("novad1"))
                    nova::copy(have_, sizeof(have_), v);
                g_rows_n = 0;
                step_    = S_REPO;
                pending_ = true;
                break;

            case S_REPO:
                store_parse();
                if (const char *v = upd_row_ver("novad1"))
                    nova::copy(avail_, sizeof(avail_), v);
                g_rows_n = 0;
                d1_      = (int8_t)upd_state(have_, avail_);
                if (d1_ == UPD_UNKNOWN && !msg_[0]) reason(g_apps_out, msg_, sizeof(msg_));
                step_    = S_OS;
                pending_ = true;
                break;

            case S_OS: {
                char rowbuf[48];
                if (upd_os_row(g_apps_out, "Installed", rowbuf, sizeof(rowbuf)))
                    upd_os_compact(rowbuf, os_have_, sizeof(os_have_));
                if (upd_os_row(g_apps_out, "Available", rowbuf, sizeof(rowbuf)))
                    upd_os_compact(rowbuf, os_avail_, sizeof(os_avail_));
                os_ = (int8_t)upd_os_state(g_apps_out);
                if (os_ == UPD_UNKNOWN && !msg_[0]) reason(g_apps_out, msg_, sizeof(msg_));
                step_ = S_DONE;
                break;
            }

            case S_ASK:
                // Kept, because `whoami` is about to rewrite the buffer and the
                // two answers have to be read together.
                nova::copy(auto_, sizeof(auto_), g_apps_out);
                step_    = S_WHO;
                pending_ = true;
                break;

            case S_WHO:
                if (!update_autonomy_ok(auto_, g_apps_out)) {
                    nova::copy(msg_, sizeof(msg_),
                               "The next start has to come up as this admin with no "
                               "login prompt, or the update stops there. See "
                               "'autonomy' in Shell.");
                    step_ = S_DONE;
                    break;
                }
                if (!update_write_script()) {
                    nova::copy(msg_, sizeof(msg_),
                               "The update script could not be written. Is the disk full?");
                    step_ = S_DONE;
                    break;
                }
                upd_remember(have_, avail_);
                // The goodbye goes up BEFORE safeboot is asked for, because
                // safeboot does not come back: it restarts from inside the call,
                // and a spinner is the last thing the panel would ever show.
                hold_ = 0;
                step_ = S_HOLD;
                break;

            default:            // S_STAGE
                // Being here at all means the restart did not happen — nearly
                // always an account that is not an admin, since safeboot,
                // pkg and reboot all want one.
                if (job_rc() == 0 && !msg_[0])
                    nova::copy(msg_, sizeof(msg_),
                               "The restart was staged but did not happen.");
                else
                    reason(g_apps_out, msg_, sizeof(msg_));
                upd_remember("", "");
                step_ = S_DONE;
                break;
        }
    }

    static void stage_yes(void *ctx) {
        UpdateScreen *s = (UpdateScreen *)ctx;
        // have_ and avail_ are already copies, so nothing here points into the
        // buffer the next command is about to rewrite.
        s->msg_[0]  = 0;
        s->step_    = S_ASK;
        s->pending_ = true;
        s->phase_   = 0;
    }
};

void open_updates(void) {
    UpdateScreen *s = gui::push<UpdateScreen>();
    if (s) s->begin();
}

// --- an app somebody else wrote ---------------------------------------------------------
//
// A manifest of rows, drawn as a menu, each row a shell command line.
//
// This is the whole of the third-party app framework on the screen side, and it
// is deliberately small. The interesting question was answered in novaapps.h:
// a Nova D1 screen cannot call into another package, so an app is DATA that
// this interprets rather than code that runs. What is left here is a Menu whose
// items came off the filesystem instead of out of a static array.
//
// The reach is the firmware command surface — cc1101, sx1276, bt, nfc, ibutton,
// net, fetch, reg, script, sd, pkg — which is the radios and rather more. What
// it cannot reach is the packaged tools: gpio, i2cscan, dht, ws2812. A row
// naming one of those gets the firmware's own refusal in the output pane, in
// full, rather than a summary of it invented here.

// The rows, as menu items. They point at the label and command strings inside
// napps' own text buffer, which stays put until another app is loaded — and
// only one is loaded at a time, by construction.
static ui::MenuItem g_user_items[NAPP_ROWS_MAX];

// Which app is up, and why it will not open when it will not.
static const napps::NappItem *g_user_app;
static napps::NappFault       g_user_fault;

// The command being run, copied out of the manifest.
//
// COPIED, because the pane the output goes into shares g_apps_out with
// everything else on this screen, and because a rescan while a job is in flight
// would rewrite the text the row points into. Cheaper to hold 96 bytes than to
// reason about that every time somebody adds a screen.
static char g_user_cmd[96];
static char g_user_row[24];

class UserAppScreen : public ui::Menu {
public:
    void enter(void) override {
        waiting_ = false;
        phase_   = 0;

        // enter(), not begin(): this runs again on the way back from the output
        // pane, so an app whose file was replaced underneath picks the new rows
        // up without anybody leaving the screen.
        const gui::App *a = gui::chosen();
        g_user_app   = a ? napps::by_key(a->key) : nullptr;
        g_user_fault = napps::NAPP_UNREADABLE;

        if (!g_user_app) { set("App", nullptr, 0); return; }

        const int n = napps::load(*g_user_app, &g_user_fault);
        for (int i = 0; i < n; i++) {
            const napps::NappRow *r = napps::row(i);
            g_user_items[i].label = r->label;
            g_user_items[i].fn    = run_row;
            g_user_items[i].ctx   = this;
        }
        // The app's own name, and it has to MATCH the label on the home icon —
        // a screen that titles itself something else reads as having landed
        // somewhere unintended, and novashots refuses it for that reason.
        set(g_user_app->label, g_user_items, n);
    }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Rows come from the file in";
        out[1] = NOVA_APPS_DIR ".";
        out[2] = "SELECT runs one.";
        return 3;
    }

    bool animating(void) const override { return waiting_; }

    bool tick(uint32_t dt) override {
        if (!waiting_) return false;
        phase_ += dt;
        if (job_take(this)) {
            waiting_ = false;
            if (!g_apps_out[0])
                nova::copy(g_apps_out, APPS_OUT_BYTES,
                           "The command ran.\nIts output could not\nbe captured: something\nelse held the buffer.");
            pane_show(g_user_row);
        }
        return true;
    }

    void draw(Canvas &c) override {
        if (waiting_) {
            c.text_centred(ui::TOP + ui::ROWH, "Running", 1);
            c.text_centred(ui::TOP + 2 * ui::ROWH, g_user_row, 1);
            c.spinner(c.width() / 2 - 2, ui::TOP + 4 * ui::ROWH, phase_ / SPIN_MS, 1);
            return;
        }
        if (count_ > 0) { ui::Menu::draw(c); return; }

        // WHY, not "Nothing here". An app with no rows is a file somebody wrote
        // and got wrong, and the panel is where they are looking for the reason.
        // The MicroPython suite skipped an app that would not load and said
        // nothing at all, which left the file on the device and no way in.
        c.text_centred(ui::TOP, g_user_app ? g_user_app->label : "App", 1);
        static char store[192];
        const char *lines[5];
        const int n = ui::wrap(napps::fault_text(g_user_fault), c.cols() - 1,
                               store, sizeof(store), lines, 5);
        for (int i = 0; i < n; i++)
            c.text(2, ui::TOP + (i + 1) * ui::ROWH, lines[i], 1);
    }

private:
    bool     waiting_;
    uint32_t phase_;

    static Action run_row(void *ctx, int index) {
        UserAppScreen *s = (UserAppScreen *)ctx;
        const napps::NappRow *r = napps::row(index);
        if (!r) return ui::ACT_STAY;

        nova::copy(g_user_cmd, sizeof(g_user_cmd), r->action);
        nova::copy(g_user_row, sizeof(g_user_row), r->label);

        if (!job_start(s, g_user_cmd, g_apps_out, APPS_OUT_BYTES)) {
            ui::notice("Busy", "Another command is still running. Try again in a moment.");
            return ui::ACT_STAY;
        }
        s->waiting_ = true;
        s->phase_   = 0;
        return ui::ACT_STAY;
    }
};

void open_user_app(void) { gui::push<UserAppScreen>(); }

}  // namespace screens
}  // namespace nova
