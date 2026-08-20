// edit — a nano-style editor on the TUI layer.
//
// v1's editor was its own 700-line program with its own screen handling. This
// one is the text buffer and the keys; the framework does the rest.
//
// The text is a paged store of fixed-width lines rather than anything clever. A
// rope or a gap buffer is the right answer at a megabyte; at this size, on a
// device whose whole heap is under 400 KB and often fragmented, fixed lines in
// small chunks are faster and — the reason for the paging — never ask the heap
// for one big block it may not have. See linestore.h for why that block used to
// stop `edit` opening at all. The limits are stated up front and enforced rather
// than discovered.

#include "command.h"
#include "out.h"
#include "tui.h"
#include "linestore.h"

#include <stdlib.h>

// Gives the screen back however the command returns -- and these return from
// several places.
namespace { struct ScreenFree { void *p; ~ScreenFree() { free(p); } }; }
#include "tuiterm.h"
#include "storage.h"
#include "path.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

// The file is streamed into the store a block at a time rather than read whole
// into a 32 KB buffer first — one more large allocation the editor no longer
// makes. The block is small and sized under a chunk, so it never becomes the
// largest thing the editor holds.
#define ED_READ_BLOCK 2048

struct Editor {
    LineStore ls;                 // the text, paged (linestore.h)
    int      count;               // lines in use
    int      cy, cx;              // cursor, in buffer coordinates
    int      top, left;           // scroll
    bool     dirty;               // unsaved changes
    bool     truncated;           // the file did not fit
    char     path[128];
    char     status[80];
};

const char *fs_cwd(void);

// --- loading and saving -----------------------------------------------------

static void ed_note(Editor *e, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->status, sizeof(e->status), fmt, ap);
    va_end(ap);
}

static bool ed_load(Editor *e, const char *path) {
    e->count = 0;
    char *line = ls_line(&e->ls, 0);
    if (!line) { ed_note(e, "not enough memory to open it"); return false; }
    line[0] = 0;

    bool is_dir = false; uint32_t size = 0;
    if (!storage_stat(path, &is_dir, &size)) {
        e->count = 1;
        ed_note(e, "new file");
        return true;
    }
    if (is_dir) { ed_note(e, "that is a directory"); return false; }

    AppSource src; void *h = nullptr;
    if (!storage_open_source(path, &src, &h)) { ed_note(e, "could not open %s", path); return false; }

    // Streamed a block at a time, straight into the store — the file never lands
    // in RAM whole. The store grows a chunk at a time as the rows fill, so the
    // most memory in one piece while opening is one chunk, not the file. The read
    // block is smaller than a chunk, so it is never the largest thing held.
    uint8_t *buf = (uint8_t *)malloc(ED_READ_BLOCK);
    if (!buf) { storage_close_source(h); ed_note(e, "not enough memory to open it"); return false; }

    int row = 0, col = 0;
    bool oom = false;
    uint32_t off = 0;
    // `line` tracks row's buffer. It is fetched once per row, at the growth edge
    // below; a new chunk allocated for a later row never moves this one, because
    // each chunk is its own allocation.
    while (row < ED_MAX_LINES) {
        int got = src.read(src.ctx, off, buf, ED_READ_BLOCK);
        if (got <= 0) break;                 // end of file, or a read error: stop with what we have
        off += (uint32_t)got;
        for (int i = 0; i < got && row < ED_MAX_LINES; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[col] = 0;
                row++; col = 0;
                if (row < ED_MAX_LINES) {
                    line = ls_line(&e->ls, row);   // growth edge: the one place a chunk is added
                    if (!line) { oom = true; break; }
                }
                continue;
            }
            // A line too long is CUT rather than wrapped: wrapping would change the
            // file's meaning on save, and silently rewriting someone's file is worse
            // than refusing part of a line.
            if (col + 1 < ED_MAX_COL) line[col++] = c;
            else e->truncated = true;
        }
        if (oom) break;
    }
    free(buf);
    storage_close_source(h);

    if (oom || row >= ED_MAX_LINES) {
        // Out of heap partway in, or more lines than the store holds. What loaded
        // is complete up to here and every counted row is allocated; it is just
        // not the whole file, so it is marked exactly like one too tall — readable,
        // never savable. count = row leaves out the row whose chunk could not be
        // added, so no counted index is missing its buffer.
        e->truncated = true;
    } else {
        line[col] = 0;      // terminate the final, unterminated line
        row++;
    }
    e->count = row ? row : 1;

    if (e->truncated) ed_note(e, "opened, but too big to hold entirely - DO NOT SAVE");
    else              ed_note(e, "%d lines", e->count);
    return true;
}

static bool ed_save(Editor *e) {
    // Refuse to save a file that was not fully loaded. Writing it back would
    // silently delete whatever did not fit.
    if (e->truncated) { ed_note(e, "not saved: the file was too big to open fully"); return false; }

    void *h = storage_open_sink(e->path);
    if (!h) { ed_note(e, "could not write %s", e->path); return false; }
    bool ok = true;
    for (int i = 0; i < e->count && ok; i++) {
        const char *l = ls_line(&e->ls, i);       // i < count, so allocated by the invariant
        uint32_t len = (uint32_t)strlen(l);
        if (len) ok = storage_sink_write(h, (const uint8_t *)l, len);
        if (ok)  ok = storage_sink_write(h, (const uint8_t *)"\n", 1);
    }
    if (!storage_close_sink(h)) ok = false;
    if (ok) { e->dirty = false; ed_note(e, "saved %d lines", e->count); }
    else    ed_note(e, "write failed - the filesystem may be full");
    return ok;
}

// --- editing ----------------------------------------------------------------

static void ed_insert(Editor *e, char c) {
    char *l = ls_line(&e->ls, e->cy);      // cy < count, so allocated
    int len = (int)strlen(l);
    if (len + 1 >= ED_MAX_COL) { ed_note(e, "line is full (%d)", ED_MAX_COL - 1); return; }
    memmove(l + e->cx + 1, l + e->cx, (size_t)(len - e->cx + 1));
    l[e->cx++] = c;
    e->dirty = true;
}

static void ed_backspace(Editor *e) {
    char *l = ls_line(&e->ls, e->cy);      // cy < count, so allocated
    if (e->cx > 0) {
        int len = (int)strlen(l);
        memmove(l + e->cx - 1, l + e->cx, (size_t)(len - e->cx + 1));
        e->cx--;
        e->dirty = true;
        return;
    }
    if (e->cy == 0) return;
    // Join with the line above, which is where the cursor lands.
    char *up = ls_line(&e->ls, e->cy - 1);
    int ulen = (int)strlen(up), llen = (int)strlen(l);
    if (ulen + llen >= ED_MAX_COL) { ed_note(e, "joined line would be too long"); return; }
    memcpy(up + ulen, l, (size_t)llen + 1);
    // Shifting up only touches indices already below count — all allocated.
    for (int i = e->cy; i < e->count - 1; i++)
        memcpy(ls_line(&e->ls, i), ls_line(&e->ls, i + 1), ED_MAX_COL);
    e->count--;
    e->cy--;
    e->cx = ulen;
    e->dirty = true;
}

static void ed_newline(Editor *e) {
    if (e->count + 1 >= ED_MAX_LINES) { ed_note(e, "file is full (%d lines)", ED_MAX_LINES); return; }
    // The growth edge. Index `count` is the new last line; touch it first so the
    // one chunk this might add is added here, where failure can still be refused.
    // If it fails, count is left unchanged and the invariant holds: every index
    // below count still has its buffer. Every memcpy below is then to an index
    // that exists.
    if (!ls_line(&e->ls, e->count)) { ed_note(e, "out of memory - line not added"); return; }
    for (int i = e->count; i > e->cy + 1; i--)
        memcpy(ls_line(&e->ls, i), ls_line(&e->ls, i - 1), ED_MAX_COL);
    char *l = ls_line(&e->ls, e->cy);
    snprintf(ls_line(&e->ls, e->cy + 1), ED_MAX_COL, "%s", l + e->cx);
    l[e->cx] = 0;
    e->count++;
    e->cy++;
    e->cx = 0;
    e->dirty = true;
}

static void ed_clamp(Editor *e, int rows, int cols) {
    if (e->cy < 0) e->cy = 0;
    if (e->cy >= e->count) e->cy = e->count - 1;
    int len = (int)strlen(ls_line(&e->ls, e->cy));    // cy now in [0,count), allocated
    if (e->cx > len) e->cx = len;
    if (e->cx < 0) e->cx = 0;

    if (e->cy < e->top) e->top = e->cy;
    if (e->cy >= e->top + rows) e->top = e->cy - rows + 1;
    if (e->cx < e->left) e->left = e->cx;
    if (e->cx >= e->left + cols) e->left = e->cx - cols + 1;
    if (e->top < 0) e->top = 0;
    if (e->left < 0) e->left = 0;
}

// --- the command ------------------------------------------------------------

static int cmd_edit(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: edit <file>"); return 1; }

    Editor *e = (Editor *)calloc(1, sizeof(Editor));
    if (!e) { out_err("Not enough memory."); return 1; }
    // The text store starts empty and grows a chunk at a time from ed_load. There
    // is no 80 KB line array to reserve up front any more — that single block was
    // what stopped `edit` opening on a board with Nova D1 resident, where the heap
    // has plenty free but never that much in one piece. calloc zeroed the store, so
    // it is ready to use; the first chunk is asked for (and can be refused) inside
    // ed_load, where the memory is actually needed.

    path_resolve(fs_cwd(), argv[1], e->path, sizeof(e->path));
    if (!ed_load(e, e->path)) {
        out_err("%s", e->status);
        ls_free(&e->ls); free(e);
        return 1;
    }

    // On the heap, not in bss.
    //
    // A TuiScreen is twelve kilobytes, and holding it statically meant every
    // device paid for the editor and the settings panel whether or not either
    // was ever opened — twenty-four kilobytes of RAM reserved at build time for
    // two screens that are open for seconds at a time. Allocated here, it costs
    // nothing until it is used and comes back when the screen closes.
    TuiScreen *sp = (TuiScreen *)malloc(sizeof(TuiScreen));
    if (!sp) { out_err("Not enough memory to open this screen."); ls_free(&e->ls); free(e); return 1; }
    TuiScreen &s = *sp;
    ScreenFree _sf{sp};
    tuiterm_begin();
    if (!tuiterm_active()) {
        out_err("Could not start the editor.");
        ls_free(&e->ls); free(e);
        return 1;
    }
    uint16_t tw = 80, th = 24;
    tuiterm_size(&tw, &th);
    tui_resize(&s, tw, th);

    // Not const: Ctrl+L can change the window under us.
    int rows = s.h - 2;                // one header line, one status line
    int cols = s.w;

    // Reset per session. A function-static would leave the second `edit` of a
    // boot already armed, so it would quit on the first Ctrl+X and discard.
    int quit_armed = 0;
    bool last_was_cr = false;      // for collapsing CR LF from a paste
    bool running = true, dirty_screen = true;
    while (running) {
        ed_clamp(e, rows, cols);

        if (dirty_screen) {
            dirty_screen = false;
            tui_clear(&s);

            // Header: what is open and whether it has unsaved changes.
            char head[TUI_MAX_W + 1];
            snprintf(head, sizeof(head), " %s%s", e->path, e->dirty ? "  *modified*" : "");
            tui_fill(&s, 0, 0, s.w, 1, ' ', TUI_REVERSE, TUI_DEFAULT);
            tui_text_clip(&s, 0, 0, head, s.w - 18, TUI_REVERSE, TUI_DEFAULT);
            char pos[24];
            snprintf(pos, sizeof(pos), "%d:%d of %d", e->cy + 1, e->cx + 1, e->count);
            tui_text(&s, s.w - 18, 0, pos, TUI_REVERSE, TUI_DEFAULT);

            for (int r = 0; r < rows; r++) {
                int i = e->top + r;
                if (i >= e->count) break;
                const char *l = ls_line(&e->ls, i);   // i < count, so allocated
                int len = (int)strlen(l);
                if (e->left < len) tui_text(&s, 0, r + 1, l + e->left, TUI_NORMAL, TUI_DEFAULT);
            }

            tui_fill(&s, 0, s.h - 1, s.w, 1, ' ', TUI_REVERSE, TUI_DEFAULT);
            tui_text_clip(&s, 1, s.h - 1, e->status, s.w - 26, TUI_REVERSE, TUI_DEFAULT);
            tui_text(&s, s.w - 25, s.h - 1, "^S saves   ^X exits", TUI_REVERSE, TUI_DEFAULT);

            tuiterm_present(&s);
            // The cursor is placed after presenting, so it ends up where the
            // text is rather than wherever the last diffed cell left it.
            tuiterm_cursor(e->cx - e->left, e->cy - e->top + 1, true);
        }

        TuiEvent ev;
        bool busy = false;
        while (tuiterm_poll(&ev)) {
            busy = true;
            dirty_screen = true;

            if (ev.kind == TUI_EV_MOUSE) {
                if (ev.mouse == TUI_MOUSE_WHEEL_UP)        e->top -= 3;
                else if (ev.mouse == TUI_MOUSE_WHEEL_DOWN) e->top += 3;
                else if (ev.mouse == TUI_MOUSE_DOWN && ev.y >= 1 && (int)ev.y <= rows) {
                    e->cy = e->top + (int)ev.y - 1;
                    e->cx = e->left + (int)ev.x;
                }
                if (e->top < 0) e->top = 0;
                if (e->top > e->count - 1) e->top = e->count - 1;
                continue;
            }
            if (ev.kind != TUI_EV_KEY) continue;

            switch (ev.key) {
                case TUI_KEY_UP:    e->cy--; break;
                case TUI_KEY_DOWN:  e->cy++; break;
                case TUI_KEY_LEFT:  if (e->cx) e->cx--; else if (e->cy) { e->cy--; e->cx = (int)strlen(ls_line(&e->ls, e->cy)); } break;
                case TUI_KEY_RIGHT: if (e->cx < (int)strlen(ls_line(&e->ls, e->cy))) e->cx++;
                                    else if (e->cy < e->count - 1) { e->cy++; e->cx = 0; } break;
                case TUI_KEY_HOME:  e->cx = 0; break;
                case TUI_KEY_END:   e->cx = (int)strlen(ls_line(&e->ls, e->cy)); break;
                case TUI_KEY_PGUP:  e->cy -= rows - 1; break;
                case TUI_KEY_PGDN:  e->cy += rows - 1; break;
                case TUI_KEY_DELETE: {
                    // Delete-forward is backspace from one place further on.
                    int len = (int)strlen(ls_line(&e->ls, e->cy));
                    if (e->cx < len) { e->cx++; ed_backspace(e); }
                    else if (e->cy < e->count - 1) { e->cy++; e->cx = 0; ed_backspace(e); }
                    break;
                }
                case 19:                     // Ctrl+S
                    ed_save(e);
                    break;
                case 24: case 3: {           // Ctrl+X, Ctrl+C
                    if (e->dirty) {
                        if (quit_armed) running = false;      // asked twice: discard
                        else ed_note(e, "unsaved changes - ^S to save, ^X again to discard");
                        quit_armed = 1;
                    } else running = false;
                    break;
                }
                case '\r':
                    ed_newline(e);
                    last_was_cr = true;
                    break;
                case '\n':
                    // A paste of Windows text sends CR LF for one line break.
                    // Without this every pasted line gains a blank one after it.
                    if (!last_was_cr) ed_newline(e);
                    break;
                case 8: case 0x7f:    ed_backspace(e); break;
                case '\t':
                    for (int i = 0; i < 4; i++) ed_insert(e, ' ');
                    break;
                case 12:            // Ctrl+L — re-measure and repaint
                    if (tuiterm_refresh()) {
                        tuiterm_size(&tw, &th);
                        tui_resize(&s, tw, th);
                        rows = s.h - 2;
                        cols = s.w;
                    }
                    break;
                default:
                    if (ev.key >= 32 && ev.key < 127) ed_insert(e, (char)ev.key);
                    break;
            }
            // Anything other than the quit key disarms it, so a Ctrl+X now and
            // another minutes later cannot combine into a discard.
            if (ev.key != 24 && ev.key != 3) quit_armed = 0;
            if (ev.key != '\r') last_was_cr = false;
        }
        if (!busy) task_sleep_ms(5);
    }

    tuiterm_end();
    if (e->dirty) out_warn("Exited with unsaved changes.");
    else          out_ok("%s", e->status);
    ls_free(&e->ls);
    free(e);
    return 0;
}

void editor_register(void) {
    static const Command c_edit{"edit", "edit a text file", cmd_edit};
    static const Command c_nano{"nano", "edit a text file", cmd_edit};
    static const Command c_vi{"vi", "edit a text file", cmd_edit};
    // The name is a convenience, not a promise of that program's bindings.
    static const Command c_vim{"vim", "edit a text file", cmd_edit};
    cmd_register(&c_edit);
    cmd_register(&c_nano);
    cmd_register(&c_vi);
    cmd_register(&c_vim);
}
