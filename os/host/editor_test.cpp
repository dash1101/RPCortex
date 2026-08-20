// The editor's paged text store, and the editing that runs on it.
//
// The editor used to hold its text as one 80,000-byte block. A board with Nova
// D1 resident has plenty of heap free but never that much in one piece, so the
// single allocation failed and `edit` refused to open on the device it is most
// wanted on. The store is paged now (linestore.h): the same fixed-width lines,
// grouped into 6,400-byte chunks allocated as the file grows into them.
//
// Two things have to be proven and neither shows up in a flat array. First, the
// seam a flat array never had — the boundary between two chunks — has to behave
// exactly like the middle of one: a cursor moving across it, a line joined
// across it, a split that lands on it. Second, running out of heap partway now
// has more than one moment it can happen, and every one of them has to refuse
// cleanly and leave the invariant every read depends on intact: every index
// below `count` has its chunk allocated. This drives the real ed_* functions
// through a faked filesystem to check both.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// --- a malloc that can be told to fail --------------------------------------
//
// The store and the loader both allocate, and the failures worth testing are
// theirs. The real allocator is captured before the macro below shadows it, so
// the fake can still hand back memory on the allocations it is not failing.
static void *(*g_real_malloc)(size_t) = malloc;
static long g_alloc_seen;      // mallocs since the last reset
static long g_fail_at = -1;    // fail this-numbered malloc and every one after; -1 = never
static void *test_malloc(size_t n) {
    long i = g_alloc_seen++;
    if (g_fail_at >= 0 && i >= g_fail_at) return NULL;
    return g_real_malloc(n);
}
#define malloc test_malloc

// --- the editor's headers, then its dependencies faked ----------------------
//
// These headers are host-clean (no hardware), so the real types line up with
// what editor.cpp expects. Everything editor.cpp calls is defined below; only
// the filesystem seam does anything, because only ed_load and ed_save reach it.
#include "command.h"
#include "out.h"
#include "tui.h"
#include "tuiterm.h"
#include "storage.h"
#include "path.h"
#include "task.h"

// The file ed_load streams from. storage_open_source hands back a reader over
// this buffer; storage_stat reports it present. g_file == nullptr models a file
// that does not exist, which is how the editor opens a new one.
static const char *g_file;
static uint32_t    g_file_len;
static bool        g_open_source_fails;
static int fake_src_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    (void)ctx;
    if (!g_file || off >= g_file_len) return 0;
    uint32_t n = g_file_len - off;
    if (n > len) n = len;
    memcpy(dst, g_file + off, n);
    return (int)n;
}
bool storage_stat(const char *, bool *is_dir, uint32_t *size) {
    if (is_dir) *is_dir = false;
    if (size)   *size = g_file_len;
    return g_file != NULL;
}
bool storage_open_source(const char *, AppSource *src, void **h) {
    if (g_open_source_fails) return false;
    src->ctx = NULL; src->read = fake_src_read; src->size = g_file_len;
    *h = (void *)1;
    return true;
}
void storage_close_source(void *) {}

// What ed_save wrote, captured so a round-trip can be read back.
static char     g_sink[16384];
static uint32_t g_sink_len;
void *storage_open_sink(const char *) { g_sink_len = 0; return (void *)2; }
bool  storage_sink_write(void *, const uint8_t *data, uint32_t len) {
    if (g_sink_len + len > sizeof(g_sink)) return false;
    memcpy(g_sink + g_sink_len, data, len);
    g_sink_len += len;
    return true;
}
bool  storage_close_sink(void *) { return true; }

// The rest of what editor.cpp names: the screen, the terminal, the command
// registry, the cwd, the poll-loop sleep. None of it is reached — cmd_edit runs
// the interactive loop and these tests never call it — but it all has to link.
void tui_clear(TuiScreen *) {}
void tui_resize(TuiScreen *, uint16_t, uint16_t) {}
void tui_fill(TuiScreen *, int, int, int, int, char, uint8_t, uint8_t) {}
int  tui_text(TuiScreen *, int, int, const char *, uint8_t, uint8_t) { return 0; }
int  tui_text_clip(TuiScreen *, int, int, const char *, int, uint8_t, uint8_t) { return 0; }
void tuiterm_begin(void) {}
void tuiterm_end(void) {}
void tuiterm_present(const TuiScreen *) {}
void tuiterm_cursor(int, int, bool) {}
bool tuiterm_poll(TuiEvent *) { return false; }
void tuiterm_size(uint16_t *w, uint16_t *h) { if (w) *w = 80; if (h) *h = 24; }
bool tuiterm_refresh(void) { return false; }
bool tuiterm_active(void) { return false; }
void out_ok  (const char *, ...) {}
void out_warn(const char *, ...) {}
void out_err (const char *, ...) {}
void out_multi(const char *, ...) {}
void path_resolve(const char *, const char *in, char *out, size_t cap) {
    snprintf(out, cap, "%s", in);
}
bool cmd_register(const Command *) { return true; }
void task_sleep_ms(uint32_t) {}
extern "C" const char *fs_cwd(void) { return "/"; }

// The editor itself. Its static ed_* functions become visible to the tests, and
// its own #includes of the headers above are no-ops the second time round.
#include "../shell/editor.cpp"

// --- the harness ------------------------------------------------------------

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

static void reset_alloc(void) { g_alloc_seen = 0; g_fail_at = -1; }

// A fresh editor with an empty store. memset zeroes the embedded LineStore,
// which is exactly what an empty store is.
static void fresh(Editor *e) { memset(e, 0, sizeof(*e)); }

static void load_str(Editor *e, const char *s) {
    fresh(e);
    g_file = s;
    g_file_len = s ? (uint32_t)strlen(s) : 0;
    g_open_source_fails = false;
    ed_load(e, "/f");
}

int main(void) {
    // =====================================================================
    //  The store on its own — boundaries, the chunk seam, the shape guard.
    // =====================================================================
    {
        LineStore ls; ls_init(&ls);

        ck(ls_line(&ls, -1) == NULL,            "a negative index is refused, not wrapped");
        ck(ls_line(&ls, ED_MAX_LINES) == NULL,  "one past the last line is refused");

        char *l0 = ls_line(&ls, 0);
        char *lN = ls_line(&ls, ED_MAX_LINES - 1);
        ck(l0 && lN && l0 != lN, "the first and last line are both real and distinct");

        // A full-width line: ED_MAX_COL-1 characters and the terminator, read back.
        char *w = ls_line(&ls, 5);
        memset(w, 'a', ED_MAX_COL - 1);
        w[ED_MAX_COL - 1] = 0;
        ck((int)strlen(w) == ED_MAX_COL - 1, "a line holds ED_MAX_COL-1 characters");

        // Within a chunk, consecutive lines are exactly ED_MAX_COL apart — the
        // layout a whole-row memcpy relies on.
        ck(ls_line(&ls, 1) - ls_line(&ls, 0) == ED_MAX_COL, "lines in a chunk are contiguous");

        // The seam: line 31 ends chunk 0, line 32 begins chunk 1. They must be
        // independent buffers — writing one cannot touch the other.
        char *a = ls_line(&ls, LS_LINES_PER_CHUNK - 1);
        char *b = ls_line(&ls, LS_LINES_PER_CHUNK);
        strcpy(a, "end-of-chunk-0");
        strcpy(b, "start-of-chunk-1");
        ck(strcmp(a, "end-of-chunk-0") == 0 && strcmp(b, "start-of-chunk-1") == 0,
           "the two sides of a chunk boundary are independent");

        // The shape guard, at runtime. No single allocation the store ever makes
        // is larger than one chunk — the whole point of the change. The
        // compile-time half is the static_assert in linestore.h.
        ck(ls.max_alloc == LS_CHUNK_BYTES, "the largest allocation is exactly one chunk");
        ck(LS_CHUNK_BYTES <= 8 * 1024,     "a chunk is small enough to fit a busy heap");
        ck((uint32_t)ED_MAX_LINES * ED_MAX_COL > 40u * 1024u,
           "...where the old flat buffer would not have");

        ls_free(&ls);
    }

    // Lazy allocation: touching a high line allocates only its own chunk, not the
    // ones below it. A small file must not pay for four hundred lines.
    {
        LineStore ls; ls_init(&ls);
        ls_line(&ls, 200);
        int allocated = 0;
        for (int c = 0; c < LS_CHUNKS; c++) if (ls.chunk[c]) allocated++;
        ck(allocated == 1, "touching one line allocates one chunk, not the file");
        ls_free(&ls);
    }

    // Out of heap: ls_line reports it by returning null, and a line already
    // handed out is untouched by the failure. This is the mechanism every
    // growth-edge refusal is built on.
    {
        LineStore ls; ls_init(&ls);
        char *keep = ls_line(&ls, 0);
        strcpy(keep, "still here");
        reset_alloc();
        g_fail_at = 0;                       // fail the next allocation
        ck(ls_line(&ls, LS_LINES_PER_CHUNK) == NULL, "a chunk that cannot be allocated returns null");
        reset_alloc();
        ck(strcmp(ls_line(&ls, 0), "still here") == 0, "the failure left the existing line intact");
        ls_free(&ls);
    }

    // =====================================================================
    //  ed_load — the streaming read, through the real function.
    // =====================================================================
    Editor e;

    load_str(&e, NULL);
    ck(e.count == 1 && !e.truncated, "a missing file opens as one empty line");
    ls_free(&e.ls);

    load_str(&e, "alpha\nbeta\ngamma\n");
    ck(e.count == 4, "three lines and a trailing newline load as four (the last empty)");
    ck(strcmp(ls_line(&e.ls, 0), "alpha") == 0, "first line loaded");
    ck(strcmp(ls_line(&e.ls, 2), "gamma") == 0, "third line loaded");
    ck(!e.truncated, "a file that fits is not marked truncated");
    ls_free(&e.ls);

    load_str(&e, "no-newline-at-end");
    ck(e.count == 1 && strcmp(ls_line(&e.ls, 0), "no-newline-at-end") == 0,
       "a final line with no newline still loads");
    ls_free(&e.ls);

    // Windows line endings: CR is dropped, so a pasted-in CRLF file is not full
    // of stray carriage returns.
    load_str(&e, "a\r\nb\r\n");
    ck(e.count == 3 && strcmp(ls_line(&e.ls, 0), "a") == 0 && strcmp(ls_line(&e.ls, 1), "b") == 0,
       "CRLF line endings lose the CR");
    ls_free(&e.ls);

    // A line longer than a line can be is cut, not wrapped, and the file is
    // marked so it will not be saved back short.
    {
        static char big[ED_MAX_COL + 64];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = 0;
        load_str(&e, big);
        ck(e.truncated, "a line too long marks the file truncated");
        ck((int)strlen(ls_line(&e.ls, 0)) == ED_MAX_COL - 1, "the over-long line is cut to fit");
        ls_free(&e.ls);
    }

    // More lines than the store holds: truncated, capped at ED_MAX_LINES, and —
    // the invariant — every counted line has a real buffer.
    {
        static char many[(ED_MAX_LINES + 40) * 2 + 8];
        int p = 0;
        for (int i = 0; i < ED_MAX_LINES + 40; i++) { many[p++] = 'x'; many[p++] = '\n'; }
        many[p] = 0;
        load_str(&e, many);
        ck(e.truncated, "more lines than fit marks the file truncated");
        ck(e.count == ED_MAX_LINES, "the line count is capped");
        bool all = true;
        for (int i = 0; i < e.count; i++) if (!ls_line(&e.ls, i)) all = false;
        ck(all, "every counted line has its chunk (the invariant holds after a full load)");
        ls_free(&e.ls);
    }

    // Out of heap partway through the load: what loaded is kept and complete,
    // the file is marked truncated, and no counted index is missing its buffer.
    {
        static char lines[64 * 4];
        int p = 0;
        for (int i = 0; i < 40; i++) { lines[p++] = 'x'; lines[p++] = '\n'; }
        lines[p] = 0;
        fresh(&e);
        g_file = lines; g_file_len = (uint32_t)p; g_open_source_fails = false;
        reset_alloc();
        g_fail_at = 2;              // chunk 0 (#0) and the read buffer (#1) succeed; chunk 1 (#2) fails
        ed_load(&e, "/f");
        reset_alloc();
        ck(e.truncated, "an allocation failure mid-load marks the file truncated");
        ck(e.count == LS_LINES_PER_CHUNK, "the count stops at the last line that fit");
        bool all = true;
        for (int i = 0; i < e.count; i++) if (!ls_line(&e.ls, i)) all = false;
        ck(all, "every counted line is allocated despite the failure (no crash, invariant held)");
        ls_free(&e.ls);
    }

    // =====================================================================
    //  ed_newline — splitting, the chunk seam, and the growth-edge refusal.
    // =====================================================================

    // A plain split: the tail of the line moves down, the head stays.
    load_str(&e, "hello world\n");
    e.cx = 5; e.cy = 0;
    ed_newline(&e);
    ck(e.count == 3, "splitting a line adds one (plus the trailing empty)");
    ck(strcmp(ls_line(&e.ls, 0), "hello") == 0, "the head stays on the first line");
    ck(strcmp(ls_line(&e.ls, 1), " world") == 0, "the tail moves to the next");
    ls_free(&e.ls);

    // A split that crosses the chunk boundary. With chunk 0 full (32 lines), any
    // newline pushes a line into chunk 1 — the shift writes across the seam.
    {
        // No trailing newline, so this loads as exactly LS_LINES_PER_CHUNK lines —
        // chunk 0 full, chunk 1 not yet allocated. A trailing newline would add an
        // empty final line and pre-allocate chunk 1, hiding the seam.
        static char b[LS_LINES_PER_CHUNK * 8];
        int p = 0;
        for (int i = 0; i < LS_LINES_PER_CHUNK; i++) {
            b[p++] = (char)('0' + (i % 10));
            if (i < LS_LINES_PER_CHUNK - 1) b[p++] = '\n';
        }
        b[p] = 0;
        load_str(&e, b);
        ck(e.count == LS_LINES_PER_CHUNK, "thirty-two lines fill exactly one chunk");
        char last_before[8];
        snprintf(last_before, sizeof(last_before), "%s", ls_line(&e.ls, LS_LINES_PER_CHUNK - 1));
        e.cy = 0; e.cx = 0;
        reset_alloc();
        ed_newline(&e);            // count -> 33, so line 32 is needed: chunk 1 is allocated here
        reset_alloc();
        ck(e.count == LS_LINES_PER_CHUNK + 1, "the newline grew the file past the chunk boundary");
        ck(ls_line(&e.ls, LS_LINES_PER_CHUNK) != NULL, "the line in the new chunk exists");
        ck(strcmp(ls_line(&e.ls, LS_LINES_PER_CHUNK), last_before) == 0,
           "the row that shifted across the seam kept its contents");
        ls_free(&e.ls);
    }

    // The growth-edge refusal. With count at a chunk boundary, the new line needs
    // a new chunk; fail that allocation and ed_newline must refuse and leave the
    // file exactly as it was.
    {
        // Exactly LS_LINES_PER_CHUNK lines (no trailing newline), so the next line
        // genuinely needs a new chunk.
        static char b[LS_LINES_PER_CHUNK * 8];
        int p = 0;
        for (int i = 0; i < LS_LINES_PER_CHUNK; i++) {
            b[p++] = 'a';
            if (i < LS_LINES_PER_CHUNK - 1) b[p++] = '\n';
        }
        b[p] = 0;
        load_str(&e, b);
        int before = e.count;      // == LS_LINES_PER_CHUNK
        e.cy = 0; e.cx = 0;
        reset_alloc();
        g_fail_at = 0;             // fail the chunk this newline would add
        ed_newline(&e);
        reset_alloc();
        ck(e.count == before, "a newline that cannot get its chunk leaves the count unchanged");
        ck(strcmp(ls_line(&e.ls, 0), "a") == 0, "and leaves the existing lines untouched");
        ls_free(&e.ls);
    }

    // The other refusal that already existed: a full file.
    {
        static char many[(ED_MAX_LINES + 40) * 2 + 8];
        int p = 0;
        for (int i = 0; i < ED_MAX_LINES + 40; i++) { many[p++] = 'x'; many[p++] = '\n'; }
        many[p] = 0;
        load_str(&e, many);        // count capped at ED_MAX_LINES, truncated
        e.truncated = false;       // clear so the refusal we see is the line cap, not the save guard
        int before = e.count;
        e.cy = 0; e.cx = 0;
        ed_newline(&e);
        ck(e.count == before, "a full file refuses another line");
        ls_free(&e.ls);
    }

    // =====================================================================
    //  ed_backspace — joining, including across the chunk seam.
    // =====================================================================

    load_str(&e, "ab\ncd\n");
    e.cy = 1; e.cx = 0;
    ed_backspace(&e);
    ck(e.count == 2 && strcmp(ls_line(&e.ls, 0), "abcd") == 0, "backspace at column 0 joins with the line above");
    ls_free(&e.ls);

    // Join across the boundary: line 32 (chunk 1) folded up into line 31 (chunk
    // 0). The read is from one chunk, the write to another.
    {
        static char b[LS_LINES_PER_CHUNK * 8];
        int p = 0;
        for (int i = 0; i <= LS_LINES_PER_CHUNK; i++) { b[p++] = 'a'; b[p++] = '\n'; }
        b[p] = 0;
        load_str(&e, b);           // LS_LINES_PER_CHUNK + 2 lines (0..32 plus a trailing empty)
        ck(e.count >= LS_LINES_PER_CHUNK + 1, "the file spans two chunks");
        e.cy = LS_LINES_PER_CHUNK;  // first line of chunk 1
        e.cx = 0;
        int before = e.count;
        ed_backspace(&e);
        ck(e.count == before - 1, "a cross-chunk join removes a line");
        ck(strcmp(ls_line(&e.ls, LS_LINES_PER_CHUNK - 1), "aa") == 0,
           "the join merged the line from chunk 1 into chunk 0");
        ls_free(&e.ls);
    }

    // =====================================================================
    //  ed_insert / ed_save — the width limit and a round-trip.
    // =====================================================================

    // A line already at full width refuses another character rather than
    // overrunning its buffer.
    {
        load_str(&e, NULL);
        char *l = ls_line(&e.ls, 0);
        memset(l, 'z', ED_MAX_COL - 1);
        l[ED_MAX_COL - 1] = 0;
        e.cy = 0; e.cx = ED_MAX_COL - 1;
        ed_insert(&e, 'q');
        ck((int)strlen(ls_line(&e.ls, 0)) == ED_MAX_COL - 1, "a full line refuses another character");
        ls_free(&e.ls);
    }

    // Save writes each line and a newline; a load of what was saved is the same
    // text. This walks the store the same way editing does.
    load_str(&e, "one\ntwo\nthree\n");
    ck(ed_save(&e), "a file that fits saves");
    ck(g_sink_len == (uint32_t)strlen("one\ntwo\nthree\n\n") &&
       memcmp(g_sink, "one\ntwo\nthree\n\n", g_sink_len) == 0,
       "the saved bytes are the lines, each newline-terminated");
    ls_free(&e.ls);

    // Save refuses a file that did not load whole — writing it back would delete
    // whatever did not fit.
    load_str(&e, "x\ny\n");
    e.truncated = true;
    ck(!ed_save(&e), "a truncated file will not save over the original");
    ls_free(&e.ls);

    printf("  editor: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
