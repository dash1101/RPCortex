// Desc: The Files browser, the Logs reader and the Alerts queue.
// File: novagui_files.cpp
#include "novagui_files.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"
#include "novalog.h"
#include "novanotify.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// Everything below keeps its bulk in file-statics rather than in the screens.
// A slot is 384 bytes and only the TOP screen is ever drawn, so one window of
// directory entries, one page of file text and one window of wrapped lines is
// all there is to hold — and none of the three fits in a slot anyway.
//
// NO DEFAULT INITIALISERS anywhere in this file. Screens are placement
// constructed into bss and start as zeroes; a default member initialiser or a
// namespace-scope object with a constructor would emit .init_array, which
// nothing in a package ever runs.

// --- the file the viewer and the confirmation are looking at ------------------

// Long enough for anything littlefs holds on this device, and short enough that
// six of them plus the rest of this file's statics stay well inside the app's
// memory. A path that would not fit is REFUSED rather than truncated — see
// join() below.
constexpr unsigned PATH_MAX = 128;

static char g_file[PATH_MAX];      // what the viewer or the info screen shows
static char g_target[PATH_MAX];    // what a delete confirmation is about
static char g_question[64];        // confirm() keeps the question as a POINTER
static char g_title[16];           // the viewer's and info screen's title

// Build "<dir>/<name>", returning false when it would not fit.
//
// Not nova::path_join, because that one would produce "//name" at the root; and
// not a bare snprintf, because snprintf TRUNCATES, and a truncated path is a
// different path that exists. Opening or deleting a different path than the one
// on the row is exactly the class of silent fault this suite comments about.
static bool join(char *out, unsigned cap, const char *dir, const char *name) {
    int n = (dir[0] == '/' && !dir[1]) ? snprintf(out, cap, "/%s", name)
                                       : snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (unsigned)n < cap;
}

// The last separator in a path. Written out rather than strrchr: a package gets
// the string functions in the firmware's export table and no others, and that
// one is not among them — it fails at the link, which is where a missing symbol
// should fail.
static const char *last_slash(const char *s) {
    const char *found = nullptr;
    for (; *s; s++) if (*s == '/') found = s;
    return found;
}

// The part after the last separator. Returns "/" for the root, where there is
// nothing after it.
static const char *basename(const char *path) {
    const char *slash = last_slash(path);
    if (!slash) return path;
    return slash[1] ? slash + 1 : "/";
}

// Bytes as somebody would say them. No float: the package has no libm worth
// pulling in for one decimal place, and the tenth is arithmetic.
static void human(char *out, unsigned cap, uint32_t n) {
    if (n < 1024u) {
        snprintf(out, cap, "%uB", (unsigned)n);
    } else if (n < 1024u * 1024u) {
        snprintf(out, cap, "%uK", (unsigned)(n / 1024u));
    } else {
        unsigned mb = (unsigned)(n >> 20);
        unsigned tenth = (unsigned)(((n >> 10) & 1023u) * 10u / 1024u);
        snprintf(out, cap, "%u.%uM", mb, tenth);
    }
}

// The same, but the input is KILOBYTES — a whole SD card in bytes overflows a
// uint32_t at 4 GB, and cards are bigger than that. Roots report capacity in KB
// for exactly this reason (see FwStorageRoot).
static void human_kb(char *out, unsigned cap, uint32_t kb) {
    if (kb < 1024u) {
        snprintf(out, cap, "%uK", (unsigned)kb);
    } else if (kb < 1024u * 1024u) {
        snprintf(out, cap, "%uM", (unsigned)(kb / 1024u));
    } else {
        unsigned gb = (unsigned)(kb >> 20);
        unsigned tenth = (unsigned)(((kb >> 10) & 1023u) * 10u / 1024u);
        snprintf(out, cap, "%u.%uG", gb, tenth);
    }
}

// Draw a value right-aligned, trimming from the LEFT when it is too wide. For a
// path or a filename the tail is the part that tells one from another, which is
// the opposite of what text_fit's trailing ".." keeps.
static void tail_fit(Canvas &c, int right, int y, const char *s) {
    int w = c.text_width(s, 1, false);
    while (*s && right - w < 2) { s++; w = c.text_width(s, 1, false); }
    c.text(right - w, y, s, 1);
}

// --- the file viewer ----------------------------------------------------------
//
// A WINDOW over the file rather than a copy of it. The Nova D1's log can be
// larger than the whole package arena, so the viewer reads the few hundred bytes
// it is about to draw with fw_file_read_at and forgets them again. The
// MicroPython original read the first kilobyte and stopped there; being able to
// reach the END of a log is the reason this one does not.
//
// The scroll position is a BYTE OFFSET, not a line number, because counting the
// lines of a file means reading all of it. Rendering always starts at that
// offset and walks forward, so every offset is a valid place to be — which is
// what makes scrolling backwards tractable at all.

constexpr unsigned PAGE_BYTES = 512;   // comfortably more than one panel of text
constexpr unsigned BACK_BYTES = 256;   // how far back the previous line is sought

static char     g_page[PAGE_BYTES];
static unsigned g_page_len;
static uint32_t g_page_off;
static uint32_t g_page_size;           // the file's size, cached with the page
static bool     g_page_ok;

// Make sure [lo, hi) is in the page. Guarded, so the common case — the next row
// of the same screenful — costs nothing; a supervisor call is 296 cycles and a
// littlefs read is a great deal more.
static void page_need(uint32_t lo, uint32_t hi) {
    if (hi > g_page_size) hi = g_page_size;
    if (g_page_ok && lo >= g_page_off && hi <= g_page_off + g_page_len) return;
    unsigned got = fw_file_read_at(g_file, lo, g_page, PAGE_BYTES);
    // A short read at the end of the file is the answer, not a failure.
    g_page_len = got > PAGE_BYTES ? PAGE_BYTES : got;
    g_page_off = lo;
    g_page_ok  = true;
}

// Render one line starting at `from` into `out`, and return where the next one
// begins. Equal to `from` means nothing was left to read.
static uint32_t view_line(uint32_t from, char *out, unsigned cap, int cols) {
    out[0] = 0;
    if (!g_page_ok || from < g_page_off) return from;
    unsigned i = (unsigned)(from - g_page_off);
    if (i >= g_page_len) return from;
    unsigned avail = g_page_len - i;

    unsigned take = 0;
    while (take < avail && take < (unsigned)cols && g_page[i + take] != '\n') take++;
    unsigned adv = take;
    if (take == (unsigned)cols && take < avail && g_page[i + take] != '\n') {
        // Break at the last space rather than mid-word — the same rule ui::wrap
        // uses, so text breaks in the same places everywhere on the device.
        unsigned sp = take;
        while (sp > 0 && g_page[i + sp] != ' ') sp--;
        if (sp > 0) { take = sp; adv = sp; }
    }

    unsigned n = 0;
    for (unsigned k = 0; k < take && n + 1 < cap; k++) {
        char ch = g_page[i + k];
        // A tab, a stray CR or a byte from a file that turned out not to be text
        // is drawn as a dot. The font substitutes for anything it does not have,
        // and a panel of substitutes hides the line that was actually there.
        out[n++] = (ch >= 0x20 && ch <= 0x7e) ? ch : '.';
    }
    out[n] = 0;

    while (adv < avail && g_page[i + adv] == ' ') adv++;   // the break is not shown
    if (adv < avail && g_page[i + adv] == '\n') adv++;
    return from + adv;
}

// Where the line ABOVE `off` starts.
//
// A wrap can only be computed forwards, so this anchors on the start of the
// physical line above and walks down to the last line that is still off the top
// of the view. The search back for that anchor is bounded; a line longer than
// the window gets an approximate one, and because any offset renders correctly
// the cost of that is a repeated line rather than a skipped one.
static uint32_t view_prev(uint32_t off, int cols) {
    if (!off) return 0;
    uint32_t from = off > BACK_BYTES ? off - BACK_BYTES : 0;
    page_need(from, off + (uint32_t)cols + 2);

    uint32_t anchor = from;
    if (off >= 2) {
        // The byte at off-1 terminates the line above, so the search for that
        // line's START begins one earlier.
        uint32_t p = off - 2;
        while (true) {
            if (p < g_page_off || p - g_page_off >= g_page_len) break;
            if (g_page[p - g_page_off] == '\n') { anchor = p + 1; break; }
            if (p == from) break;
            p--;
        }
    }
    if (anchor >= off) anchor = from;

    char line[40];
    uint32_t cur = anchor, prev = anchor;
    while (cur < off) {
        prev = cur;
        uint32_t next = view_line(cur, line, sizeof(line), cols);
        if (next <= cur) break;
        cur = next;
    }
    return prev;
}

class ViewScreen : public Screen {
public:
    // g_file already holds the path: the browser puts it there before pushing,
    // because push<T>() constructs into a zeroed slot and calls enter() before
    // the caller gets a chance to say anything to the screen.
    void begin(void) {
        off_ = 0;
        g_page_ok = false;
        g_page_size = fw_file_size(g_file);
        nova::ellipsize(g_title, sizeof(g_title), basename(g_file), 14);
    }

    const char *title(void) const override { return g_title[0] ? g_title : "File"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Turn to scroll.";
        out[1] = "Only the part on screen is";
        out[2] = "read, so any size opens.";
        return 3;
    }

    void draw(Canvas &c) override {
        if (!g_page_size) { c.text(2, ui::TOP, "(empty file)", 1); return; }

        const int rows = ui::rows_for(c);
        const int cols = c.cols() - 1;          // the scrollbar lane
        char line[40];
        uint32_t cur = off_;
        for (int i = 0; i < rows; i++) {
            // Guarded, so this is not an ABI call per row — only the first row
            // of a screenful that moved outside the page pays for one.
            page_need(cur, cur + (uint32_t)cols + 2);
            uint32_t next = view_line(cur, line, sizeof(line), cols);
            if (line[0]) c.text(2, ui::TOP + i * ui::ROWH, line, 1);
            if (next <= cur) break;
            cur = next;
        }

        // Proportional by BYTE, since the line count of a file this screen never
        // reads whole is not a number it has. Position is what the bar is for.
        c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP,
                    (int)off_, (int)(cur - off_), (int)g_page_size);
    }

    Action on_event(Event e) override {
        const int cols = gui::canvas().cols() - 1;
        if (e == EV_ROT_CW) {
            page_need(off_, off_ + (uint32_t)cols + 2);
            char line[40];
            uint32_t next = view_line(off_, line, sizeof(line), cols);
            // Stopping while one line is still on screen: scrolling to a blank
            // panel reads as the file having ended badly.
            if (next > off_ && next < g_page_size) off_ = next;
            return ui::ACT_STAY;
        }
        if (e == EV_ROT_CCW) { off_ = view_prev(off_, cols); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    uint32_t off_;
};

// --- what a non-text file is ---------------------------------------------------
//
// Previewing a .pkg or a capture would be meaningless, so it says what the thing
// is instead. Ported from the MicroPython InfoScreen, rows and all.

class InfoScreen : public Screen {
public:
    void begin(uint32_t size) {
        size_ = size;
        nova::ellipsize(g_title, sizeof(g_title), basename(g_file), 14);
    }

    const char *title(void) const override { return g_title[0] ? g_title : "File"; }

    void draw(Canvas &c) override {
        char v[24];
        const int right = c.width() - 2;

        c.text(2, ui::TOP, "Name", 1);
        tail_fit(c, right, ui::TOP, basename(g_file));

        human(v, sizeof(v), size_);
        c.text(2, ui::TOP + ui::ROWH, "Size", 1);
        tail_fit(c, right, ui::TOP + ui::ROWH, v);

        // The directory, which is the path with the name taken off it. Built
        // here rather than kept, because it is only ever looked at.
        char where[PATH_MAX];
        nova::copy(where, sizeof(where), g_file);
        const char *slash = last_slash(where);
        // The root keeps its slash: "" is not where anything is.
        if (slash) {
            unsigned cut = (unsigned)(slash - where);
            where[cut ? cut : 1] = 0;
        }
        c.text(2, ui::TOP + 2 * ui::ROWH, "Where", 1);
        tail_fit(c, right, ui::TOP + 2 * ui::ROWH, where);
    }

private:
    uint32_t size_;
};

// --- the browser ----------------------------------------------------------------
//
// Entering a directory PUSHES another browser, which is what makes BACK mean "up
// one level" for free and lets each level keep the row somebody was on. The
// MicroPython original mutated one screen's path instead and had to intercept
// BACK, and it lost the selection every time.
//
// The cost is that the browser is as deep as the screen stack allows. Eight
// slots: home takes one, each directory level takes one, and ONE IS ALWAYS HELD
// BACK so the deepest level can still push a viewer or a confirmation. That is
// the root plus five levels under it, and the level after that SAYS SO — a push
// that quietly does not happen is a button that reads as broken.
constexpr int LEVEL_MAX = (int)gui::STACK_MAX - 3;

// One directory per level, in bss. A path in each screen would fit, but the
// window of entries below it would not — six FwDirEntry is 432 bytes against a
// 384-byte slot — so once that has to live here the path belongs beside it.
// ModuleScreen::g_mod is the same idea with one level.
//
// THE INVARIANT: open_files() claims level 0, and a child is always its parent's
// level plus one. It holds because open_files is only ever reached from the app
// catalogue.
static char g_path[LEVEL_MAX + 1][PATH_MAX];

// Held between the roots screen and the browser it opens: the browser shows the
// root's friendly name ("On-Board", "SD") at its top level rather than "/" or
// "sd". Written by RootsScreen::open_sel before the browser is pushed.
static char g_root_label[16];

// The entries currently on the panel, and which listing they came from. Keyed
// rather than refilled every frame: fw_dir_entry(path, i) walks the directory to
// reach i, so a redraw that has not scrolled must not pay for it again.
constexpr int WIN_MAX = 8;              // more rows than a 64-pixel panel has
static FwDirEntry g_win[WIN_MAX];
static int  g_win_n, g_win_level, g_win_first, g_win_rows;
static bool g_win_ok;

// The files worth previewing. Anything else gets the info screen, because a
// wrapped page of a binary is noise wearing text's clothes.
static const char *const kTexty[] = {
    ".txt", ".md", ".cfg", ".json", ".py", ".lp", ".log", ".rps", ".csv",
    ".ini", ".sh",
};

static bool texty(const char *name) {
    unsigned len = (unsigned)strlen(name);
    for (unsigned i = 0; i < sizeof(kTexty) / sizeof(kTexty[0]); i++) {
        unsigned el = (unsigned)strlen(kTexty[i]);
        if (len > el && nova::ieq(name + len - el, kTexty[i])) return true;
    }
    return false;
}

class FilesScreen : public Screen {
public:
    void begin(int level) {
        level_  = level;
        sel_    = 0;
        top_    = 0;
        report_ = false;
        gone_   = false;
        ready_  = true;
        relist();
    }

    // The directory IS the title. There is no room for a path row and the status
    // bar is already there — and it is set from the path rather than computed in
    // draw(), because the runner paints the bar BEFORE it calls draw() and a
    // title decided there shows up a frame late, carrying the previous
    // directory's name into the first frame of this one.
    const char *title(void) const override {
        if (!ready_) return "Files";
        // At a root, the store's name ("On-Board", "SD") reads better than the
        // path it happens to be ("/", "sd"). Deeper in, the folder name is what
        // tells you where you are.
        if (level_ == 0 && g_root_label[0]) return g_root_label;
        return basename(g_path[level_]);
    }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "SELECT opens the row.";
        out[1] = "BACK goes up; at / it quits.";
        out[2] = "Hold SELECT to delete it.";
        out[3] = "A folder must be empty first.";
        return 4;
    }

    // Called on the way in, and again every time a child screen pops — which is
    // what makes delete work: the confirmation removes the file and the browser
    // underneath has re-listed by the time it is drawn again. The guard is for
    // the very first call, which push_commit() makes before begin() has run.
    void enter(void) override { if (ready_) relist(); }

    // Only a FAILED delete is worth interrupting for; a successful one is
    // obvious, the row is gone. Reported from here rather than from inside the
    // confirmation's callback: pushing a notice there would put it under the
    // ACT_BACK the confirmation is about to return, and the pop would take the
    // notice instead of the question.
    bool tick(uint32_t dt) override {
        (void)dt;
        if (!report_) return false;
        report_ = false;
        if (!gone_)
            ui::notice("Delete", "That could not be removed. A folder has to be "
                                 "empty first.");
        return true;
    }

    void draw(Canvas &c) override {
        const int rows = ui::rows_for(c);
        if (!count_) {
            c.text(2, ui::TOP + ui::ROWH, "(empty folder)", 1);
            return;
        }

        if (sel_ < top_)              top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;
        if (top_ < 0) top_ = 0;
        sync(rows);

        const bool scrolls = count_ > rows;
        // The scrollbar lane is only taken when there is something to scroll, so
        // a short listing gets the whole width for its names.
        const int right = scrolls ? c.width() - (ui::SB_W + 1) : c.width();

        for (int i = 0; i < g_win_n; i++) {
            const FwDirEntry &e = g_win[i];
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (top_ + i == sel_);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
            const int col = on ? 0 : 1;

            if (e.is_dir) {
                // A trailing '/' marks a directory. It costs one character where
                // an icon would cost a column on every row, and it survives the
                // inversion of the selected row.
                char label[FW_NAME_MAX + 2];
                snprintf(label, sizeof(label), "%s/", e.name);
                c.text_fit(3, y, label, col, right - 6, false);
            } else {
                char sz[12];
                human(sz, sizeof(sz), e.size);
                const int sw = c.text_width(sz, 1, false);
                c.text_fit(3, y, e.name, col, right - sw - 8, false);
                c.text(right - sw - 2, y, sz, col);
            }
        }

        if (scrolls)
            c.scrollbar(right + 1, ui::TOP, c.height() - ui::TOP, top_, rows, count_);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW  && count_) { sel_ = (sel_ + 1) % count_; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW && count_) { sel_ = (sel_ + count_ - 1) % count_; return ui::ACT_STAY; }
        if (e == EV_SELECT      && count_) { open_sel();   return ui::ACT_STAY; }
        if (e == EV_SELECT_HOLD && count_) { ask_delete(); return ui::ACT_STAY; }
        // BACK is the default: it pops this level, which IS "up one directory",
        // and at the root it leaves the app.
        return Screen::on_event(e);
    }

private:
    int  level_, sel_, top_, count_;
    bool ready_, report_, gone_;

    void relist(void) {
        int n = fw_dir_count(g_path[level_]);
        count_ = n > 0 ? n : 0;
        // Clamp rather than reset. Coming back from a subdirectory should land
        // on the folder it was entered from, and deleting the last row should
        // leave the cursor on the new last row rather than past the end.
        if (sel_ >= count_) sel_ = count_ ? count_ - 1 : 0;
        if (sel_ < 0) sel_ = 0;
        g_win_ok = false;
    }

    void sync(int rows) {
        if (rows > WIN_MAX) rows = WIN_MAX;
        if (g_win_ok && g_win_level == level_ && g_win_first == top_ && g_win_rows == rows)
            return;
        g_win_n = 0;
        for (int i = 0; i < rows && top_ + i < count_; i++) {
            if (fw_dir_entry(g_path[level_], (unsigned)(top_ + i), &g_win[g_win_n]) != 1)
                break;
            g_win_n++;
        }
        g_win_ok    = true;
        g_win_level = level_;
        g_win_first = top_;
        g_win_rows  = rows;
    }

    // Read the selected entry straight from the filesystem rather than from the
    // window. An event can arrive before the first draw has filled it, and a
    // stale row is the difference between opening what is highlighted and
    // opening whatever used to be there.
    bool selected(FwDirEntry *out) const {
        return fw_dir_entry(g_path[level_], (unsigned)sel_, out) == 1;
    }

    void open_sel(void) {
        FwDirEntry e;
        if (!selected(&e)) return;

        if (e.is_dir) {
            // The held-back slot, spent here rather than found missing later.
            if (level_ >= LEVEL_MAX || gui::depth() + 1 >= gui::STACK_MAX) {
                ui::notice("Files", "As deep as the screen stack goes. The shell "
                                    "reaches the rest.");
                return;
            }
            const int child = level_ + 1;
            if (!join(g_path[child], PATH_MAX, g_path[level_], e.name)) {
                ui::notice("Files", "That path is longer than this screen can "
                                    "hold.");
                return;
            }
            FilesScreen *s = gui::push<FilesScreen>();
            if (s) s->begin(child);
            return;
        }

        if (!join(g_file, sizeof(g_file), g_path[level_], e.name)) return;
        if (texty(e.name)) {
            ViewScreen *v = gui::push<ViewScreen>();
            if (v) v->begin();
        } else {
            InfoScreen *v = gui::push<InfoScreen>();
            if (v) v->begin(e.size);
        }
    }

    // The MicroPython browser was read-only and said so: deleting from a 128x64
    // panel with three buttons can lose something, and the shell already has rm.
    // What it asked for was a confirmation of its own rather than a row on the
    // list, and this is that. A directory goes through the same path and simply
    // fails unless it is empty, which is the firmware's rule, not a check here.
    void ask_delete(void) {
        FwDirEntry e;
        if (!selected(&e)) return;
        if (!join(g_target, sizeof(g_target), g_path[level_], e.name)) return;
        // The question names the KIND as well as the thing, because "Delete
        // codes?" reads very differently once it is known to be a folder.
        if (e.is_dir) snprintf(g_question, sizeof(g_question), "Delete folder %s?", e.name);
        else          snprintf(g_question, sizeof(g_question), "Delete %s?", e.name);
        ui::confirm(g_question, "Delete", do_delete, this);
    }

    static void do_delete(void *ctx) {
        FilesScreen *s = (FilesScreen *)ctx;
        s->gone_   = fw_file_remove(g_target) != 0;
        s->report_ = true;
    }
};

// --- the storage roots -----------------------------------------------------------
//
// The top of the browser. On-board flash is always here; an SD card appears as a
// second root when one is mounted and vanishes when it is pulled — the firmware
// answers fw_storage_roots every time this screen asks, so a card coming or
// going is noticed while somebody is looking at the list.
//
// Why a level ABOVE the directory browser rather than a "/sd" folder inside "/":
// the two are different filesystems on different chips, and a card that is not
// there is not an empty folder — it is a row that says "no card", which a folder
// cannot. It also gives each store its own name and icon at the point of choice,
// which is what the person picking between them actually wants to see.

// A memory-chip glyph for on-board flash: a body with legs down each side.
static void root_ic_flash(Canvas &c, int x, int y, int col) {
    c.rect(x + 1, y, 5, 6, col);
    c.pixel(x, y + 1, col);   c.pixel(x, y + 3, col);      // left legs
    c.pixel(x + 6, y + 1, col); c.pixel(x + 6, y + 3, col); // right legs
    c.pixel(x + 3, y + 2, col); c.pixel(x + 3, y + 3, col); // a mark on the die
}

// An SD card: a body with the top-right corner cut off, the way a real one is
// keyed so it only goes in one way.
static void root_ic_sd(Canvas &c, int x, int y, int col) {
    // Outline with the corner notched.
    c.hline(x, y, 5, col);            // top, short of the corner
    c.line(x + 5, y, x + 6, y + 1, col);
    c.vline(x + 6, y + 1, 5, col);    // right
    c.hline(x, y + 6, 7, col);        // bottom
    c.vline(x, y, 7, col);            // left
    c.pixel(x + 2, y + 2, col);       // a contact
    c.pixel(x + 4, y + 2, col);
}

class RootsScreen : public Screen {
public:
    void begin(void) { sel_ = 0; ready_ = true; refresh(); }

    // Re-read on the way in too, so a card removed while down in its files is
    // gone from the list by the time BACK lands back here.
    void enter(void) override { if (ready_) refresh(); }

    // Its label must equal the catalogue's — novashots fails the build otherwise
    // — and "Files" is what the app is called.
    const char *title(void) const override { return "Files"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Pick where to look.";
        out[1] = "On-Board is the device; SD is";
        out[2] = "the card, when one is in.";
        return 3;
    }

    // The card is hot-pluggable, so poll for a change and redraw when the set of
    // roots moves. fw_storage_roots is a cheap firmware call; once a second is
    // plenty and costs nothing a person would see.
    bool tick(uint32_t dt) override {
        poll_ += dt;
        if (poll_ < 1000) return false;
        poll_ = 0;
        int was = n_;
        uint8_t sig = present_sig();
        refresh();
        return n_ != was || sig != present_sig_prev_;
    }

    void draw(Canvas &c) override {
        if (n_ <= 0) { c.text(2, ui::TOP, "(no storage)", 1); return; }
        const int right = c.width();
        for (int i = 0; i < n_; i++) {
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (i == sel_);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
            const int col = on ? 0 : 1;

            if (roots_[i].kind == FW_ROOT_SD) root_ic_sd(c, 3, y, col);
            else                              root_ic_flash(c, 3, y, col);
            c.text(14, y, roots_[i].label, col);

            char v[16];
            if (!roots_[i].present) {
                nova::copy(v, sizeof(v), "no card");
            } else if (roots_[i].total_kb) {
                human_kb(v, sizeof(v), roots_[i].free_kb);
            } else {
                v[0] = 0;
            }
            if (v[0]) {
                int w = c.text_width(v, 1, false);
                c.text(right - w - 2, y, v, col);
            }
        }
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW  && n_) { sel_ = (sel_ + 1) % n_; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW && n_) { sel_ = (sel_ + n_ - 1) % n_; return ui::ACT_STAY; }
        if (e == EV_SELECT  && n_) { open_sel(); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    FwStorageRoot roots_[4];
    int      n_, sel_;
    bool     ready_;
    uint32_t poll_;
    uint8_t  present_sig_prev_;

    // A one-byte fingerprint of which roots are present, so tick() can tell a
    // card going in or out from a mere capacity update.
    uint8_t present_sig(void) const {
        uint8_t s = 0;
        for (int i = 0; i < n_ && i < 8; i++) if (roots_[i].present) s |= (uint8_t)(1 << i);
        return s;
    }

    void refresh(void) {
        int got = fw_storage_roots(roots_, 4);
        n_ = got > 0 ? got : 0;
        if (sel_ >= n_) sel_ = n_ ? n_ - 1 : 0;
        present_sig_prev_ = present_sig();
    }

    void open_sel(void) {
        const FwStorageRoot &r = roots_[sel_];
        if (!r.present) {
            ui::notice(r.label, "No card is in the slot. Push one in and it will "
                                "appear here.");
            return;
        }
        nova::copy(g_path[0], PATH_MAX, r.path);
        nova::copy(g_root_label, sizeof(g_root_label), r.label);
        FilesScreen *s = gui::push<FilesScreen>();
        if (s) s->begin(0);
    }
};

void open_files(void) {
    RootsScreen *s = gui::push<RootsScreen>();
    if (s) s->begin();
}

// --- a list of lines, newest first ------------------------------------------------
//
// Logs and Alerts are the same screen twice: entries somebody scrolls through
// and one destructive action behind a question. The two differ only in where the
// entries come from, so they share this and override three methods.

constexpr int      ROW_MAX   = 8;       // more rows than a 64-pixel panel has
constexpr unsigned ROW_CHARS = 24;      // one panel width, plus the terminator
constexpr unsigned ENTRY_MAX = 96;      // the longest line novalog will hold

static char g_rows[ROW_MAX][ROW_CHARS];
static int  g_row_n;                    // rows filled
static int  g_row_entries;              // entries those rows came from
static int  g_row_top, g_row_rows;
static const void *g_row_owner;         // which screen filled them, null for none

class LineList : public Screen {
public:
    void begin(void) { top_ = 0; ready_ = true; reload(); }

    void enter(void) override { if (ready_) reload(); }

    void draw(Canvas &c) override {
        const int rows = ui::rows_for(c);
        if (!count_) {
            c.text(2, ui::TOP, nothing(), 1);
            return;
        }
        sync(c, rows);
        for (int i = 0; i < g_row_n; i++)
            c.text(2, ui::TOP + i * ui::ROWH, g_rows[i], 1);
        c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP,
                    top_, g_row_entries, count_);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW) {
            // By ENTRY, not by wrapped row. Knowing which wrapped row an entry
            // starts at means wrapping every entry above it, and for the log
            // that is the whole file read once per entry — so the encoder moves
            // between entries and each one is wrapped only while it is on the
            // panel. Nothing becomes unreachable: an entry too tall for what is
            // left of the screen is shown in full once it reaches the top.
            if (top_ + 1 < count_) { top_++; stale(); }
            return ui::ACT_STAY;
        }
        if (e == EV_ROT_CCW) {
            if (top_ > 0) { top_--; stale(); }
            return ui::ACT_STAY;
        }
        if (e == EV_SELECT && count_) {
            ui::confirm(question(), "Clear", wipe_cb, this);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

protected:
    // Defaults rather than pure virtuals: a pure virtual generates a reference
    // to __cxa_pure_virtual, which the firmware does not export.
    virtual int  total(void) { return 0; }
    virtual bool fetch(int i, char *out, unsigned cap) { (void)i; (void)out; (void)cap; return false; }
    virtual void wipe(void) {}
    virtual const char *nothing(void) const { return "(nothing here)"; }
    virtual const char *question(void) const { return "Clear everything?"; }

    void reload(void) {
        count_ = total();
        if (top_ >= count_) top_ = count_ ? count_ - 1 : 0;
        if (top_ < 0) top_ = 0;
        stale();
    }

    int  top_, count_;
    bool ready_;

private:
    void stale(void) { g_row_owner = nullptr; }

    // Wrap as many entries as the panel will take. Keyed on the screen and the
    // scroll position, so DRAW ITSELF MAKES NO ABI CALL — a refill costs one
    // fetch per visible entry and only happens when something moved.
    void sync(Canvas &c, int rows) {
        if (rows > ROW_MAX) rows = ROW_MAX;
        if (g_row_owner == this && g_row_top == top_ && g_row_rows == rows) return;

        const int cols = c.cols() - 1;          // the scrollbar lane
        g_row_n = 0;
        g_row_entries = 0;
        char text[ENTRY_MAX];
        char store[ENTRY_MAX + 8];
        const char *lines[ROW_MAX];
        for (int i = top_; i < count_ && g_row_n < rows; i++) {
            if (!fetch(i, text, sizeof(text))) break;
            int n = ui::wrap(text, cols, store, sizeof(store), lines, rows - g_row_n);
            // An empty entry still takes its row. Swallowing it would make the
            // list shorter than the count beside it and the scrollbar wrong.
            if (!n) nova::copy(g_rows[g_row_n++], ROW_CHARS, "");
            for (int k = 0; k < n && g_row_n < rows; k++)
                nova::copy(g_rows[g_row_n++], ROW_CHARS, lines[k]);
            g_row_entries++;
        }
        g_row_owner = this;
        g_row_top   = top_;
        g_row_rows  = rows;
    }

    static void wipe_cb(void *ctx) {
        LineList *s = (LineList *)ctx;
        s->wipe();
        s->top_ = 0;
        // The reload happens in enter(), which the pop of the confirmation
        // runs — doing it here would read a store the wipe has just emptied and
        // then read it again a moment later anyway.
    }
};

// --- Logs -------------------------------------------------------------------------
//
// The Nova D1's own event log, newest first. The MicroPython version handed
// novalog.tail(40) to a generic text screen, which meant forty lines in RAM at
// once; this reads the entries that are on the panel and no others.

class LogsScreen : public LineList {
public:
    const char *title(void) const override { return "Logs"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Newest first. Turn to scroll.";
        out[1] = "SELECT clears the log.";
        out[2] = "Sixty lines are kept.";
        return 3;
    }

protected:
    int  total(void) override { return log::count(); }
    bool fetch(int i, char *out, unsigned cap) override { return log::line(i, out, cap); }
    void wipe(void) override { log::clear(); }
    const char *nothing(void) const override { return "(no log yet)"; }
    const char *question(void) const override { return "Clear the whole log?"; }
};

void open_logs(void) {
    LogsScreen *s = gui::push<LogsScreen>();
    if (s) s->begin();
}

// --- Alerts -----------------------------------------------------------------------
//
// The notification queue. Opening it marks everything read, which is what clears
// the count in the status bar.

class AlertsScreen : public LineList {
public:
    const char *title(void) const override { return "Alerts"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Newest first. Turn to scroll.";
        out[1] = "SELECT clears them all.";
        out[2] = "What matters is in the log.";
        return 3;
    }

    // Also runs when a confirmation pops back to here, which is harmless —
    // marking read twice is marking read.
    void enter(void) override {
        notify::mark_read();
        LineList::enter();
    }

    // The queue is live: anything on the device can post while this is open, and
    // a list of alerts that does not show the one that just arrived is the one
    // screen where that is obviously wrong. The count is a plain read of a ring
    // in RAM, so asking every frame costs nothing — which is why Logs, whose
    // count means reading the file, does not do this.
    bool tick(uint32_t dt) override {
        (void)dt;
        if (notify::count() == count_) return false;
        notify::mark_read();
        reload();
        return true;
    }

protected:
    int  total(void) override { return notify::count(); }
    bool fetch(int i, char *out, unsigned cap) override { return notify::at(i, out, cap); }
    void wipe(void) override { notify::clear(); }
    const char *nothing(void) const override { return "(no notifications)"; }
    const char *question(void) const override { return "Clear all alerts?"; }
};

void open_alerts(void) {
    AlertsScreen *s = gui::push<AlertsScreen>();
    if (s) s->begin();
}

}  // namespace screens
}  // namespace nova
