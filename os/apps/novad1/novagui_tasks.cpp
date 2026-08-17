// Desc: The Task Manager — what is running now, and what runs at every boot.
// File: novagui_tasks.cpp
//
// An upgrade of the MicroPython suite's task manager, which could list and could
// not act. Two halves, and the split between them is the whole design:
//
//   Running tasks   what `ps` shows. Stopping one of these lasts until the board
//                   next boots, and not a moment longer.
//   Services        what `service list` shows: the command lines the OS runs at
//                   every boot. Removing one is still true after a reboot.
//
// Those are different in kind, and conflating them is how somebody means the
// first and gets the second. So they are two lists, reached separately, with two
// differently worded actions — "Stop now" against "Remove from boot" — and the
// service side spends a whole screen saying what removal costs before it offers
// to do it. ui::confirm already starts on NO; that is the last guard, not the
// only one.
//
// THERE IS NO TASK-LISTING ABI. What exists is fw_shell_run, which runs a shell
// command and hands back the text it printed with the colour already stripped.
// Everything here is parsed out of four commands, and four things follow:
//
//   * THE OS HAS ONE OUTPUT CAPTURE. A second fw_shell_run while one is in
//     flight still RUNS the command and returns an EMPTY buffer. An empty buffer
//     is therefore not an empty answer, and every read here checks for it and
//     says so rather than drawing "no tasks" over a list nobody could read.
//   * A package command cannot be run through fw_shell_run from inside a package
//     command — the firmware refuses it. `ps`, `kill` and `service` are all
//     firmware commands, so nothing here shells out to `novad1 ...`.
//   * `service remove <n>` RENUMBERS everything after it. The index on the panel
//     is stale the moment any removal succeeds, so a removal re-reads the list
//     and finds its entry again. See remove_task below; that lesson cost two
//     passes in novad1cmd.cpp and there is no reason to pay for it twice.
//   * A refresh is a shell round trip. `ps` is sampled once a second, the way
//     the Resources screen samples its readings; the boot list is not sampled at
//     all, because it only changes when something changes it.
//
// The formats being parsed are cmd_ps in os/shell/ps.cpp and list_command in
// os/shell/jobs.cpp, quoted where the parsers are.
#include "novagui_tasks.h"
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

// The scheduler holds twelve slots (TASK_MAX), so `ps` can never print more
// rows than this and the table never has to decide what to drop.
constexpr int TASKS_MAX = 12;

// Eight boot entries. A device with more than eight is being configured rather
// than used, and the shell is the better tool for that — said on the screen
// rather than by silently showing the first eight.
constexpr int SVC_MAX = 8;

// Twelve rows of `ps` is about 660 characters, plus its header, rule and footer,
// plus room for a few of the stack warnings the shell interleaves. A capture
// that overflows loses the END of the listing, so this is sized generously: a
// short list reads exactly like a truncated one.
constexpr unsigned TASKS_OUT_MAX = 1152;

// One second, matching the Resources screen. See ps_sample_tick.
constexpr uint32_t REFRESH_MS = 1000;

// --- running a command without stopping the screen ------------------------------
//
// The same arrangement novagui_ops uses and for the same reason: fw_shell_run is
// synchronous, so calling it from tick() would freeze the panel with the previous
// frame still on the glass for as long as the command took. It runs on a task of
// its own and the screen polls a flag.
//
// Its own copy of the machinery rather than a shared one, because the runner in
// novagui_ops is file-static there and reaching across for it would make two
// screen files depend on each other's privates. What they DO share is the
// firmware's single capture, which is why every read here handles an empty
// buffer.

// Which command the answer in the buffer belongs to. Two screens use this runner
// and a screen must never collect a reply meant for another one — a Services
// screen parsing the output of a `kill` would find no rows and report the boot
// list as empty.
enum Op { OP_NONE = 0, OP_PS, OP_SVC_LIST, OP_KILL, OP_SVC_REMOVE };

// How a removal ended, from the task that did it. An enum rather than a message
// because the task cannot push a screen and the screen cannot see the task.
enum RmResult { RM_NONE = 0, RM_DONE, RM_GONE, RM_MOVED, RM_UNREADABLE, RM_REFUSED };

static char          g_tasks_out[TASKS_OUT_MAX];
static char          g_tasks_cmd[64];
static volatile bool g_tasks_busy;      // a command is in flight
static volatile bool g_tasks_done;      // ...and its answer is in the buffer
static volatile int  g_tasks_rc;
static volatile int  g_tasks_op;
static volatile int  g_tasks_rm;

// The command line of the service a removal is about. A COPY, not a pointer into
// g_tasks_out: the removal re-reads the listing into that same buffer, so a
// pointer would be looking at the new listing by the time it was compared
// against it.
static char g_tasks_focus_cmd[64];
static int  g_tasks_focus_idx;

// The task that runs the commands. Named, because it shows up in its own output
// and has to be filtered out again by name — see parse_ps.
#define TASKS_HELPER "d1tasks"

static int tasks_shell_task(void *arg) {
    (void)arg;
    g_tasks_rc = fw_shell_run(g_tasks_cmd, g_tasks_out, TASKS_OUT_MAX);
    // The flag LAST, after everything the reader will look at. The reader only
    // touches the buffer once it has seen this, so the order of the stores is
    // the whole of the synchronisation.
    g_tasks_busy = false;
    g_tasks_done = true;
    return 0;
}

// Removing a service is TWO round trips and they have to be consecutive, which
// is why it is a task of its own rather than two calls from a screen.
//
// Between reading the list and removing from it nothing else may renumber it, so
// the read and the remove happen back to back on one task. The index the panel
// showed is not used at all: it was correct when it was drawn and a single
// earlier removal makes it point at the next entry down.
//
// listing_index_of does the finding, because it is the function that has already
// been got wrong once and made right — it steps over the leading blanks and over
// the ANSI escapes a firmware before 2.0.1 leaves in the row. But it matches a
// SUBSTRING, so with both "novad1 gui" and "novad1 gui --bg" registered, asking
// for the shorter one finds the longer row first. The answer is read back out of
// the same listing and compared for equality before anything is removed; a
// mismatch is reported as the list having changed rather than acted on.
static bool svc_row_at(const char *listing, int want, char *cmd, unsigned cap);

static int remove_task(void *arg) {
    (void)arg;
    g_tasks_rc = 0;

    g_tasks_out[0] = 0;
    fw_shell_run("service list", g_tasks_out, TASKS_OUT_MAX);
    if (!g_tasks_out[0]) {
        // Either the command printed nothing, or the OS could not lend out its
        // one capture because something else held it. Both mean there is nothing
        // to read, and reading nothing as "the entry is gone" would remove
        // whatever happened to be first.
        g_tasks_rm = RM_UNREADABLE;
    } else {
        const int at = nova::listing_index_of(g_tasks_out, g_tasks_focus_cmd);
        char now[64];
        if (at < 0) {
            g_tasks_rm = RM_GONE;
        } else if (!svc_row_at(g_tasks_out, at, now, sizeof(now)) ||
                   strcmp(now, g_tasks_focus_cmd) != 0) {
            g_tasks_rm = RM_MOVED;
        } else {
            char line[32];
            snprintf(line, sizeof(line), "service remove %d", at);
            g_tasks_out[0] = 0;
            g_tasks_rc = fw_shell_run(line, g_tasks_out, TASKS_OUT_MAX);
            g_tasks_rm = g_tasks_rc ? RM_REFUSED : RM_DONE;
        }
    }

    g_tasks_busy = false;
    g_tasks_done = true;
    return 0;
}

// Start `line` as `op`. False when something else is still running or there was
// no task to be had — both of which the caller says out loud rather than leaving
// a screen that waits for ever.
static bool tasks_start(int op, const char *line) {
    if (g_tasks_busy) return false;
    g_tasks_out[0] = 0;
    g_tasks_rc     = 0;
    g_tasks_rm     = RM_NONE;
    g_tasks_done   = false;
    g_tasks_op     = op;
    nova::copy(g_tasks_cmd, sizeof(g_tasks_cmd), line ? line : "");
    g_tasks_busy = true;
    // Four kilobytes, the same as the GUI task. Not for what the package does
    // here — that is one ABI call and a flag — but because an ABI call runs the
    // firmware on the PACKAGE's stack, so this has to carry the shell, the
    // command, and for `service remove` a littlefs write underneath it.
    int (*body)(void *) = (op == OP_SVC_REMOVE) ? remove_task : tasks_shell_task;
    if (fw_task_spawn(TASKS_HELPER, body, nullptr, 4096) < 0) {
        g_tasks_busy = false;
        g_tasks_op   = OP_NONE;
        return false;
    }
    return true;
}

// Is the answer to `op` ready? Consumes it, so it is answered once.
static bool tasks_collect(int op) {
    if (!g_tasks_done || g_tasks_op != op) return false;
    g_tasks_done = false;
    g_tasks_op   = OP_NONE;
    return true;
}

// --- reading text ---------------------------------------------------------------

// What a read amounted to. Kept apart from "how many rows", because zero rows
// has three different causes and only one of them means the list is empty.
enum ReadState { RD_NEVER = 0, RD_OK, RD_EMPTY, RD_REFUSED };

// Why a read came back with nothing, in the firmware's own words. Shown instead
// of an empty list: `service` is an admin command and fw_shell_run runs as
// whoever is signed in, so a guest session gets a perfectly well-formed refusal
// and no rows. Drawing that as "no boot services" is the same class of wrong
// answer as reading an empty buffer as an empty device.
static char g_tasks_why[64];

static const char *line_end(const char *p) {
    while (*p && *p != '\n' && *p != '\r') p++;
    return p;
}

// strstr, bounded to one line. The unbounded one runs into the next row, which
// on a listing is a match against text that belongs to something else.
static const char *find_in(const char *p, const char *end, const char *needle) {
    const unsigned n = (unsigned)strlen(needle);
    for (; p + n <= end; p++)
        if (strncmp(p, needle, n) == 0) return p;
    return nullptr;
}

static void copy_span(char *out, unsigned cap, const char *p, const char *end) {
    unsigned n = 0;
    while (p < end && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    while (n && out[n - 1] == ' ') out[--n] = 0;
}

// The first line the shell tagged as a warning or an error, without its tag.
// fw_shell_run captures the tagged lines too — that is the difference between
// out_capture_begin and out_capture_begin_all — so the reason is in the buffer
// whenever there is one.
static bool first_complaint(const char *text, char *out, unsigned cap) {
    for (const char *p = text; *p; ) {
        const char *end = line_end(p);
        while (*p == ' ') p++;
        if (p + 4 <= end && p[0] == '[' && (p[1] == '!' || p[1] == '?') && p[2] == ']') {
            p += 3;
            while (p < end && *p == ' ') p++;
            copy_span(out, cap, p, end);
            return out[0] != 0;
        }
        p = end;
        while (*p == '\n' || *p == '\r') p++;
    }
    return false;
}

// The first line that is not blank and not tagged, without any tag. For showing
// a command's own reply — "Removed: novad1 gui --bg" — in its own words rather
// than in a second set that could drift from it.
static bool first_reply(const char *text, char *out, unsigned cap) {
    for (const char *p = text; *p; ) {
        const char *end = line_end(p);
        while (p < end && *p == ' ') p++;
        if (p < end && p[0] == '[') {
            const char *close = find_in(p, end, "] ");
            if (close) p = close + 2;
        }
        while (p < end && *p == ' ') p++;
        if (p < end) { copy_span(out, cap, p, end); if (out[0]) return true; }
        p = end;
        while (*p == '\n' || *p == '\r') p++;
    }
    return false;
}

// Work out what a listing amounted to. `empty_marker` is the text the command
// prints for a genuinely empty list — "(none)" for `service list` — which is the
// only thing that tells an empty list apart from a refused one.
static int classify(const char *text, int rows, const char *empty_marker) {
    g_tasks_why[0] = 0;
    if (!text[0]) return RD_EMPTY;
    if (rows > 0)  return RD_OK;
    if (empty_marker && strstr(text, empty_marker)) return RD_OK;
    if (first_complaint(text, g_tasks_why, sizeof(g_tasks_why))) return RD_REFUSED;
    return RD_REFUSED;
}

// --- what `ps` prints -------------------------------------------------------------
//
// cmd_ps builds every row from one format, and this is it with the colour taken
// out on the way into the capture:
//
//     "  PID  NAME         STATE  CORE STACK       CPU      "
//     "  ------------------------------------------------------------"
//     "  1    shell        run    0    1.5K/8.0K   2.1s     "
//     "  14   d1tasks      run    1    -           0ms      "
//     "[?]   pid 3 is using 84% of its stack."
//     "  4 tasks  ·  CPU 7%  ·  180 KB free"
//
// Read as TOKENS rather than as columns, because a task name is allowed fifteen
// characters and the column is twelve — a long name overruns "%-12s" and shifts
// everything after it, where a column reader would then take the state out of
// the middle of the name.
//
// A row is recognised by its STATE word, not by starting with a digit. The
// footer starts with a digit too ("  4 tasks  ·  ..."), and reading that as a
// task called "tasks" is exactly the sort of thing nobody notices until the
// list has a ghost in it. A name containing a space would shift the tokens and
// fail the same check, which loses the row rather than mis-reading it — the
// safe direction of the two.

struct TaskRow {
    int16_t pid;
    uint8_t core;
    uint8_t warn;          // the stack percentage the shell warned about, or 0
    char    name[16];      // TASK_NAME_MAX
    char    state[7];
    char    stack[14];     // "1.5K/8.0K", or "-" for the adopted boot context
    char    cpu[10];       // "2.1s" or "340ms"
};

static TaskRow g_tasks_row[TASKS_MAX];
static int     g_tasks_nrow;
static int     g_tasks_read;        // a ReadState

// The pid of the task this screen is running on, read on the UI task. It is what
// identifies the GUI's own task without guessing at a name, and it is right
// whether the screen was started in the background as a service or in the
// foreground from the shell.
static int g_tasks_self;

static const char *token(const char *p, const char *end, char *out, unsigned cap) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    unsigned n = 0;
    while (p < end && *p != ' ' && *p != '\t') { if (n + 1 < cap) out[n++] = *p; p++; }
    out[n] = 0;
    return p;
}

static bool all_digits(const char *s) {
    if (!*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

static int to_int(const char *s) {
    int v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
    return v;
}

// state_text() in ps.cpp, all of it. A word this does not know is not a state
// and the line is not a row.
static bool is_state(const char *s) {
    return !strcmp(s, "ready") || !strcmp(s, "run")  || !strcmp(s, "sleep") ||
           !strcmp(s, "block") || !strcmp(s, "done") || !strcmp(s, "-");
}

// The warning cmd_ps prints under a row using more than 80% of its stack:
//
//     out_warn("  pid %d is using %u%% of its stack.", pid, pct)
//
// Kept because it is the firmware's own judgement about a number this screen
// only has as text. Re-deriving it would mean parsing "1.5K/8.0K" back into
// bytes, and two places deciding what "nearly full" means is one too many.
static void attach_warning(const char *p, const char *end) {
    const char *at = find_in(p, end, "pid ");
    if (!at || !find_in(p, end, "of its stack")) return;
    at += 4;
    char tok[8];
    token(at, end, tok, sizeof(tok));
    if (!all_digits(tok)) return;
    const int pid = to_int(tok);

    const char *pct = find_in(at, end, "using ");
    if (!pct) return;
    pct += 6;
    char ptok[8];
    token(pct, end, ptok, sizeof(ptok));
    int v = to_int(ptok);
    if (v <= 0 || v > 100) return;

    for (int i = 0; i < g_tasks_nrow; i++)
        if (g_tasks_row[i].pid == pid) { g_tasks_row[i].warn = (uint8_t)v; return; }
}

static void parse_ps(const char *text) {
    g_tasks_nrow = 0;

    for (const char *p = text; *p; ) {
        const char *end = line_end(p);

        char pid[8], name[16], state[8], core[8], stack[16], cpu[12];
        const char *q = token(p, end, pid, sizeof(pid));
        if (all_digits(pid)) {
            q = token(q, end, name,  sizeof(name));
            q = token(q, end, state, sizeof(state));
            if (is_state(state)) {
                q = token(q, end, core,  sizeof(core));
                q = token(q, end, stack, sizeof(stack));
                (void)token(q, end, cpu, sizeof(cpu));

                // The sampler lists itself: `ps` runs INSIDE this task, so it is
                // always there and always running. Reporting the measurement as
                // one of the things measured would put a row on the panel that
                // exists only because somebody is looking at the panel.
                //
                // By name rather than by pid, because a finished task stays in
                // the table until something reaps it — so the last few samples
                // are all still listed, as `done`.
                if (strcmp(name, TASKS_HELPER) != 0 && g_tasks_nrow < TASKS_MAX) {
                    TaskRow &r = g_tasks_row[g_tasks_nrow++];
                    r.pid  = (int16_t)to_int(pid);
                    r.core = (uint8_t)to_int(core);
                    r.warn = 0;
                    nova::copy(r.name,  sizeof(r.name),  name);
                    nova::copy(r.state, sizeof(r.state), state);
                    nova::copy(r.stack, sizeof(r.stack), stack[0] ? stack : "-");
                    nova::copy(r.cpu,   sizeof(r.cpu),   cpu[0] ? cpu : "-");
                }
            }
        } else {
            attach_warning(p, end);
        }

        p = end;
        while (*p == '\n' || *p == '\r') p++;
    }

    // No empty marker: `ps` always prints its header and its footer, so it has
    // no "(none)" and any listing with no rows in it went wrong somewhere.
    g_tasks_read = classify(text, g_tasks_nrow, nullptr);
}

static TaskRow *row_for_pid(int pid) {
    for (int i = 0; i < g_tasks_nrow; i++)
        if (g_tasks_row[i].pid == pid) return &g_tasks_row[i];
    return nullptr;
}

// Sample `ps` on a timer and fold the answer in. True when the panel needs
// repainting.
//
// ONCE A SECOND, the way the Resources screen samples its readings. Every sample
// is a shell command on a task of its own — a task slot, a 4 KB stack, and a walk
// of the whole task table — to move a state word and a millisecond counter. At
// sixty frames a second the screen would spend the device on watching the device,
// and `ps` would mostly be reporting the tasks that `ps` had started.
static bool ps_sample_tick(uint32_t dt, uint32_t *acc) {
    bool changed = false;
    if (tasks_collect(OP_PS)) {
        parse_ps(g_tasks_out);
        changed = true;
    }
    *acc += dt;
    if (*acc < REFRESH_MS) return changed;
    *acc = 0;
    tasks_start(OP_PS, "ps");
    return changed;
}

// --- what `service list` prints -----------------------------------------------------
//
// list_command in jobs.cpp, with the colour taken out:
//
//     "[:] Services:"
//     "   1  novad1 gui --bg"
//     "   2  httpd --port 80"
//     "  Live state is in 'ps'; services run as ordinary tasks."
//
// and for an empty one:
//
//     "[:] Services:"
//     "  (none)"
//     "  Live state is in 'ps'; services run as ordinary tasks."
//
// Indices start at ONE, from joblist_walk's `cb(ctx, ++idx, line)`, and that is
// the number `service remove <n>` takes.
//
// This ENUMERATES; it does not search. Searching a listing for an entry is
// nova::listing_index_of's job and the removal path uses it, because that is the
// one that has been wrong before. What is here is the other direction — the row
// at a given position — which listing_index_of cannot answer and which the panel
// needs for every row it draws.

struct SvcRow {
    uint8_t index;
    char    cmd[64];
};

static SvcRow g_tasks_svc[SVC_MAX];
static int    g_tasks_nsvc;
static int    g_tasks_svc_read;     // a ReadState
static bool   g_tasks_svc_more;     // there were more entries than the table holds

// Step over leading blanks and over any ANSI escape, exactly as novacore's
// skip_blank does and for its reason: a package and the firmware under it ship
// on separate versions, and a firmware before 2.0.1 leaves the colour in the row.
// A parser that only skipped spaces would find no rows at all there, and no rows
// reads the same as no services.
static const char *skip_lead(const char *p, const char *end) {
    while (p < end) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (*p == '\033') {
            p++;
            if (p < end && *p == '[') {
                p++;
                while (p < end && !(*p >= 0x40 && *p <= 0x7e)) p++;
            }
            if (p < end) p++;
            continue;
        }
        break;
    }
    return p;
}

// One row, split into its index and its command. False when the line is not one.
static bool svc_split(const char *p, const char *end, int *index, char *cmd, unsigned cap) {
    p = skip_lead(p, end);
    if (p >= end || *p < '1' || *p > '9') return false;
    int n = 0;
    while (p < end && *p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    p = skip_lead(p, end);
    if (p >= end) return false;
    copy_span(cmd, cap, p, end);
    if (!cmd[0]) return false;
    if (index) *index = n;
    return true;
}

// The command at index `want` in a listing, for checking that an index found by
// listing_index_of still points at what it was asked about.
static bool svc_row_at(const char *listing, int want, char *cmd, unsigned cap) {
    for (const char *p = listing; *p; ) {
        const char *end = line_end(p);
        int idx = 0;
        if (svc_split(p, end, &idx, cmd, cap) && idx == want) return true;
        p = end;
        while (*p == '\n' || *p == '\r') p++;
    }
    return false;
}

static void parse_services(const char *text) {
    g_tasks_nsvc     = 0;
    g_tasks_svc_more = false;

    for (const char *p = text; *p; ) {
        const char *end = line_end(p);
        char cmd[64];
        int  idx = 0;
        if (svc_split(p, end, &idx, cmd, sizeof(cmd))) {
            if (g_tasks_nsvc < SVC_MAX) {
                SvcRow &r = g_tasks_svc[g_tasks_nsvc++];
                r.index = (uint8_t)idx;
                nova::copy(r.cmd, sizeof(r.cmd), cmd);
            } else {
                g_tasks_svc_more = true;
            }
        }
        p = end;
        while (*p == '\n' || *p == '\r') p++;
    }

    g_tasks_svc_read = classify(text, g_tasks_nsvc, "(none)");
}

// Is this the entry that starts the Nova D1's own screen? Matched on its text,
// because a service list holds command lines and nothing else — there is no
// identity to match on. `novad1 setup` registers "novad1 gui --bg", and the two
// words together are what tells it apart from `novad1 scan` in a startup list.
static bool is_screen_service(const char *cmd) {
    return strstr(cmd, "novad1") != nullptr && strstr(cmd, "gui") != nullptr;
}

// --- what was agreed to ------------------------------------------------------------
//
// A confirm's yes callback cannot be where the work happens: it runs while the
// question is still the top screen and the question pops itself immediately
// afterwards, so a callback that pushed anything would have that pop take the
// pushed screen instead of the question.
//
// So the callback only RECORDS. The screen underneath is the top one again by
// the time the next tick comes round, and it does the work then.
//
// The record is a file-static rather than a member, and that is not tidiness:
// pop() calls enter() on the screen it reveals, so a member set by the callback
// would be cleared by that enter() before any tick could read it.

enum TasksPending { TPEND_NONE = 0, TPEND_KILL, TPEND_REMOVE };

static int g_tasks_pending;
static int g_tasks_kill_pid;

static void agreed_kill(void *)   { g_tasks_pending = TPEND_KILL; }
static void agreed_remove(void *) { g_tasks_pending = TPEND_REMOVE; }

// The question itself. HELD by ConfirmScreen rather than copied, so it has to
// outlive the question — and it is built from a name, so it cannot be a literal.
static char g_tasks_ask[88];

// What the command said, for the screen that asked for it.
static char g_tasks_reply[96];

// --- the shared furniture ------------------------------------------------------------

// A full-width action bar along the bottom, with a rule above it. Filled when it
// can be pressed and outlined when it cannot, which is the same "this goes
// somewhere / that one does not" distinction the Menu draws with its arrow and
// its cross.
static void action_bar(Canvas &c, const char *label, bool live) {
    const int y = c.height() - ui::ROWH;
    c.hline(0, y - 2, c.width(), 1);
    c.rounded_rect(0, y - 1, c.width(), ui::ROWH, 1, live);
    c.text_centred_in(0, c.width(), y, label, live ? 0 : 1);
}

// A one-line footer with a rule above it, for a list that wants to say something
// about itself under it.
static void footer(Canvas &c, const char *text) {
    const int y = c.height() - ui::ROWH;
    c.hline(0, y - 2, c.width(), 1);
    c.text(2, y, text, 1);
}

// Rows a list has left once its footer is taken out.
static int tasks_list_rows(const Canvas &c) {
    const int n = ui::rows_for(c) - 1;
    return n < 1 ? 1 : n;
}

// What to draw instead of a list, when there is no list to draw. Null when the
// read was fine and the list really is empty.
static const char *trouble(int read_state) {
    switch (read_state) {
        case RD_NEVER:
            return "Reading...";
        case RD_EMPTY:
            // The command RAN. The OS has one output capture and something else
            // held it, so the answer went nowhere — which is a different thing
            // from an empty answer and has to be said as one.
            return "Could not read the reply. Something else has the output buffer.";
        case RD_REFUSED:
            return g_tasks_why[0] ? g_tasks_why : "The list could not be read.";
        default:
            return nullptr;
    }
}

// Wrapped prose, in the middle of an otherwise empty body. For the states above
// and for a screen with something to explain rather than something to list.
static char        g_tasks_wrap[224];
static const char *g_tasks_line[14];

static int wrap_into(Canvas &c, const char *text) {
    return ui::wrap(text, c.cols() - 1, g_tasks_wrap, sizeof(g_tasks_wrap),
                    g_tasks_line, 14);
}

static void draw_message(Canvas &c, const char *text) {
    const int n = wrap_into(c, text);
    for (int i = 0; i < n; i++) c.text(2, ui::TOP + i * ui::ROWH, g_tasks_line[i], 1);
}

// --- one running task -----------------------------------------------------------------
//
// The live readings for one pid, and the one thing that can be done to it.
//
// STOPPING IS "NOW", and the wording says so everywhere it appears — on the bar,
// in the question, and in the reply. A task that a boot entry starts comes back
// at the next boot and this screen cannot change that; Services can, and that is
// deliberately a different screen with a different word on its button.

class TaskScreen : public Screen {
public:
    void begin(int pid) { pid_ = pid; }

    const char *title(void) const override { return name_; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Stopping a task lasts until";
        out[1] = "the next boot. Services is";
        out[2] = "where the boot list lives.";
        return 3;
    }

    void enter(void) override {
        // Not pid_: enter() runs again every time the confirmation pops, and
        // clearing the pid there would leave the screen showing nothing about
        // nothing the moment somebody said no.
        state_ = ST_VIEW;
        acc_   = 0;
        spin_  = 0;
        g_tasks_self = fw_task_self();
        refresh_name();
    }

    bool animating(void) const override { return state_ == ST_WORK; }

    bool tick(uint32_t dt) override {
        if (state_ == ST_WORK) {
            if (tasks_collect(OP_KILL)) {
                if (!first_reply(g_tasks_out, g_tasks_reply, sizeof(g_tasks_reply)))
                    nova::copy(g_tasks_reply, sizeof(g_tasks_reply),
                               g_tasks_rc ? "It was refused."
                                          : "Asked it to stop. It stops at its next "
                                            "safe point, not this instant.");
                state_ = ST_DONE;
                return true;
            }
            acc_ += dt;
            if (acc_ < 120) return false;
            acc_ = 0;
            spin_++;
            return true;
        }
        if (state_ == ST_DONE) return false;

        if (g_tasks_pending == TPEND_KILL) {
            g_tasks_pending = TPEND_NONE;
            char line[24];
            snprintf(line, sizeof(line), "kill %d", g_tasks_kill_pid);
            if (tasks_start(OP_KILL, line)) {
                state_ = ST_WORK;
            } else {
                nova::copy(g_tasks_reply, sizeof(g_tasks_reply),
                           "Another command is still running. Try again in a moment.");
                state_ = ST_DONE;
            }
            return true;
        }

        bool changed = ps_sample_tick(dt, &acc2_);
        if (changed) refresh_name();
        return changed;
    }

    void draw(Canvas &c) override {
        if (state_ == ST_WORK) {
            c.text(2, ui::TOP + ui::ROWH, "Stopping...", 1);
            c.spinner(c.width() - 10, c.height() - 10, spin_, 1);
            return;
        }
        if (state_ == ST_DONE) {
            draw_message(c, g_tasks_reply);
            action_bar(c, "Back", true);
            return;
        }

        const TaskRow *r = row_for_pid(pid_);
        if (!r) {
            // It went while the screen was open, which is an answer rather than
            // an error — and the more likely one right after a stop.
            draw_message(c, "This task is no longer listed. It has finished or "
                            "been stopped.");
            action_bar(c, "Back", true);
            return;
        }

        // Labels at the left, values in one column, so the readings line up and
        // the screen reads as a table rather than as five sentences.
        const int vx = 6 * ui::ADV;
        int y = ui::TOP;
        char v[24];

        snprintf(v, sizeof(v), "%d", (int)r->pid);
        c.text(0, y, "pid", 1);
        c.text(vx, y, v, 1);
        snprintf(v, sizeof(v), "core %u", (unsigned)r->core);
        c.text(c.width() - c.text_width(v) - 1, y, v, 1);
        y += ui::ROWH;

        c.text(0, y, "state", 1);
        c.text(vx, y, r->state, 1);
        y += ui::ROWH;

        c.text(0, y, "stack", 1);
        c.text_fit(vx, y, r->stack, 1, c.width() - vx, false);
        y += ui::ROWH;

        c.text(0, y, "cpu", 1);
        c.text(vx, y, r->cpu, 1);
        y += ui::ROWH;

        // The last body row says what is special about this task, when anything
        // is. The shell's own stack warning first — it is the one that predicts
        // a fault rather than describing a state.
        if (r->warn) {
            snprintf(v, sizeof(v), "stack %u%% used", (unsigned)r->warn);
            c.text(0, y, v, 1);
        } else if (r->pid == g_tasks_self) {
            c.text(0, y, "this is the screen", 1);
        } else if (r->pid == 1) {
            c.text(0, y, "this is the shell", 1);
        }

        action_bar(c, bar_label(r), stoppable(r));
    }

    // Declared, not just done. While the stop is in flight this screen holds
    // BACK — and it held HOME too, in practice, only because the runner rescues
    // HOME for any screen that does not say it is modal. Saying it is modal
    // makes the two agree: both gestures wait, on purpose, for the moment it
    // takes. The Updates screen does exactly this for its staged steps.
    bool modal(void) const override { return state_ == ST_WORK; }

    Action on_event(Event e) override {
        if (state_ == ST_WORK) return ui::ACT_STAY;      // it will not be long
        if (state_ == ST_DONE) {
            if (e == EV_SELECT) return ui::ACT_BACK;
            return Screen::on_event(e);
        }
        if (e == EV_SELECT) {
            const TaskRow *r = row_for_pid(pid_);
            if (!r) return ui::ACT_BACK;
            if (!stoppable(r)) return ui::ACT_STAY;
            ask(r);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    enum { ST_VIEW = 0, ST_WORK, ST_DONE };

    int      pid_;
    int      state_;
    uint32_t acc_, acc2_, spin_;
    char     name_[16];

    void refresh_name(void) {
        const TaskRow *r = row_for_pid(pid_);
        nova::copy(name_, sizeof(name_), r ? r->name : "Task");
    }

    // The two tasks this screen will not offer to stop, and the reason is the
    // same for both: the answer would arrive on a screen that had stopped being
    // drawn. Killing the GUI's own task takes the panel with it, so there is
    // nowhere to report what happened; the firmware refuses pid 1 outright.
    // Offering an action whose outcome cannot be shown is worse than not
    // offering it, so the bar says which one this is instead.
    bool stoppable(const TaskRow *r) const {
        return r->pid != g_tasks_self && r->pid != 1;
    }

    const char *bar_label(const TaskRow *r) const {
        if (r->pid == g_tasks_self) return "stop it from the shell";
        if (r->pid == 1)            return "the shell cannot stop";
        return "Stop now";
    }

    void ask(const TaskRow *r) {
        g_tasks_kill_pid = r->pid;
        // The consequence in the question, not just the name. "Only for now" is
        // the whole difference between this and the Services screen, and the
        // question is the last place it can be said before something happens.
        snprintf(g_tasks_ask, sizeof(g_tasks_ask),
                 "Stop %s now? This does not change what runs at boot.", r->name);
        ui::confirm(g_tasks_ask, "Stop", agreed_kill, nullptr);
    }
};

// --- the running list -------------------------------------------------------------------

class RunningScreen : public Screen {
public:
    const char *title(void) const override { return "Running"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Live, sampled once a second.";
        out[1] = "Stopping a task lasts until";
        out[2] = "the next boot, and no longer.";
        return 3;
    }

    void enter(void) override {
        // THE CURSOR IS NOT RESET. enter() runs again when the detail screen
        // pops, and putting somebody back on the first task every time they
        // looked at one is the difference between a list that remembers where
        // you were and one that makes you find your place again. draw() clamps
        // it against the row count, which is the part that has to be right
        // when a task has gone since the last sample.
        //
        // A full interval, so the first tick samples straight away rather than
        // showing a second of nothing. Coming BACK from a detail or a stop lands
        // here too, and a list that took a second to notice a task had gone
        // would be showing a task that was not there.
        acc_ = REFRESH_MS;
        g_tasks_read = RD_NEVER;
        g_tasks_nrow = 0;
        g_tasks_self = fw_task_self();
    }

    bool tick(uint32_t dt) override { return ps_sample_tick(dt, &acc_); }

    void draw(Canvas &c) override {
        const char *say = trouble(g_tasks_read);
        if (say) { draw_message(c, say); return; }
        if (!g_tasks_nrow) { draw_message(c, "Nothing is running, which cannot be true."); return; }

        if (sel_ >= g_tasks_nrow) sel_ = g_tasks_nrow - 1;
        if (sel_ < 0)             sel_ = 0;

        const int rows = tasks_list_rows(c);
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
        if (top_ < 0) top_ = 0;

        const bool scrolls = g_tasks_nrow > rows;
        const int  right   = scrolls ? c.width() - (ui::SB_W + 1) : c.width();

        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= g_tasks_nrow) break;
            const TaskRow &r = g_tasks_row[idx];
            const int y = ui::TOP + i * ui::ROWH;
            const int on = (idx == sel_) ? 0 : 1;
            if (idx == sel_) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);

            char pid[8];
            snprintf(pid, sizeof(pid), "%d", (int)r.pid);
            c.text(3, y, pid, on);

            // The state at the right edge, the name in what is left. The state
            // is the same handful of short words on every row, so a fixed right
            // column reads as one; the name is what varies and gets the space.
            const int sw = c.text_width(r.state, 1, false);
            c.text(right - sw - 3, y, r.state, on);
            const int nx = 3 + 3 * ui::ADV;
            c.text_fit(nx, y, r.name, on, right - sw - 8 - nx, false);
        }

        if (scrolls)
            c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP,
                        c.height() - ui::TOP - ui::ROWH, top_, rows, g_tasks_nrow);

        char bar[24];
        snprintf(bar, sizeof(bar), "%d task%s   CPU %u%%", g_tasks_nrow,
                 g_tasks_nrow == 1 ? "" : "s", (unsigned)fw_cpu_percent());
        footer(c, bar);
    }

    Action on_event(Event e) override {
        if (!g_tasks_nrow) return Screen::on_event(e);
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % g_tasks_nrow; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + g_tasks_nrow - 1) % g_tasks_nrow; return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            TaskScreen *s = gui::push<TaskScreen>();
            // After the push, because push_commit calls enter() and enter() is
            // where the screen sets itself up.
            if (s) s->begin(g_tasks_row[sel_].pid);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int      sel_, top_;
    uint32_t acc_;
};

// --- one service ----------------------------------------------------------------------
//
// The screen the whole feature is arranged around.
//
// It would be half the size as a row on a list with a Remove option, and that is
// exactly what it must not be. Removing a boot entry is a change that is still
// there after the device has been switched off and on again, and the one it is
// most likely to be aimed at is the entry that starts this very screen. So the
// consequence is spelled out on the panel BEFORE the question, the question
// spells it out again in one line, and ui::confirm starts on No.
//
// None of that stops anybody who means it. It is meant to stop somebody who
// meant "stop it for now" and reached for the nearest thing that looked like it.

static const char *const kWhyGeneral =
    "Started at every boot. Removing it survives a reboot; stopping a task "
    "does not. To stop this only until the next boot, use Running tasks.";

static const char *const kWhyScreen =
    "This is what starts the Nova D1 screen at boot. Remove it and the device "
    "boots to a dark panel with nothing on it to say why. Only the serial shell "
    "can put it back: novad1 setup.";

class ServiceScreen : public Screen {
public:
    const char *title(void) const override { return "Service"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Turn to read the whole note.";
        out[1] = "Removing an entry is kept";
        out[2] = "after a reboot; a stop is not.";
        return 3;
    }

    void enter(void) override {
        // top_ is NOT reset. This screen is mostly text to be read to the end,
        // and enter() runs again when the confirmation it raises pops — so
        // answering No used to throw somebody back to the top of what they had
        // just scrolled through. draw() clamps it against the line count.
        state_ = ST_VIEW;
        acc_   = 0;
        spin_  = 0;
    }

    bool animating(void) const override { return state_ == ST_WORK; }

    bool tick(uint32_t dt) override {
        if (state_ == ST_WORK) {
            if (tasks_collect(OP_SVC_REMOVE)) {
                said();
                state_ = ST_DONE;
                return true;
            }
            acc_ += dt;
            if (acc_ < 120) return false;
            acc_ = 0;
            spin_++;
            return true;
        }
        if (state_ == ST_DONE) return false;

        if (g_tasks_pending == TPEND_REMOVE) {
            g_tasks_pending = TPEND_NONE;
            if (tasks_start(OP_SVC_REMOVE, "service remove")) {
                state_ = ST_WORK;
            } else {
                nova::copy(g_tasks_reply, sizeof(g_tasks_reply),
                           "Another command is still running. Nothing was removed.");
                state_ = ST_DONE;
            }
            return true;
        }
        return false;
    }

    void draw(Canvas &c) override {
        if (state_ == ST_WORK) {
            c.text(2, ui::TOP + ui::ROWH, "Removing...", 1);
            c.spinner(c.width() - 10, c.height() - 10, spin_, 1);
            return;
        }
        if (state_ == ST_DONE) {
            draw_message(c, g_tasks_reply);
            action_bar(c, "Back", true);
            return;
        }

        // The command itself at the top, under the heading rule, so what the
        // rest of the screen is about is never off the top of a scroll.
        ui::heading(c, g_tasks_focus_cmd);

        const int body_y = ui::TOP + ui::ROWH;
        const int act_y  = c.height() - ui::ROWH;
        int rows = (act_y - 3 - ui::FH - body_y) / ui::ROWH + 1;
        if (rows < 1) rows = 1;

        const int n = wrap_into(c, is_screen_service(g_tasks_focus_cmd) ? kWhyScreen
                                                                        : kWhyGeneral);
        if (top_ > n - rows) top_ = n - rows;
        if (top_ < 0)        top_ = 0;

        for (int i = 0; i < rows && top_ + i < n; i++)
            c.text(2, body_y + i * ui::ROWH, g_tasks_line[top_ + i], 1);

        if (n > rows)
            c.scrollbar(c.width() - ui::SB_W + 1, body_y, rows * ui::ROWH, top_, rows, n);

        action_bar(c, "Remove from boot", true);
    }

    // As on the task screen: the wait is deliberate, so it is declared.
    bool modal(void) const override { return state_ == ST_WORK; }

    Action on_event(Event e) override {
        if (state_ == ST_WORK) return ui::ACT_STAY;
        if (state_ == ST_DONE) {
            if (e == EV_SELECT) return ui::ACT_BACK;
            return Screen::on_event(e);
        }
        // Turning READS rather than moves a selection. There is one action on
        // this screen and it is on the bar, so the knob is free to do the thing
        // the screen is mostly for, which is being read to the end.
        if (e == EV_ROT_CW)  { top_++;               return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { if (top_) top_--;     return ui::ACT_STAY; }
        if (e == EV_SELECT)  { ask();                return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    enum { ST_VIEW = 0, ST_WORK, ST_DONE };

    int      state_, top_;
    uint32_t acc_, spin_;

    void ask(void) {
        // Both questions name the CONSEQUENCE and leave the asking to the end,
        // which is the right order when the default answer is no. Eighty
        // characters is what ConfirmScreen can wrap onto its four lines, so the
        // long version stays on the screen underneath.
        if (is_screen_service(g_tasks_focus_cmd))
            nova::copy(g_tasks_ask, sizeof(g_tasks_ask),
                       "Removing this leaves the device booting to a dark screen. "
                       "Remove it?");
        else
            nova::copy(g_tasks_ask, sizeof(g_tasks_ask),
                       "Stop this running at every boot? The change is kept after "
                       "a reboot.");
        ui::confirm(g_tasks_ask, "Remove", agreed_remove, nullptr);
    }

    // What the removal amounted to, in words that say which of the several
    // things that can happen did.
    void said(void) {
        switch (g_tasks_rm) {
            case RM_DONE:
                if (!first_reply(g_tasks_out, g_tasks_reply, sizeof(g_tasks_reply)))
                    nova::copy(g_tasks_reply, sizeof(g_tasks_reply), "Removed.");
                break;
            case RM_GONE:
                nova::copy(g_tasks_reply, sizeof(g_tasks_reply),
                           "It is already not in the boot list. Nothing was removed.");
                break;
            case RM_MOVED:
                // The entries renumber as they go, so a list that changed while
                // this screen was open would have this removing whatever moved
                // into the place. It refuses instead.
                nova::copy(g_tasks_reply, sizeof(g_tasks_reply),
                           "The boot list changed underneath. Nothing was removed - "
                           "go back and look again.");
                break;
            case RM_UNREADABLE:
                nova::copy(g_tasks_reply, sizeof(g_tasks_reply),
                           "Could not read the boot list, so nothing was removed.");
                break;
            default:
                if (!first_complaint(g_tasks_out, g_tasks_reply, sizeof(g_tasks_reply)))
                    nova::copy(g_tasks_reply, sizeof(g_tasks_reply),
                               "It was refused. Editing services needs an admin session.");
                break;
        }
    }
};

// --- the boot list ------------------------------------------------------------------------

class ServicesScreen : public Screen {
public:
    const char *title(void) const override { return "Services"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "These run at every boot.";
        out[1] = "Removing one is kept after a";
        out[2] = "reboot; stopping a task is not.";
        return 3;
    }

    void enter(void) override {
        // The cursor stays where it was, for the reason the running list gives:
        // this screen comes back through here every time the detail below it
        // pops. draw() clamps it against the count the re-read produces.
        //
        // Read once, here, rather than on a timer.
        //
        // A boot list is a file on flash. It does not change on its own, and the
        // only thing on this device that changes it while this screen is open is
        // this screen — which comes back through here, because pop() calls
        // enter() on what it reveals. Polling it once a second would be a shell
        // round trip and a flash read every second to watch a value that cannot
        // move.
        g_tasks_svc_read = RD_NEVER;
        g_tasks_nsvc     = 0;
        asked_           = false;
    }

    bool tick(uint32_t dt) override {
        (void)dt;
        if (tasks_collect(OP_SVC_LIST)) {
            parse_services(g_tasks_out);
            return true;
        }
        if (!asked_) {
            // On the tick rather than in enter(), so the "Reading..." frame
            // reaches the panel first. Starting the work on the frame that asked
            // for it means the screen underneath is still on the glass while it
            // runs, which is the "the device looks frozen" report.
            asked_ = true;
            if (!tasks_start(OP_SVC_LIST, "service list")) {
                g_tasks_svc_read = RD_EMPTY;
                return true;
            }
        }
        return false;
    }

    void draw(Canvas &c) override {
        const char *say = trouble(g_tasks_svc_read);
        if (say) { draw_message(c, say); return; }
        if (!g_tasks_nsvc) {
            draw_message(c, "Nothing runs at boot. The screen would not be here if "
                            "something did not start it, so it was started by hand.");
            return;
        }

        if (sel_ >= g_tasks_nsvc) sel_ = g_tasks_nsvc - 1;
        if (sel_ < 0)             sel_ = 0;

        const int rows = tasks_list_rows(c);
        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
        if (top_ < 0) top_ = 0;

        const bool scrolls = g_tasks_nsvc > rows;
        const int  right   = scrolls ? c.width() - (ui::SB_W + 1) : c.width();

        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= g_tasks_nsvc) break;
            const SvcRow &r = g_tasks_svc[idx];
            const int y  = ui::TOP + i * ui::ROWH;
            const int on = (idx == sel_) ? 0 : 1;
            if (idx == sel_) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
            char n[6];
            snprintf(n, sizeof(n), "%u", (unsigned)r.index);
            c.text(3, y, n, on);
            const int cx = 3 + 2 * ui::ADV;
            c.text_fit(cx, y, r.cmd, on, right - cx - 3, false);
        }

        if (scrolls)
            c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP,
                        c.height() - ui::TOP - ui::ROWH, top_, rows, g_tasks_nsvc);

        // The footer says what the list IS, every time it is drawn. It is the
        // first of the three places this screen says that a boot entry outlives
        // a reboot, and the cheapest — nobody has to press anything to see it.
        footer(c, g_tasks_svc_more ? "8 shown - more in shell" : "runs at every boot");
    }

    Action on_event(Event e) override {
        if (!g_tasks_nsvc) return Screen::on_event(e);
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % g_tasks_nsvc; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + g_tasks_nsvc - 1) % g_tasks_nsvc; return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            // The COMMAND is what the detail screen is about, not the index.
            // Indices renumber; the command line is the only stable name a boot
            // entry has.
            nova::copy(g_tasks_focus_cmd, sizeof(g_tasks_focus_cmd), g_tasks_svc[sel_].cmd);
            g_tasks_focus_idx = g_tasks_svc[sel_].index;
            gui::push<ServiceScreen>();
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int  sel_, top_;
    bool asked_;
};

// --- the two halves --------------------------------------------------------------------
//
// A door each, rather than one list with a heading in the middle of it.
//
// The two lists look alike — a number, a name, a thing that can be got rid of —
// and they are not alike at all. Keeping them on separate screens means the
// device gets to say which one somebody is in every time they arrive, in the
// status bar and in the footer, instead of relying on them having noticed a
// divider they scrolled past.

static Action open_running(void *, int) {
    gui::push<RunningScreen>();
    return ui::ACT_STAY;
}

static Action open_services(void *, int) {
    gui::push<ServicesScreen>();
    return ui::ACT_STAY;
}

static const ui::MenuItem kTasksItems[] = {
    { "Running tasks", open_running,  nullptr },
    { "Services",      open_services, nullptr },
};

#define TASKS_ROWS ((int)(sizeof(kTasksItems) / sizeof(kTasksItems[0])))

class TasksScreen : public ui::Menu {
public:
    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Running tasks: stop one now.";
        out[1] = "Services: stop one starting";
        out[2] = "at every boot. Not the same.";
        return 3;
    }

    void enter(void) override { refresh("Tasks", kTasksItems, TASKS_ROWS); }
};

void open_tasks(void) {
    // Cleared here rather than trusted: the slot this lands in has been used
    // before and bss is not re-zeroed between pushes. An agreement left over
    // from a question somebody answered on a previous visit would be carried out
    // on the first tick of this one.
    g_tasks_pending = TPEND_NONE;
    g_tasks_rm      = RM_NONE;
    gui::push<TasksScreen>();
}

}  // namespace screens
}  // namespace nova
