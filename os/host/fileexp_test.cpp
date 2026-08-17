// fileexp_test — the file browser, driven by a scripted keyboard.
//
// Two kinds of question, and the second is the reason this file is long.
//
// The arithmetic — joining, sorting, the parent of a path, what a filter
// matches — is ordinary and testable, and every one of those has a wrong answer
// that looks plausible on screen.
//
// The other kind cannot be seen at all. This package turns a FILENAME INTO A
// COMMAND LINE, and the shell it hands that line to honours double quotes and
// has no escape character. A file called
//     x" ; rm -r /home
// closes the quoting and the rest is command. Nothing about running it would
// look like an error; the browser would refresh and the directory would be
// gone. So the fake shell here RECORDS what it was asked to run, and the tests
// assert on that string rather than on whether the command appeared to work.
//
// The screen is faked as a grid of characters, which is what makes "is the
// selected row highlighted, does the footer say how many entries were dropped"
// into questions with exact answers instead of something to squint at over a
// serial line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <map>
#include <vector>
#include <deque>

#include "../include/rpc_app.h"

static int g_checks, g_fails;
static void ck(bool cond, const char *what) {
    g_checks++;
    if (!cond) { g_fails++; printf("  FAIL: %s\n", what); }
}

// --- a filesystem in a map --------------------------------------------------

struct FeNode { bool is_dir; std::string data; };
static std::map<std::string, FeNode> g_fs;

static void fs_reset(void) { g_fs.clear(); }
static void fs_dir(const char *p)  { g_fs[p] = FeNode{true, ""}; }
static void fs_file(const char *p, const std::string &d) { g_fs[p] = FeNode{false, d}; }
static bool fs_has(const char *p)  { return g_fs.count(p) != 0; }

static std::vector<std::string> fs_children(const char *dir) {
    std::string prefix = dir;
    if (prefix.empty() || prefix.back() != '/') prefix += '/';
    std::vector<std::string> out;
    for (auto &kv : g_fs) {
        if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
        std::string rest = kv.first.substr(prefix.size());
        if (rest.empty() || rest.find('/') != std::string::npos) continue;
        out.push_back(rest);
    }
    return out;
}

// --- a screen of characters -------------------------------------------------

#define FE_W 80
#define FE_H 24
struct FeCell { char ch; unsigned char attr; unsigned char fg; };
static FeCell g_cells[FE_H][FE_W];
static FeCell g_shown[FE_H][FE_W];      // what the last present() sent

static void screen_clear(void) {
    for (int y = 0; y < FE_H; y++)
        for (int x = 0; x < FE_W; x++) g_cells[y][x] = FeCell{' ', 0, 0};
}

static std::string screen_row(int y) {
    std::string s;
    for (int x = 0; x < FE_W; x++) s += g_shown[y][x].ch;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
static bool screen_has(const char *needle) {
    for (int y = 0; y < FE_H; y++)
        if (screen_row(y).find(needle) != std::string::npos) return true;
    return false;
}
// The row a piece of text landed on, or -1.
static int screen_find(const char *needle) {
    for (int y = 0; y < FE_H; y++)
        if (screen_row(y).find(needle) != std::string::npos) return y;
    return -1;
}
static bool screen_row_reversed(int y) {
    for (int x = 0; x < FE_W; x++)
        if (g_shown[y][x].attr & FW_ATTR_REVERSE) return true;
    return false;
}

// --- scripted input ---------------------------------------------------------

static std::deque<FwTuiEvent> g_keys;

static void key(int k) {
    FwTuiEvent e{};
    e.kind = 1;
    e.key = k;
    g_keys.push_back(e);
}
static void type(const char *s) { for (const char *p = s; *p; p++) key(*p); }
static void mouse(int kind, int x, int y) {
    FwTuiEvent e{};
    e.kind = 2;
    e.mouse = (unsigned char)kind;
    e.x = (unsigned short)x;
    e.y = (unsigned short)y;
    g_keys.push_back(e);
}

// --- the ABI ----------------------------------------------------------------

static char       g_out[8192];
static unsigned   g_out_len;
static void       out_reset(void) { g_out_len = 0; g_out[0] = 0; }

// Every command line the package asked the shell to run, in order. The point of
// the whole file.
static std::vector<std::string> g_ran;
// What the fake shell prints back, so the status line can be checked.
static std::string g_shell_reply;
static int         g_shell_rc;
static bool        g_tui_active;
static int         g_begin_count, g_end_count;
static unsigned    g_alloc_fail_over;   // refuse allocations bigger than this

static RpcCommandFn g_cmd;
static int          g_cmds_registered;

extern "C" {
int fw_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_out + g_out_len, sizeof(g_out) - g_out_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_out_len += (unsigned)n;
    return n;
}
void *fw_malloc(size_t n) {
    if (g_alloc_fail_over && n > g_alloc_fail_over) return nullptr;
    return malloc(n ? n : 1);
}
void  fw_free(void *p) { free(p); }

int fw_file_exists(const char *p) { return fs_has(p) ? 1 : 0; }
uint32_t fw_file_size(const char *p) {
    auto it = g_fs.find(p);
    return (it == g_fs.end() || it->second.is_dir) ? 0 : (uint32_t)it->second.data.size();
}
uint32_t fw_file_read_at(const char *p, uint32_t off, void *buf, uint32_t cap) {
    auto it = g_fs.find(p);
    if (it == g_fs.end() || it->second.is_dir) return 0;
    const std::string &d = it->second.data;
    if (off >= d.size()) return 0;
    uint32_t n = (uint32_t)d.size() - off;
    if (n > cap) n = cap;
    memcpy(buf, d.data() + off, n);
    return n;
}
int fw_file_write(const char *p, const void *data, uint32_t len) {
    g_fs[p] = FeNode{false, std::string((const char *)data, len)};
    return 1;
}
int fw_file_remove(const char *p) {
    auto it = g_fs.find(p);
    if (it == g_fs.end()) return 0;
    if (it->second.is_dir && !fs_children(p).empty()) return 0;   // empty dirs only
    g_fs.erase(it);
    return 1;
}
int fw_mkdir(const char *p) {
    if (fs_has(p)) return 0;
    fs_dir(p);
    return 1;
}
int fw_dir_count(const char *p) {
    auto it = g_fs.find(p);
    if (it == g_fs.end() || !it->second.is_dir) return -1;
    return (int)fs_children(p).size();
}
int fw_dir_entry(const char *p, unsigned index, FwDirEntry *out) {
    auto it = g_fs.find(p);
    if (it == g_fs.end() || !it->second.is_dir) return -1;
    auto kids = fs_children(p);
    if (index >= kids.size()) return 0;
    snprintf(out->name, sizeof(out->name), "%s", kids[index].c_str());
    std::string full = std::string(p) + (kids[index].empty() ? "" : "/") + kids[index];
    if (strcmp(p, "/") == 0) full = "/" + kids[index];
    out->is_dir = g_fs[full].is_dir ? 1 : 0;
    out->size   = (uint32_t)g_fs[full].data.size();
    return 1;
}

int fw_shell_run(const char *line, char *out, uint32_t cap) {
    g_ran.push_back(line);
    if (out && cap) snprintf(out, cap, "%s", g_shell_reply.c_str());
    return g_shell_rc;
}

void fw_tui_begin(void) { g_begin_count++; g_tui_active = true; screen_clear(); }
void fw_tui_end(void)   { g_end_count++;   g_tui_active = false; }
void fw_tui_size(int *w, int *h) { if (w) *w = FE_W; if (h) *h = FE_H; }
void fw_tui_clear(void) { screen_clear(); }

void fw_tui_text(int x, int y, const char *s, unsigned char attr, unsigned char fg) {
    if (y < 0 || y >= FE_H) return;
    for (int i = 0; s[i] && x + i < FE_W; i++)
        if (x + i >= 0) g_cells[y][x + i] = FeCell{s[i], attr, fg};
}
void fw_tui_box(int x, int y, int w, int h, const char *title,
                unsigned char attr, unsigned char fg) {
    // Only the title matters to a test; the border is the firmware's business.
    if (title) fw_tui_text(x + 2, y, title, attr, fg);
    (void)w; (void)h;
}
void fw_tui_fill(int x, int y, int w, int h, char ch, unsigned char attr, unsigned char fg) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (y + j >= 0 && y + j < FE_H && x + i >= 0 && x + i < FE_W)
                g_cells[y + j][x + i] = FeCell{ch, attr, fg};
}
void fw_tui_present(void) { memcpy(g_shown, g_cells, sizeof(g_cells)); }
int  fw_tui_refresh(void) { return 0; }

int fw_tui_poll(FwTuiEvent *out) {
    if (g_keys.empty()) return 0;
    *out = g_keys.front();
    g_keys.pop_front();
    return 1;
}

// Once the script has run out there is nothing more coming, so the wait
// unwinds the same way Ctrl+C would rather than spinning for ever.
int  fw_task_should_stop(void) { return g_keys.empty() ? 1 : 0; }
void fw_task_sleep_ms(uint32_t) {}

int rpc_register_command(const char *, const char *, RpcCommandFn fn) {
    g_cmds_registered++;
    g_cmd = fn;
    return 1;
}
}  // extern "C"

#include "../apps/fileexp/fileexp.cpp"

// --- driving ----------------------------------------------------------------

static int run(const char *arg = nullptr) {
    out_reset();
    g_ran.clear();
    char  buf[64];
    char *argv[2];
    int   argc = 1;
    argv[0] = (char *)"files";
    if (arg) { snprintf(buf, sizeof(buf), "%s", arg); argv[argc++] = buf; }
    return g_cmd(argc, argv);
}

static void fs_populate(void) {
    fs_reset();
    fs_dir("/");
    fs_dir("/home");
    fs_dir("/os");
    fs_dir("/etc");
    fs_file("/notes.txt", "first line\nsecond line\nthird line\n");
    fs_file("/Alpha.cfg", "a=1\n");
    fs_file("/zebra.app", std::string(2048, 'z'));
}

int main(void) {
    printf("fileexp_test - paths, listings, and the command line a filename becomes\n");

    app_main(0);
    ck(g_cmds_registered == 3, "the package registers files, fm and explorer");
    ck(g_cmd != nullptr, "and the entry point is reachable");
    if (!g_cmd) return 1;

    // --- joining ------------------------------------------------------------
    {
        char p[64];
        ck(fx_join(p, sizeof(p), "/home", "notes.txt"), "a join fits");
        ck(strcmp(p, "/home/notes.txt") == 0, "and reads as expected");
        // The root already ends in a separator; "//notes" is a different path.
        ck(fx_join(p, sizeof(p), "/", "notes.txt"), "a join at the root fits");
        ck(strcmp(p, "/notes.txt") == 0, "and does not double the slash");
        char small[10];
        ck(!fx_join(small, sizeof(small), "/home", "notes.txt"),
           "a join that would not fit is refused, not truncated");
        char tight[12];
        ck(!fx_join(tight, sizeof(tight), "/verylongdirectory", "x"),
           "and so is one whose directory alone does not fit");
    }

    // --- parents ------------------------------------------------------------
    {
        char p[64];
        fx_parent(p, sizeof(p), "/home/dash/notes.txt");
        ck(strcmp(p, "/home/dash") == 0, "the parent of a deep path");
        fx_parent(p, sizeof(p), "/home");
        ck(strcmp(p, "/") == 0, "the parent of a top-level folder is the root");
        // The one that matters: without this, going up walks off the top for ever.
        fx_parent(p, sizeof(p), "/");
        ck(strcmp(p, "/") == 0, "the root is its own parent");
        fx_parent(p, sizeof(p), "/home/dash/");
        ck(strcmp(p, "/home") == 0, "a trailing slash does not cost a level");
    }

    // --- comparing and matching --------------------------------------------
    {
        ck(fx_casecmp("apple", "Banana") < 0, "case is ignored when sorting");
        ck(fx_casecmp("Zebra", "apple") > 0, "in both directions");
        ck(fx_casecmp("same", "SAME") == 0, "equal but for case is equal");
        ck(fx_casecmp("ab", "abc") < 0, "a prefix sorts first");
        ck(fx_contains("Notes.txt", "note"), "the filter ignores case");
        ck(fx_contains("Notes.txt", "txt"), "and matches at the end");
        ck(!fx_contains("Notes.txt", "zzz"), "and does not match what is absent");
        ck(fx_contains("anything", ""), "an empty filter matches everything");
    }

    // --- sizes --------------------------------------------------------------
    {
        char s[16];
        fx_size_str(s, sizeof(s), 900);      ck(strcmp(s, "900 B") == 0, "bytes");
        fx_size_str(s, sizeof(s), 1536);     ck(strcmp(s, "1.5 K") == 0, "kilobytes");
        fx_size_str(s, sizeof(s), 1572864);  ck(strcmp(s, "1.5 M") == 0, "megabytes");
    }

    // --- THE ONE THAT MATTERS ----------------------------------------------
    {
        ck(fx_shell_safe("notes.txt"), "an ordinary name is safe");
        ck(fx_shell_safe("my notes.txt"), "a space is safe - quoting covers it");
        ck(fx_shell_safe("a;b|c&d>e"), "and so are the shell's own connectors");
        ck(!fx_shell_safe("x\"y"), "a double quote is NOT safe - it closes the quoting");
        ck(!fx_shell_safe("x\ny"), "a newline is not safe");
        ck(!fx_shell_safe("x\ty"), "nor a tab");
        ck(!fx_shell_safe(""), "nor an empty name");

        char line[FX_LINE_MAX];
        ck(fx_build(line, sizeof(line), "cp", "/a b.txt", "/c"), "a safe pair builds");
        ck(strcmp(line, "cp \"/a b.txt\" \"/c\"") == 0, "quoted on both sides");
        ck(fx_build(line, sizeof(line), "edit", "/a", 0), "and so does a single path");
        ck(strcmp(line, "edit \"/a\"") == 0, "with one pair of quotes");

        // The whole reason for the check. This name would otherwise become
        //     cp "/x" ; rm -r /home" "/dest"
        // and the deletion would run.
        line[0] = 0;
        ck(!fx_build(line, sizeof(line), "cp", "/x\" ; rm -r /home", "/dest"),
           "a name carrying a quote is refused before it reaches the shell");
        ck(line[0] == 0, "and nothing was built");
        ck(!fx_build(line, sizeof(line), "cp", "/safe", "/dest\" ; reboot"),
           "the DESTINATION is checked too, not just the source");

        // Too long to run is refused rather than sent truncated, which would
        // name a different file.
        char huge[FX_PATH_MAX];
        memset(huge, 'a', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = 0;
        ck(!fx_build(line, sizeof(line), "cp", huge, huge),
           "a pair that overflows the command line is refused");
    }

    // --- the last line of a reply ------------------------------------------
    {
        char s[64];
        fx_last_line(s, sizeof(s), "  working\n  Could not copy /a\n");
        ck(strcmp(s, "Could not copy /a") == 0, "the last real line, unindented");
        fx_last_line(s, sizeof(s), "only one line");
        ck(strcmp(s, "only one line") == 0, "a reply with no newline");
        fx_last_line(s, sizeof(s), "answer\n\n   \n");
        ck(strcmp(s, "answer") == 0, "blank trailing lines are skipped");
        fx_last_line(s, sizeof(s), "");
        ck(s[0] == 0, "an empty reply gives an empty summary");
    }

    // --- sorting ------------------------------------------------------------
    {
        FxEntry e[5];
        const char *names[] = {"zebra.txt", "Apple", "beta", "Zoo", "alpha.cfg"};
        const int   dirs[]  = {0, 1, 0, 1, 0};
        for (int i = 0; i < 5; i++) {
            snprintf(e[i].name, sizeof(e[i].name), "%s", names[i]);
            e[i].is_dir = (unsigned char)dirs[i];
            e[i].size = 0;
        }
        fx_sort(e, 5);
        ck(strcmp(e[0].name, "Apple") == 0, "folders come first");
        ck(strcmp(e[1].name, "Zoo") == 0, "and are sorted among themselves");
        ck(strcmp(e[2].name, "alpha.cfg") == 0, "then files, ignoring case");
        ck(strcmp(e[3].name, "beta") == 0, "in order");
        ck(strcmp(e[4].name, "zebra.txt") == 0, "to the end");
    }

    // --- opening the browser ------------------------------------------------
    {
        fs_populate();
        key('q');
        int rc = run();
        ck(rc == 0, "the browser opens and closes cleanly");
        ck(g_begin_count == g_end_count,
           "every fw_tui_begin was paired with an end - a terminal left in mouse "
           "mode types by itself afterwards");
        ck(screen_has("Files"), "the title is drawn");
        ck(screen_has("notes.txt"), "a file is listed");
        ck(screen_has("etc/"), "a folder is listed with its slash");
        ck(screen_has("DIR"), "and marked as one");
        ck(screen_has("2.0 K"), "a file's size is shown");
        ck(screen_has("Enter open"), "the key hints are there");

        // Folders first, then files: /etc, /home, /os, then Alpha.cfg...
        int etc = screen_find("etc/"), alpha = screen_find("Alpha.cfg");
        ck(etc > 0 && alpha > etc, "folders are listed above files");
        ck(screen_row_reversed(3), "the first row is the highlighted one");
    }

    // --- moving about -------------------------------------------------------
    {
        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN);        // to /os
        key(13);                                    // open it
        key('q');
        run();
        ck(screen_has("/os"), "Enter on a folder walks into it");

        fs_populate();
        fs_file("/home/deep.txt", "x");
        key(FW_KEY_DOWN);                           // /home
        key(13);
        key(FW_KEY_LEFT);                           // back up
        key('q');
        run();
        ck(screen_has("deep.txt") == false, "going up leaves the folder");
        ck(screen_has("notes.txt"), "and shows the parent again");

        // The root is its own parent, so this must not walk off the top.
        fs_populate();
        for (int i = 0; i < 5; i++) key(FW_KEY_LEFT);
        key('q');
        run();
        ck(screen_has("notes.txt"), "going up from the root stays at the root");
    }

    // --- the filter ---------------------------------------------------------
    {
        fs_populate();
        key('/'); type("txt"); key(13);
        key('q');
        run();
        ck(screen_has("notes.txt"), "the filter keeps what matches");
        ck(!screen_has("Alpha.cfg"), "and drops what does not");
        ck(screen_has("[/txt]"), "the title says a filter is on");

        fs_populate();
        key('/'); type("nothingmatchesthis"); key(13);
        key('q');
        run();
        ck(screen_has("nothing matches"), "a filter matching nothing says so");

        // Escape must not be taken as "clear the filter" - it means "leave it".
        fs_populate();
        key('/'); type("txt"); key(13);
        key('/'); type("zzz"); key(FW_KEY_ESC);
        key('q');
        run();
        ck(screen_has("[/txt]"), "Escape at the prompt keeps the previous filter");
    }

    // --- the actions, and what they ask the shell to run --------------------
    {
        g_shell_reply = "";
        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);   // past the folders to Alpha.cfg
        key(13);                                    // edit it
        key('q');
        run();
        bool edited = false;
        for (auto &s : g_ran) if (s == "edit \"/Alpha.cfg\"") edited = true;
        ck(edited, "Enter on a file opens it in the built-in editor");
        ck(g_end_count == g_begin_count,
           "and the screen was handed back and taken again in pairs");
    }
    {
        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);   // Alpha.cfg
        key('c'); type("/home"); key(13);
        key('q');
        run();
        ck(g_ran.size() == 1, "copy runs exactly one command");
        // /home is a folder, so the destination becomes the file inside it.
        ck(g_ran.size() && g_ran[0] == "cp \"/Alpha.cfg\" \"/home/Alpha.cfg\"",
           "copying to a folder puts the file inside it");
    }
    {
        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);
        key('c'); type("/home/renamed.cfg"); key(13);
        key('q');
        run();
        ck(g_ran.size() && g_ran[0] == "cp \"/Alpha.cfg\" \"/home/renamed.cfg\"",
           "copying to a path that is not a folder uses it as the name");
    }
    {
        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);
        key('R'); type("Beta.cfg"); key(13);
        key('q');
        run();
        ck(g_ran.size() && g_ran[0] == "rename \"/Alpha.cfg\" \"Beta.cfg\"",
           "rename passes the bare new name, which is what the shell wants");
    }
    {
        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);
        key('m'); type("/home"); key(13);
        key('q');
        run();
        ck(g_ran.size() && g_ran[0] == "mv \"/Alpha.cfg\" \"/home/Alpha.cfg\"",
           "move uses mv, not cp");
    }
    {
        // A copy of a folder is refused: cp does not recurse, and half a folder
        // copied is worse than none.
        fs_populate();
        key('c');
        key('q');
        run();
        ck(g_ran.empty(), "copying a folder runs nothing");
        ck(screen_has("does not recurse"), "and says why");
    }
    {
        // Move IS allowed for a folder - mv renames it in place.
        fs_populate();
        key('m'); type("/home/etc"); key(13);
        key('q');
        run();
        ck(g_ran.size() && g_ran[0] == "mv \"/etc\" \"/home/etc\"",
           "moving a folder is allowed");
    }

    // --- installing ---------------------------------------------------------
    {
        fs_populate();
        key(FW_KEY_END);                            // zebra.app, last
        key('p');
        key('q');
        run();
        ck(g_ran.size() && g_ran[0] == "pkg install \"/zebra.app\"",
           "p on a .app asks pkg to install it");

        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);   // Alpha.cfg
        key('p');
        key('q');
        run();
        ck(g_ran.empty(), "p on something else runs nothing");
        ck(screen_has(".app file"), "and says what it wants");
    }

    // --- INJECTION, end to end ---------------------------------------------
    {
        fs_populate();
        fs_file("/x\" ; reboot", "gotcha");
        // It sorts under 'x', after the folders and the other files.
        key(FW_KEY_END);
        key(FW_KEY_UP);                             // off zebra.app onto the trap
        key('c'); type("/home"); key(13);
        key('q');
        run();
        ck(g_ran.empty(),
           "a filename carrying a quote never reaches the shell");
        ck(screen_has("refused"), "and the browser says it was refused");
        ck(g_ran.empty(), "so nothing at all was handed to the shell");

        fs_populate();
        fs_file("/x\" ; reboot", "gotcha");
        key(FW_KEY_END); key(FW_KEY_UP);
        key(13);                                    // try to EDIT it
        key('q');
        run();
        ck(g_ran.empty(), "and neither does opening it in the editor");
    }

    // --- new file and new folder -------------------------------------------
    {
        fs_populate();
        key('n'); type("newdir/"); key(13);
        key('q');
        run();
        ck(fs_has("/newdir"), "a name ending in / makes a folder");
        ck(g_ran.empty(), "and does not need the shell to do it");
        ck(screen_has("Created folder"), "and says so");

        fs_populate();
        key('n'); type("fresh.txt"); key(13);
        key('q');
        run();
        ck(fs_has("/fresh.txt"), "a plain name makes an empty file");
        ck(g_ran.size() && g_ran[0] == "edit \"/fresh.txt\"",
           "and opens it in the editor, the way v1 did");

        fs_populate();
        key('n'); type("notes.txt"); key(13);
        key('q');
        run();
        ck(screen_has("already there"), "an existing name is refused");
        ck(g_ran.empty(), "and nothing was run");
    }

    // --- deleting -----------------------------------------------------------
    {
        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);   // Alpha.cfg
        key('d'); type("y"); key(13);
        key('q');
        run();
        ck(!fs_has("/Alpha.cfg"), "d then y deletes the file");
        ck(screen_has("Deleted"), "and says so");

        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);
        key('d'); key(13);                          // just Enter: not a yes
        key('q');
        run();
        ck(fs_has("/Alpha.cfg"), "Enter alone at the prompt is not a yes");
        ck(screen_has("Cancelled"), "and it says it cancelled");

        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN);
        key('d'); type("n"); key(13);
        key('q');
        run();
        ck(fs_has("/Alpha.cfg"), "'n' does not delete either");

        // A folder with things in it. v1 could only remove empty ones and so
        // can this - a whole tree going on one keystroke is not a mistake
        // anybody should be able to make by accident.
        fs_populate();
        fs_file("/home/keep.txt", "x");
        key(FW_KEY_DOWN);                           // /home
        key('d'); type("y"); key(13);
        key('q');
        run();
        ck(fs_has("/home"), "a folder with contents is not deleted");
        ck(fs_has("/home/keep.txt"), "and its contents are untouched");
        ck(screen_has("not empty"), "and it says why");
    }

    // --- the viewer ---------------------------------------------------------
    {
        fs_populate();
        key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN); key(FW_KEY_DOWN); // notes.txt
        key('v');
        key('q');                                   // leave the viewer
        key('q');                                   // leave the browser
        run();
        ck(screen_has("notes.txt"), "the viewer names the file");
        // The last present() before quitting is the browser again, so check the
        // viewer ran at all by what it left on screen mid-way is not reliable -
        // instead check it did not fall through to the editor.
        ck(g_ran.empty(), "viewing does not open the editor");

        fs_populate();
        key(FW_KEY_DOWN);                           // a folder
        key('v');
        key('q');
        run();
        ck(g_ran.empty(), "v on a folder does nothing rather than misbehaving");
    }

    // --- a listing too big to hold -----------------------------------------
    {
        fs_reset();
        fs_dir("/");
        char name[64];
        for (int i = 0; i < 300; i++) {
            snprintf(name, sizeof(name), "/f%03d.txt", i);
            fs_file(name, "x");
        }
        key('q');
        run();
        ck(screen_has("of 300 items"),
           "a directory with more entries than fit says how many it dropped");
        ck(screen_has("too many to hold"), "in words, not just numbers");
    }

    // --- a heap with nothing big enough ------------------------------------
    {
        fs_populate();
        g_alloc_fail_over = 16;          // every listing allocation fails
        int rc = run();
        ck(rc != 0, "with no block big enough the browser refuses to open");
        ck(strstr(g_out, "no block big enough") != nullptr, "and says that");
        g_alloc_fail_over = 0;
        g_keys.clear();
    }

    // --- the mouse ----------------------------------------------------------
    {
        fs_populate();
        mouse(0, 10, 5);                            // click the third row
        key('d'); type("y"); key(13);
        key('q');
        run();
        ck(!fs_has("/os"), "clicking a row selects it");
        ck(fs_has("/etc"), "and only that one");
    }

    // --- arguments ----------------------------------------------------------
    {
        fs_populate();
        ck(run("help") == 0, "'files help' prints the keys");
        ck(strstr(g_out, "R rename") != nullptr, "including rename");
        ck(g_begin_count == g_end_count, "and never opens the screen at all");

        fs_populate();
        key('q');
        run("/home");
        ck(screen_has("/home"), "a path argument starts there");

        fs_populate();
        key('q');
        run("/notes.txt");
        ck(strstr(g_out, "not a folder") != nullptr,
           "a file as the argument says so rather than starting somewhere odd");
        ck(screen_has("notes.txt"), "and falls back to the root");
    }

    printf("  %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
