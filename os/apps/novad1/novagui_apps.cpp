// Desc: The Shell, Scripts and App Store screens — the three that run commands.
// File: novagui_apps.cpp
#include "novagui_apps.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"

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
    fw_shell_run(g_job_line, g_job_out, g_job_cap);
    // LAST, and after the output is written. The UI task reads the buffer only
    // once it has seen this, so the order of these two lines is what makes the
    // handover safe without a lock — the same single-writer rule the input
    // queue is built on.
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

// How long a spinner frame lasts. Slow enough to read as turning rather than
// as flickering, and it is the only thing on these screens that animates.
#define SPIN_MS 140

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
    //
    // The LAST line, not the first. A search that found the index and matched
    // nothing prints its heading before saying so, and the heading is the one
    // line that explains nothing.
    void keep_reason(void) {
        msg_[0] = 0;
        const char *best = nullptr;
        for (const char *p = g_apps_out; *p; ) {
            const char *e = p;
            while (*e && *e != '\n') e++;
            if (e > p) best = p;
            p = *e ? e + 1 : e;
        }
        if (!best) return;
        // The tag is for a terminal, not for a panel nine pixels tall.
        if (best[0] == '[' && best[1] && best[2] == ']' && best[3] == ' ') best += 4;
        unsigned n = 0;
        while (best[n] && best[n] != '\n' && n + 1 < sizeof(msg_)) n++;
        memcpy(msg_, best, n);
        msg_[n] = 0;
    }

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

}  // namespace screens
}  // namespace nova
