// FileExp — the file explorer, brought across from v1. `files`, `fm`, `explorer`.
//
// Arrow keys, Enter to open, and the same letter keys v1 had, so somebody who
// used it there does not have to learn it again. What is underneath is entirely
// different: v1 wrote its own escape sequences and called uos directly, and this
// draws into the firmware's grid and asks the shell to do the file work.
//
// ASKING THE SHELL IS THE INTERESTING DECISION. Copy, move and package install
// are commands the shell already has — streamed, so a copy never lands in RAM
// whole, and run with the SESSION's rights rather than the package's, so this
// cannot touch anything the person at the keyboard could not have touched by
// typing it themselves.
//
// It costs nothing on screen, either, because fw_shell_run with a buffer
// CAPTURES the output instead of printing it. The browser stays up, and what
// the command had to say lands in the status line. Only `edit` needs the
// terminal handed back, because it is a full screen of its own.
//
// The real cost is that a filename becomes part of a command line, and that is
// a way to be badly wrong quietly. The shell honours double quotes and has no
// escape character, so every path goes inside quotes and any name carrying a
// quote or a control character is REFUSED rather than pasted in. A file called
//     x" ; rm -r /home
// is otherwise a command, and nothing about running it would look like an error.
//
// RENAME NO LONGER PAYS THAT COST. It has a door of its own now — fw_file_rename
// (API 1.23) — so it hands the two paths across as data and a quote in a name is
// just a quote. It is the one verb here that is a pure filesystem operation with
// nothing the shell adds; copy, move and install still go through the quoting
// above, because installing a package and moving across mounts are the shell's
// to do and there is no door that would replace them.
#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

RPC_APP_VER("fileexp", "2.1");

#define FX_PATH_MAX   160
#define FX_LINE_MAX   RPC_SHELL_LINE_MAX
#define FX_FILTER_MAX 32
#define FX_STATUS_MAX 96
#define FX_COLS_MAX   104           // one screen row, with room for the widest terminal

// The TUI layer sends Insert and Delete; rpc_app.h names them now (API 1.23), so
// this no longer derives Delete as FW_KEY_UP + 9 — an offset that read correctly
// only until the key run grew and would then have pointed a keystroke somewhere
// else with nothing to say it had.

#define FX_C_DIR  2      // green
#define FX_C_HEAD 6      // cyan
#define FX_C_WARN 3      // yellow

typedef struct {
    char          name[FW_NAME_MAX];
    unsigned long size;
    unsigned char is_dir;
} FxEntry;

typedef struct {
    FxEntry      *ents;
    short        *view;              // indices into ents that pass the filter
    int           cap;               // how many entries were affordable
    int           count;             // how many the directory actually had
    int           read;              // how many were read (count clamped to cap)
    int           nview;
    int           sel, top, rows;
    int           w, h;
    unsigned long total;
    char          cwd[FX_PATH_MAX];
    char          filter[FX_FILTER_MAX];
    char          status[FX_STATUS_MAX];
} FxState;

static FxState fx;

// --- strings and paths ------------------------------------------------------

static int fx_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static char fx_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Case-insensitive, so "Photos" and "apps" sort where a person expects rather
// than in ASCII order with every capital first.
static int fx_casecmp(const char *a, const char *b) {
    while (*a && *b) {
        char x = fx_lower(*a), y = fx_lower(*b);
        if (x != y) return x < y ? -1 : 1;
        a++; b++;
    }
    if (*a == *b) return 0;
    return *a ? 1 : -1;
}

static int fx_contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (const char *h = hay; *h; h++) {
        const char *p = h, *q = needle;
        while (*p && *q && fx_lower(*p) == fx_lower(*q)) { p++; q++; }
        if (!*q) return 1;
    }
    return 0;
}

// Join, refusing rather than truncating: a path that lost its tail names some
// other file, and every action here would then act on that one instead.
static int fx_join(char *out, unsigned cap, const char *dir, const char *name) {
    unsigned n = 0;
    for (const char *p = dir; *p; p++) {
        if (n + 2 >= cap) return 0;
        out[n++] = *p;
    }
    // "/" already ends in a separator; "/home" does not.
    if (n == 0 || out[n - 1] != '/') {
        if (n + 2 >= cap) return 0;
        out[n++] = '/';
    }
    for (const char *p = name; *p; p++) {
        if (n + 1 >= cap) return 0;
        out[n++] = *p;
    }
    out[n] = 0;
    return 1;
}

// The containing directory. "/" is its own parent, which is what stops going up
// from walking off the top for ever.
static void fx_parent(char *out, unsigned cap, const char *path) {
    unsigned n = 0;
    while (path[n] && n + 1 < cap) { out[n] = path[n]; n++; }
    out[n] = 0;
    if (n <= 1) { out[0] = '/'; out[1] = 0; return; }
    if (out[n - 1] == '/') out[--n] = 0;
    while (n > 0 && out[n - 1] != '/') out[--n] = 0;
    if (n > 1) out[n - 1] = 0;
    if (out[0] == 0) { out[0] = '/'; out[1] = 0; }
}

static void fx_size_str(char *out, unsigned cap, unsigned long b) {
    if (b < 1024)          snprintf(out, cap, "%lu B", b);
    else if (b < 1048576u) snprintf(out, cap, "%lu.%lu K", b / 1024, (b % 1024) * 10 / 1024);
    else                   snprintf(out, cap, "%lu.%lu M", b / 1048576u,
                                    (b % 1048576u) * 10 / 1048576u);
}

// Is this a directory? Only ever asked about a path somebody TYPED — for an
// entry in the listing the answer already came back with it, and asking again
// walks the whole directory to count it.
static int fx_is_dir(const char *path) { return fw_dir_count(path) >= 0; }

// --- the one that matters ---------------------------------------------------
//
// Whether this name can safely be pasted into a command line. The shell honours
// double quotes and has NO escape character, so a quoted path handles spaces,
// pipes, semicolons and redirection — and a name containing a quote of its own
// closes the quoting early, after which the rest of it is command rather than
// filename. A control character is refused for the same reason: a newline ends
// the line and starts another one.
static int fx_shell_safe(const char *s) {
    if (!s || !*s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p == '"' || *p < 0x20 || *p == 0x7f) return 0;
    return 1;
}

// Build `verb "a"` or `verb "a" "b"`, refusing what cannot be quoted safely.
// Returns 0 having already put the reason where it will be read.
static void fx_note(const char *fmt, const char *a);

static int fx_build(char *out, unsigned cap, const char *verb,
                    const char *a, const char *b) {
    if (!fx_shell_safe(a) || (b && !fx_shell_safe(b))) {
        fx_note("A quote or a control character in that name - refused.", 0);
        return 0;
    }
    int n = b ? snprintf(out, cap, "%s \"%s\" \"%s\"", verb, a, b)
              : snprintf(out, cap, "%s \"%s\"", verb, a);
    if (n < 0 || (unsigned)n >= cap) {
        fx_note("That path makes too long a command line.", 0);
        return 0;
    }
    return 1;
}

// --- reading a directory ----------------------------------------------------

static void fx_sort(FxEntry *e, int n) {
    // Insertion sort. n is at most a couple of hundred and this runs once per
    // directory change, so the simple one is the right one — and it is stable,
    // which keeps equal names in the order the filesystem gave them.
    for (int i = 1; i < n; i++) {
        FxEntry key = e[i];
        int j = i - 1;
        while (j >= 0) {
            // Folders first, then by name, ignoring case.
            int after = (e[j].is_dir != key.is_dir)
                        ? (key.is_dir ? 1 : 0)
                        : (fx_casecmp(e[j].name, key.name) > 0);
            if (!after) break;
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = key;
    }
}

static void fx_apply_filter(void) {
    fx.nview = 0;
    for (int i = 0; i < fx.read; i++) {
        if (fx.filter[0] && !fx_contains(fx.ents[i].name, fx.filter)) continue;
        fx.view[fx.nview++] = (short)i;
    }
    if (fx.sel >= fx.nview) fx.sel = fx.nview - 1;
    if (fx.sel < 0) fx.sel = 0;
}

static void fx_load(void) {
    fx.read = 0;
    fx.total = 0;
    int n = fw_dir_count(fx.cwd);
    fx.count = n < 0 ? 0 : n;
    for (int i = 0; i < fx.count && fx.read < fx.cap; i++) {
        FwDirEntry d;
        if (fw_dir_entry(fx.cwd, (unsigned)i, &d) != 1) continue;
        FxEntry *e = &fx.ents[fx.read++];
        snprintf(e->name, sizeof(e->name), "%s", d.name);
        e->is_dir = d.is_dir ? 1 : 0;
        e->size   = d.is_dir ? 0 : d.size;
        if (!e->is_dir) fx.total += e->size;
    }
    fx_sort(fx.ents, fx.read);
    fx_apply_filter();
}

static void fx_goto(const char *path) {
    snprintf(fx.cwd, sizeof(fx.cwd), "%s", path);
    fx.filter[0] = 0;
    fx.sel = 0;
    fx.top = 0;
    fx_load();
}

// The entry under the cursor, or null when the listing is empty.
static FxEntry *fx_current(void) {
    if (fx.nview <= 0 || fx.sel < 0 || fx.sel >= fx.nview) return 0;
    return &fx.ents[fx.view[fx.sel]];
}

static void fx_note(const char *fmt, const char *a) {
    if (a) snprintf(fx.status, sizeof(fx.status), fmt, a);
    else   snprintf(fx.status, sizeof(fx.status), "%s", fmt);
}

// --- drawing ----------------------------------------------------------------

static void fx_draw(void) {
    fw_tui_clear();

    char title[64];
    if (fx.filter[0]) snprintf(title, sizeof(title), " Files  [/%s] ", fx.filter);
    else              snprintf(title, sizeof(title), " Files ");
    fw_tui_box(0, 0, fx.w, fx.h - 2, title, FW_ATTR_NORMAL, FX_C_HEAD);
    fw_tui_text(2, 1, fx.cwd, FW_ATTR_BOLD, FX_C_HEAD);

    if (fx.nview == 0)
        fw_tui_text(4, 3, fx.filter[0] ? "(nothing matches)" : "(empty folder)",
                    FW_ATTR_DIM, 0);

    for (int r = 0; r < fx.rows; r++) {
        int i = fx.top + r;
        if (i >= fx.nview) break;
        FxEntry *e = &fx.ents[fx.view[i]];
        int y = r + 3;
        int selected = (i == fx.sel);
        unsigned char attr = selected ? FW_ATTR_REVERSE : FW_ATTR_NORMAL;

        // Fill first so the highlight reaches the full width rather than
        // stopping where the name does.
        fw_tui_fill(2, y, fx.w - 4, 1, ' ', attr, 0);

        char row[FW_NAME_MAX + 4];
        snprintf(row, sizeof(row), "%s%s", e->name, e->is_dir ? "/" : "");
        fw_tui_text(3, y, row, attr,
                    (unsigned char)(selected ? 0 : (e->is_dir ? FX_C_DIR : 0)));

        char meta[16];
        if (e->is_dir) snprintf(meta, sizeof(meta), "DIR");
        else           fx_size_str(meta, sizeof(meta), e->size);
        int mx = fx.w - 4 - (int)strlen(meta);
        if (mx > 4 + (int)strlen(row)) fw_tui_text(mx, y, meta, attr, 0);
    }

    // A scrollbar, so a long folder shows where in it you are.
    if (fx.nview > fx.rows) {
        int thumb = (fx.top * fx.rows) / fx.nview;
        for (int r = 0; r < fx.rows; r++)
            fw_tui_text(fx.w - 2, r + 3, r == thumb ? "#" : "|", FW_ATTR_DIM, 0);
    }

    char foot[FX_STATUS_MAX], tot[16];
    fx_size_str(tot, sizeof(tot), fx.total);
    // A listing quietly missing its tail is the same kind of lie as a truncated
    // path, so the shortfall is said out loud rather than left to be noticed.
    if (fx.count > fx.read)
        snprintf(foot, sizeof(foot), " %d of %d items - too many to hold at once   %s",
                 fx.read, fx.count, tot);
    else
        snprintf(foot, sizeof(foot), " %d items   %s", fx.nview, tot);
    fw_tui_text(1, fx.h - 3, foot, FW_ATTR_DIM, 0);

    fw_tui_fill(0, fx.h - 2, fx.w, 1, ' ', FW_ATTR_DIM, 0);
    fw_tui_text(0, fx.h - 2,
                " Enter open  n new  R rename  c copy  m move  d del"
                "  / find  v view  p install  g goto  q quit",
                FW_ATTR_DIM, 0);

    fw_tui_fill(0, fx.h - 1, fx.w, 1, ' ', FW_ATTR_NORMAL, 0);
    if (fx.status[0]) fw_tui_text(1, fx.h - 1, fx.status, FW_ATTR_NORMAL, FX_C_WARN);
    fw_tui_present();
}

// Ask the terminal how big it is and lay the list out to fit.
static void fx_relayout(void) {
    fw_tui_size(&fx.w, &fx.h);
    if (fx.w < 20) { fx.w = 80; fx.h = 24; }
    if (fx.w > FX_COLS_MAX) fx.w = FX_COLS_MAX;
    fx.rows = fx.h - 6;
    if (fx.rows < 1) fx.rows = 1;
}

static void fx_scroll_to_sel(void) {
    if (fx.sel < fx.top) fx.top = fx.sel;
    else if (fx.sel >= fx.top + fx.rows) fx.top = fx.sel - fx.rows + 1;
    int max_top = fx.nview - fx.rows;
    if (max_top < 0) max_top = 0;
    if (fx.top > max_top) fx.top = max_top;
    if (fx.top < 0) fx.top = 0;
}

// --- input ------------------------------------------------------------------

// Wait for one event. It sleeps rather than spins: this is a foreground app on
// a cooperative scheduler, and without the sleep nothing else gets a turn.
static int fx_wait(FwTuiEvent *e) {
    for (;;) {
        if (fw_tui_poll(e)) return 1;
        if (fw_task_should_stop()) return 0;      // Ctrl+C, or a kill
        fw_task_sleep_ms(5);
    }
}

// A line typed at the bottom of the screen. Returns 1 on Enter, 0 on Escape or
// Ctrl+C — which is the difference between "" meaning "clear the filter" and ""
// meaning "changed my mind", and for a delete prompt that is the whole question.
static int fx_prompt(const char *label, char *out, unsigned cap) {
    unsigned n = 0;
    out[0] = 0;
    for (;;) {
        char line[FX_COLS_MAX + FX_PATH_MAX];
        snprintf(line, sizeof(line), "%s %s", label, out);
        fw_tui_fill(0, fx.h - 1, fx.w, 1, ' ', FW_ATTR_REVERSE, 0);
        fw_tui_text(0, fx.h - 1, line, FW_ATTR_REVERSE, 0);
        fw_tui_present();

        FwTuiEvent e;
        if (!fx_wait(&e)) { out[0] = 0; return 0; }
        if (e.kind != 1) continue;
        if (e.key == 13 || e.key == 10) return 1;
        if (e.key == FW_KEY_ESC || e.key == 3) { out[0] = 0; return 0; }
        if (e.key == 127 || e.key == 8) { if (n) out[--n] = 0; continue; }
        if (e.key >= 32 && e.key < 127 && n + 1 < cap) { out[n++] = (char)e.key; out[n] = 0; }
    }
}

static int fx_confirm(const char *question) {
    char answer[8];
    if (!fx_prompt(question, answer, sizeof(answer))) return 0;
    return answer[0] == 'y' || answer[0] == 'Y';
}

// --- talking to the shell ---------------------------------------------------

// The last line the command printed, which for a failure is the reason and for
// a success is usually the confirmation. Leading spaces come off so it reads as
// a status line rather than an indented one.
static void fx_last_line(char *out, unsigned cap, const char *text) {
    const char *best = 0;
    for (const char *p = text; *p; ) {
        const char *start = p;
        while (*p && *p != '\n') p++;
        int blank = 1;
        for (const char *q = start; q < p; q++) if (*q != ' ' && *q != '\t') blank = 0;
        if (!blank) best = start;
        if (*p) p++;
    }
    if (!best) { out[0] = 0; return; }
    while (*best == ' ' || *best == '\t') best++;
    unsigned n = 0;
    while (best[n] && best[n] != '\n' && n + 1 < cap) { out[n] = best[n]; n++; }
    out[n] = 0;
}

// Run a command with its output CAPTURED, so nothing reaches the terminal and
// the browser stays on screen. What it had to say goes in the status line.
static void fx_run(const char *line, const char *done) {
    char reply[256];
    reply[0] = 0;
    fw_shell_run(line, reply, sizeof(reply));
    char summary[FX_STATUS_MAX];
    fx_last_line(summary, sizeof(summary), reply);
    // A command can succeed silently, and the capture is a single slot that
    // another task may hold — either way there is nothing to show, so say what
    // was asked for rather than leaving the line blank.
    fx_note(summary[0] ? summary : done, 0);
}

// Run something that owns the whole screen, which today means the editor.
//
// The terminal layer refuses to start twice, so it has to be given BACK first —
// otherwise the editor draws over the browser and, worse, ends by turning mouse
// reporting off underneath it. The terminal then sends escape sequences to the
// shell for every click, which looks like the device typing by itself.
static void fx_run_fullscreen(const char *line) {
    fw_tui_end();
    fw_shell_run(line, 0, 0);
    fw_tui_begin();
    fx_relayout();
}

// --- the viewer -------------------------------------------------------------
//
// A window on the file rather than the whole of it. v1 read 8 KB and stopped;
// fw_file_read_at means a big file can be walked a screen at a time instead,
// and nothing more than one bufferful is ever held.
#define FX_VIEW_BYTES 2048

static void fx_view(const char *path) {
    char *buf = (char *)fw_malloc(FX_VIEW_BYTES + 1);
    if (!buf) { fx_note("Not enough memory to view that.", 0); return; }
    unsigned long off = 0;
    unsigned long size = fw_file_size(path);

    for (;;) {
        unsigned got = fw_file_read_at(path, (unsigned)off, buf, FX_VIEW_BYTES);
        buf[got] = 0;

        fw_tui_clear();
        fw_tui_box(0, 0, fx.w, fx.h - 1, " View ", FW_ATTR_NORMAL, FX_C_HEAD);
        fw_tui_text(2, 1, path, FW_ATTR_BOLD, 0);

        // Clipped, not wrapped. A wrapped line makes one long line look like
        // several, which for a config file is actively misleading.
        int y = 3;
        unsigned i = 0, shown = 0;
        while (y < fx.h - 2 && i < got) {
            char line[FX_COLS_MAX];
            unsigned n = 0;
            while (i < got && buf[i] != '\n' && n + 1 < sizeof(line)) {
                char c = buf[i++];
                // A control character drawn literally moves the cursor about; a
                // dot in its place keeps the layout honest.
                line[n++] = (c >= 32 && c < 127) ? c : '.';
            }
            line[n] = 0;
            while (i < got && buf[i] != '\n') i++;      // the rest of a long line
            if (i < got && buf[i] == '\n') i++;
            fw_tui_text(2, y++, line, FW_ATTR_NORMAL, 0);
            shown = i;
        }
        if (shown == 0) shown = got;

        char foot[80];
        snprintf(foot, sizeof(foot),
                 " %lu of %lu bytes   PgDn/PgUp move   q returns",
                 off + shown, size);
        fw_tui_fill(0, fx.h - 1, fx.w, 1, ' ', FW_ATTR_REVERSE, 0);
        fw_tui_text(0, fx.h - 1, foot, FW_ATTR_REVERSE, 0);
        fw_tui_present();

        FwTuiEvent e;
        if (!fx_wait(&e)) break;
        if (e.kind == 2) {
            if (e.mouse == 3) off = off > 512 ? off - 512 : 0;
            else if (e.mouse == 4 && off + shown < size) off += 512;
            continue;
        }
        if (e.kind != 1) continue;
        if (e.key == 'q' || e.key == FW_KEY_ESC || e.key == 3) break;
        else if (e.key == FW_KEY_PGDN || e.key == ' ') { if (off + shown < size) off += shown; }
        else if (e.key == FW_KEY_PGUP) off = off > shown ? off - shown : 0;
        else if (e.key == FW_KEY_DOWN) { if (off + shown < size) off += 256; }
        else if (e.key == FW_KEY_UP)   off = off > 256 ? off - 256 : 0;
        else if (e.key == FW_KEY_HOME) off = 0;
    }
    fw_free(buf);
}

// --- the actions ------------------------------------------------------------

static void fx_act_open(void) {
    FxEntry *e = fx_current();
    if (!e) return;
    char target[FX_PATH_MAX];
    if (!fx_join(target, sizeof(target), fx.cwd, e->name)) {
        fx_note("That path is too long to open.", 0);
        return;
    }
    if (e->is_dir) { fx_goto(target); return; }

    char line[FX_LINE_MAX];
    if (!fx_build(line, sizeof(line), "edit", target, 0)) return;
    fx_run_fullscreen(line);
    fx_load();                      // the size on screen may have changed
}

static void fx_act_new(void) {
    char name[FW_NAME_MAX];
    if (!fx_prompt("New (end the name with / for a folder):", name, sizeof(name))) return;
    if (!name[0]) return;

    int want_dir = 0;
    unsigned n = (unsigned)strlen(name);
    while (n && name[n - 1] == '/') { name[--n] = 0; want_dir = 1; }
    if (!n) return;

    char target[FX_PATH_MAX];
    if (!fx_join(target, sizeof(target), fx.cwd, name)) {
        fx_note("That name makes too long a path.", 0);
        return;
    }
    if (fw_file_exists(target)) { fx_note("'%s' is already there.", name); return; }

    if (want_dir) {
        fx_note(fw_mkdir(target) ? "Created folder '%s'." : "Could not create '%s'.", name);
        fx_load();
        return;
    }
    // A new file starts empty and opens in the editor, the way v1 did it.
    if (!fw_file_write(target, "", 0)) { fx_note("Could not create '%s'.", name); return; }
    char line[FX_LINE_MAX];
    if (fx_build(line, sizeof(line), "edit", target, 0)) fx_run_fullscreen(line);
    fx_note("Created '%s'.", name);
    fx_load();
}

static void fx_act_rename(void) {
    FxEntry *e = fx_current();
    if (!e) return;
    char label[FW_NAME_MAX + 24], to[FW_NAME_MAX];
    snprintf(label, sizeof(label), "Rename '%s' to:", e->name);
    if (!fx_prompt(label, to, sizeof(to))) return;
    if (!to[0] || fx_streq(to, e->name)) return;

    // A rename is IN PLACE: the new name carries no path of its own. That is the
    // shell `rename` rule too — a name with a slash in it is a move, and `m` does
    // that — and refusing it here is what keeps the destination in this folder.
    for (const char *p = to; *p; p++)
        if (*p == '/') { fx_note("A new name cannot contain '/' - use m to move.", 0); return; }

    // Straight through the ABI (API 1.23), not a `rename "..."` line handed to
    // the shell. THIS is the change that matters: the destination is built here
    // and the two paths go to fw_file_rename as data, so a name carrying a quote
    // or a semicolon is a name and never something the shell could read as a
    // command. Copy, move and install still go through fx_build's quoting,
    // because they have no door of their own — but rename no longer does.
    char from[FX_PATH_MAX], dst[FX_PATH_MAX];
    if (!fx_join(from, sizeof(from), fx.cwd, e->name)) return;
    if (!fx_join(dst, sizeof(dst), fx.cwd, to)) {
        fx_note("That name makes too long a path.", 0);
        return;
    }
    if (fw_file_rename(from, dst)) fx_note("Renamed to '%s'.", to);
    else                          fx_note("Could not rename '%s'.", e->name);
    fx_load();
}

// Copy and move differ in their verb and in whether a folder is allowed, and in
// nothing else. `shell_verb` is separate from `label` so the two never drift.
static void fx_act_transfer(const char *label, const char *shell_verb, int allow_dir) {
    FxEntry *e = fx_current();
    if (!e) return;
    if (e->is_dir && !allow_dir) {
        fx_note("Copy is for files - cp does not recurse into a folder.", 0);
        return;
    }
    char prompt[FW_NAME_MAX + 48], dest[FX_PATH_MAX];
    snprintf(prompt, sizeof(prompt), "%s '%s' to (folder or path):", label, e->name);
    if (!fx_prompt(prompt, dest, sizeof(dest))) return;
    if (!dest[0]) return;

    // A destination that is a folder means "into it" — which is what anybody
    // typing a folder name means, and it saves typing the name twice.
    char target[FX_PATH_MAX];
    if (fx_is_dir(dest)) {
        if (!fx_join(target, sizeof(target), dest, e->name)) {
            fx_note("That destination makes too long a path.", 0);
            return;
        }
    } else {
        snprintf(target, sizeof(target), "%s", dest);
    }

    char from[FX_PATH_MAX], line[FX_LINE_MAX];
    if (!fx_join(from, sizeof(from), fx.cwd, e->name)) return;
    if (!fx_build(line, sizeof(line), shell_verb, from, target)) return;
    fx_run(line, "Done.");
    fx_load();
}

static void fx_act_delete(void) {
    FxEntry *e = fx_current();
    if (!e) return;
    char q[FW_NAME_MAX + 32], target[FX_PATH_MAX];
    snprintf(q, sizeof(q), "Delete '%s'? (y/N):", e->name);
    if (!fx_confirm(q)) { fx_note("Cancelled.", 0); return; }
    if (!fx_join(target, sizeof(target), fx.cwd, e->name)) return;

    // Straight through the ABI rather than the shell: this is the one operation
    // with a door of its own, and it removes a file or an EMPTY folder — the
    // same rule v1 had, and the reason a folder with things in it has to be
    // emptied first rather than disappearing on one keystroke.
    int was_dir = e->is_dir;
    char name[FW_NAME_MAX];
    snprintf(name, sizeof(name), "%s", e->name);
    if (fw_file_remove(target))  fx_note("Deleted '%s'.", name);
    else if (was_dir)            fx_note("'%s' is not empty - empty it first.", name);
    else                         fx_note("Could not delete '%s'.", name);
    fx_load();
}

static void fx_act_install(void) {
    FxEntry *e = fx_current();
    if (!e) return;
    unsigned n = (unsigned)strlen(e->name);
    // v2's packages are .app files where v1's were .pkg. Same key, same idea.
    int is_app = n > 4 && fx_streq(e->name + n - 4, ".app");
    if (e->is_dir || !is_app) { fx_note("Pick a .app file to install.", 0); return; }

    char target[FX_PATH_MAX], line[FX_LINE_MAX];
    if (!fx_join(target, sizeof(target), fx.cwd, e->name)) return;
    if (!fx_build(line, sizeof(line), "pkg install", target, 0)) return;
    fx_run(line, "Installed.");
    fx_load();
}

static void fx_act_goto(void) {
    char dest[FX_PATH_MAX];
    if (!fx_prompt("Go to:", dest, sizeof(dest))) return;
    if (!dest[0]) return;
    if (fx_is_dir(dest)) fx_goto(dest);
    else                 fx_note("Not a folder: %s", dest);
}

static void fx_act_find(void) {
    char f[FX_FILTER_MAX];
    if (!fx_prompt("Find (blank clears):", f, sizeof(f))) return;
    snprintf(fx.filter, sizeof(fx.filter), "%s", f);
    fx.sel = 0;
    fx.top = 0;
    fx_apply_filter();
    if (fx.filter[0]) fx_note("Showing names containing '%s'.", fx.filter);
    else              fx_note("Filter cleared.", 0);
}

static void fx_act_up(void) {
    char up[FX_PATH_MAX];
    fx_parent(up, sizeof(up), fx.cwd);
    if (!fx_streq(up, fx.cwd)) fx_goto(up);
}

// --- the loop ---------------------------------------------------------------

// How many entries to hold. A directory of a few hundred is unusual on a device
// with this much flash; the smaller sizes are there so a fragmented heap gets a
// smaller browser rather than none at all.
static const int fx_caps[] = { 192, 96, 48 };

static int fx_alloc(void) {
    for (unsigned i = 0; i < sizeof(fx_caps) / sizeof(fx_caps[0]); i++) {
        int n = fx_caps[i];
        fx.ents = (FxEntry *)fw_malloc((unsigned)n * sizeof(FxEntry));
        if (!fx.ents) continue;
        fx.view = (short *)fw_malloc((unsigned)n * sizeof(short));
        if (!fx.view) { fw_free(fx.ents); fx.ents = 0; continue; }
        fx.cap = n;
        return 1;
    }
    return 0;
}

static void fx_usage(void) {
    fw_printf("files / fm / explorer - browse the filesystem.\n\n");
    fw_printf("  files [path]   open the browser (default: /)\n\n");
    fw_printf("  arrows or j/k  move          Enter or l   open\n");
    fw_printf("  left or h      go up         v            view without the editor\n");
    fw_printf("  n new          R rename      c copy       m move\n");
    fw_printf("  d delete       / find        g go to      p install a .app\n");
    fw_printf("  r refresh      ^L redraw     q or Esc     quit\n");
}

static int fx_cmd(int argc, char **argv) {
    if (argc > 1 && (fx_streq(argv[1], "help") || fx_streq(argv[1], "-h") ||
                     fx_streq(argv[1], "--help") || fx_streq(argv[1], "?"))) {
        fx_usage();
        return 0;
    }

    const char *start = argc > 1 ? argv[1] : "/";
    if (!fx_is_dir(start)) {
        if (argc > 1) fw_printf("'%s' is not a folder - starting at /.\n", start);
        start = "/";
    }
    if (!fx_alloc()) {
        fw_printf("The heap has no block big enough for a listing right now.\n");
        return 1;
    }

    fx.status[0] = 0;
    fw_tui_begin();
    fx_relayout();
    fx_goto(start);

    int running = 1;
    while (running) {
        fx_scroll_to_sel();
        fx_draw();
        fx.status[0] = 0;

        FwTuiEvent e;
        if (!fx_wait(&e)) break;                 // Ctrl+C, or the task was killed

        if (e.kind == 2) {                       // mouse
            int max_top = fx.nview - fx.rows;
            if (max_top < 0) max_top = 0;
            if (e.mouse == 3) { fx.top -= 3; if (fx.top < 0) fx.top = 0; }
            else if (e.mouse == 4) { fx.top += 3; if (fx.top > max_top) fx.top = max_top; }
            else if (e.mouse == 0) {
                int row = (int)e.y - 3;
                if (row >= 0 && row < fx.rows && fx.top + row < fx.nview)
                    fx.sel = fx.top + row;
            }
            continue;
        }
        if (e.kind != 1) continue;

        switch (e.key) {
            case 'q': case 3: case FW_KEY_ESC: running = 0; break;

            case FW_KEY_UP:   case 'k':
                if (fx.nview) fx.sel = (fx.sel + fx.nview - 1) % fx.nview;
                break;
            case FW_KEY_DOWN: case 'j':
                if (fx.nview) fx.sel = (fx.sel + 1) % fx.nview;
                break;
            case FW_KEY_HOME: fx.sel = 0; break;
            case FW_KEY_END:  fx.sel = fx.nview ? fx.nview - 1 : 0; break;
            case FW_KEY_PGUP: fx.sel -= fx.rows; if (fx.sel < 0) fx.sel = 0; break;
            case FW_KEY_PGDN: fx.sel += fx.rows;
                              if (fx.sel >= fx.nview) fx.sel = fx.nview ? fx.nview - 1 : 0;
                              break;

            case FW_KEY_LEFT:  case 'h': case 127: case 8: fx_act_up();   break;
            case FW_KEY_RIGHT: case 'l': case 13:  case 10: fx_act_open(); break;

            case 'v': {
                FxEntry *c = fx_current();
                char p[FX_PATH_MAX];
                if (c && !c->is_dir && fx_join(p, sizeof(p), fx.cwd, c->name)) fx_view(p);
            } break;

            case '/': fx_act_find(); break;
            case 'g': fx_act_goto(); break;
            case 'n': fx_act_new(); break;
            case 'R': fx_act_rename(); break;
            case 'c': fx_act_transfer("Copy", "cp", 0); break;
            case 'm': fx_act_transfer("Move", "mv", 1); break;
            case 'p': fx_act_install(); break;
            case 'd': case FW_KEY_DELETE: fx_act_delete(); break;
            case 'r': fx_load(); fx_note("Refreshed.", 0); break;

            case 12:                                   // Ctrl+L, after a resize
                if (fw_tui_refresh()) fx_relayout();
                break;
            default: break;
        }
    }

    // ALWAYS. A terminal left in mouse-reporting mode sends escape sequences to
    // the shell for every click afterwards, which looks like the device has
    // started typing by itself.
    fw_tui_end();
    fw_free(fx.ents);
    fw_free(fx.view);
    fx.ents = 0;
    fx.view = 0;
    fw_printf("Closed the file browser.\n");
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("files", "browse the filesystem", fx_cmd);
    // v1's other two spellings. Somebody who typed `fm` for a year should not
    // have to find out it is called something else now.
    rpc_register_command("fm", "browse the filesystem", fx_cmd);
    rpc_register_command("explorer", "browse the filesystem", fx_cmd);
    return 0;
}
