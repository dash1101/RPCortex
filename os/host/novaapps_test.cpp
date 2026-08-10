// Third-party Nova D1 apps: the scan, the manifest, and every way one is wrong.
//
// This is the half of the app framework that has no screen in it, and that is
// deliberate — a manifest is DATA, so every decision made about one can be
// checked here rather than by looking at a panel. novagui_test drives the
// screen; novashots photographs it; this proves what they are given.
//
// The fake filesystem is a table the test rewrites between cases, because most
// of the interesting questions are about a directory whose contents are wrong:
// a file that is not an app, a file that went away between the listing and the
// read, more apps than the table holds, a listing that changed underneath.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../include/rpc_app.h"

// --- the firmware, faked -----------------------------------------------------------

struct FakeFile {
    const char *name;
    int         is_dir;
    const char *body;         // null means a file that cannot be read
};

static FakeFile g_files[16];
static int      g_files_n;
static const char *g_dir = "/nova/apps";

// A file that APPEARS between the two counts scan() takes — a download landing,
// a drag-and-drop finishing. The first count does not include it and the second
// does, which is the case the two-pass scan exists for and the only arrangement
// where one pass and two give different answers: a file that goes AWAY mid-scan
// is missed by both, and a stale high count is caught by the entry read.
static const char *g_appears;
static const char *g_appears_body;

static void fs_reset(void) { g_files_n = 0; g_appears = nullptr; }

static void fs_add(const char *name, const char *body, int is_dir = 0) {
    g_files[g_files_n].name   = name;
    g_files[g_files_n].is_dir = is_dir;
    g_files[g_files_n].body   = body;
    g_files_n++;
}

static const FakeFile *fs_find(const char *path) {
    for (int i = 0; i < g_files_n; i++) {
        char full[128];
        snprintf(full, sizeof(full), "%s/%s", g_dir, g_files[i].name);
        if (!strcmp(full, path)) return &g_files[i];
    }
    return nullptr;
}

extern "C" {

int fw_dir_count(const char *path) {
    if (strcmp(path, g_dir)) return 0;
    const int now = g_files_n;
    // Answered, THEN the new file lands. So the count the scan is working from
    // is already one behind by the time it starts reading entries.
    if (g_appears) { fs_add(g_appears, g_appears_body); g_appears = nullptr; }
    return now;
}

int fw_dir_entry(const char *path, unsigned index, FwDirEntry *out) {
    if (strcmp(path, g_dir) || (int)index >= g_files_n) return 0;
    snprintf(out->name, FW_NAME_MAX, "%s", g_files[index].name);
    out->is_dir = g_files[index].is_dir;
    out->size   = g_files[index].body ? (uint32_t)strlen(g_files[index].body) : 0;
    return 1;
}

uint32_t fw_file_read(const char *path, void *buf, uint32_t cap) {
    const FakeFile *f = fs_find(path);
    if (!f || !f->body) return 0;
    uint32_t n = (uint32_t)strlen(f->body);
    if (n > cap) n = cap;
    memcpy(buf, f->body, n);
    return n;
}

uint32_t fw_file_size(const char *path) {
    const FakeFile *f = fs_find(path);
    return (f && f->body) ? (uint32_t)strlen(f->body) : 0;
}

// novacore's leaf needs these and nothing else touches them here.
static char g_reg[8][2][128];
static int  g_regn;
int fw_reg_get(const char *k, char *out, uint32_t cap) {
    for (int i = 0; i < g_regn; i++)
        if (!strcmp(g_reg[i][0], k)) { snprintf(out, cap, "%s", g_reg[i][1]); return 1; }
    if (cap) out[0] = 0;
    return 0;
}
int fw_reg_set(const char *k, const char *v) {
    if (g_regn >= 8) return 0;
    snprintf(g_reg[g_regn][0], 128, "%s", k);
    snprintf(g_reg[g_regn][1], 128, "%s", v);
    g_regn++;
    return 1;
}
int32_t fw_reg_get_int(const char *k, int32_t d) {
    char b[128];
    if (!fw_reg_get(k, b, sizeof(b)) || !b[0]) return d;
    return (int32_t)atoi(b);
}
int  fw_reg_has(const char *k) { char b[4]; return fw_reg_get(k, b, sizeof(b)); }
void fw_reg_save(void) {}
int  fw_file_exists(const char *) { return 1; }
int  fw_mkdir(const char *) { return 1; }
int  fw_time_get(struct FwTime *out) { (void)out; return 0; }

// The module table comes along for category_name() alone, and it probes for
// hardware the moment it is asked anything. Everything it needs to do that is
// stubbed rather than the table being copied: a second copy of the category
// names is a second thing to keep in step, and this test would still pass after
// the real one changed.
int fw_i2c_init(unsigned, unsigned, unsigned, unsigned) { return 0; }
int fw_i2c_write(unsigned, unsigned, const void *, unsigned, int) { return -1; }
int fw_i2c_read(unsigned, unsigned, void *, unsigned, int) { return -1; }
int fw_i2c_deinit(unsigned) { return 0; }
int fw_gpio_usable(unsigned) { return 0; }
unsigned fw_gpio_count(void) { return 30; }
int fw_board(char *out, unsigned cap) { snprintf(out, cap, "pico2_w"); return 1; }

}  // extern "C"

// The real thing.
#include "../apps/novad1/novacore.cpp"
#include "../apps/novad1/novaboard.cpp"
#include "../apps/novad1/novamodtab.cpp"
#include "../apps/novad1/novaapps.cpp"

using namespace nova;
using namespace nova::napps;

static int checks, failures;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { failures++; printf("    FAIL %s\n", what); }
}
static void eq(int got, int want, const char *what) {
    checks++;
    if (got != want) { failures++; printf("    FAIL %s: got %d want %d\n", what, got, want); }
}
static void streq(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got ? got : "(null)", want)) {
        failures++;
        printf("    FAIL %s: got '%s' want '%s'\n", what, got ? got : "(null)", want);
    }
}

// --- the header ----------------------------------------------------------------------

static void test_header(void) {
    NappItem it;

    parse_header(
        "# a comment\n"
        "app.name: Sub-GHz Kit\n"
        "app.ver: 1.2.0\n"
        "app.category: Wireless\n"
        "app.desc: whatever\n"
        "\n"
        "Row = cc1101 status\n", &it);
    streq(it.label, "Sub-GHz Kit", "the name is read");
    streq(it.ver, "1.2.0", "the version is read");
    eq(it.cat, CAT_WIRELESS, "the category is read");
    eq(it.fault, NAPP_OK, "a complete header is fine");

    // Whatever whitespace and line endings a desktop editor produced.
    parse_header("app.name   :   Spaced Out   \r\n\tapp.ver:\t9.9\r\n", &it);
    streq(it.label, "Spaced Out", "the name is trimmed on both sides");
    streq(it.ver, "9.9", "a tab-indented key is still a key");

    // The reason the app. test comes before the '=' split: a description is
    // prose and prose has punctuation in it.
    parse_header("app.name: Odd\napp.desc: a = b, and c: d\n", &it);
    streq(it.label, "Odd", "a description containing = and : does not confuse the header");

    parse_header("app.ver: 1.0.0\nRow = ps\n", &it);
    eq(it.fault, NAPP_NO_NAME, "no app.name is a fault");
    streq(it.label, "", "and there is no name to show");

    parse_header("app.name: Dice\napp.kind: py\n", &it);
    eq(it.fault, NAPP_KIND, "kind: py is refused");
    parse_header("app.name: Fine\napp.kind: rows\n", &it);
    eq(it.fault, NAPP_OK, "kind: rows is the kind this reads");
    parse_header("app.name: Fine\napp.kind: ROWS\n", &it);
    eq(it.fault, NAPP_OK, "and the kind is not case-sensitive");
    parse_header("app.name: Fine\n", &it);
    eq(it.fault, NAPP_OK, "an absent kind means rows");

    // An unnamed app with a bad kind reports the KIND, because that is the line
    // the author has to change; the missing name is the smaller of two problems.
    parse_header("app.kind: py\n", &it);
    eq(it.fault, NAPP_KIND, "the kind outranks the missing name");

    parse_header("app.name: Mystery\napp.category: Interpretive Dance\n", &it);
    eq(it.cat, CAT_TOOLS, "an unknown category lands in Tools");
    parse_header("app.name: Mystery\napp.category: sYsTeM\n", &it);
    eq(it.cat, CAT_SYSTEM, "a category is matched case-insensitively");

    parse_header("", &it);
    eq(it.fault, NAPP_NO_NAME, "an empty file has no name");
    parse_header(nullptr, &it);
    eq(it.fault, NAPP_NO_NAME, "and neither has no file at all");

    // A name longer than the field. It has to truncate rather than run over —
    // the label is fourteen characters because that is what fits under an icon.
    parse_header("app.name: An Extremely Long Application Name\n", &it);
    ok(strlen(it.label) < sizeof(it.label), "a long name is truncated, not overrun");
    eq(it.fault, NAPP_OK, "and it is still a named app");

    eq(category_from("Testing"), CAT_TESTING, "Testing maps");
    eq(category_from(""), CAT_TOOLS, "an empty category is Tools");
}

// --- the rows -------------------------------------------------------------------------

static char g_scratch[NAPP_TEXT_MAX];
static NappRow g_rows[NAPP_ROWS_MAX];

static int rows_of(const char *text) {
    snprintf(g_scratch, sizeof(g_scratch), "%s", text);
    return parse_rows(g_scratch, g_rows, NAPP_ROWS_MAX);
}

static void test_rows(void) {
    int n = rows_of(
        "# a comment\n"
        "app.name: Kit\n"
        "\n"
        "  Status   =   cc1101 status  \n"
        "Listen = cc1101 rx 433.92\r\n"
        "not a row\n"
        " = nothing to press\n"
        "No action =   \n"
        "Script = script /nova/scripts/x.rps");
    eq(n, 3, "three rows out of that, and only three");
    streq(g_rows[0].label,  "Status",        "the label is trimmed");
    streq(g_rows[0].action, "cc1101 status", "and so is the command");
    streq(g_rows[1].action, "cc1101 rx 433.92", "a CRLF line ending does not reach the command");
    streq(g_rows[2].label,  "Script",        "the last line needs no newline");

    // A label with an '=' in it. First one wins, and that has to be said
    // somewhere because it is the one grammar decision an author can trip on.
    n = rows_of("a = b = c\n");
    eq(n, 1, "one row");
    streq(g_rows[0].label,  "a",     "the label stops at the first =");
    streq(g_rows[0].action, "b = c", "and the rest is all command");

    n = rows_of("app.desc: rows = fun\napp.name: X\n");
    eq(n, 0, "a header line is never a row, whatever is in it");

    // More rows than the table holds. The cap is honoured and nothing is
    // written past the end — the canary below is what actually proves it.
    char many[NAPP_TEXT_MAX];
    unsigned o = 0;
    for (int i = 0; i < NAPP_ROWS_MAX + 6; i++)
        o += (unsigned)snprintf(many + o, sizeof(many) - o, "Row%d = ps\n", i);
    NappRow guard[NAPP_ROWS_MAX + 1];
    guard[NAPP_ROWS_MAX].label = (const char *)0x5A5A5A5A;
    snprintf(g_scratch, sizeof(g_scratch), "%s", many);
    eq(parse_rows(g_scratch, guard, NAPP_ROWS_MAX), NAPP_ROWS_MAX, "the row cap is honoured");
    ok(guard[NAPP_ROWS_MAX].label == (const char *)0x5A5A5A5A,
       "and nothing is written past the last slot");

    eq(parse_rows(nullptr, g_rows, NAPP_ROWS_MAX), 0, "no text, no rows");
    eq(rows_of(""), 0, "an empty file has no rows");
    eq(rows_of("# nothing but comments\n\n\n"), 0, "and neither has a file of comments");
}

// --- discovery -------------------------------------------------------------------------

static const char *kGood =
    "app.name: Device Check\n"
    "app.ver: 1.0.0\n"
    "app.category: System\n"
    "Tasks = ps\n"
    "Storage = df\n";

static void test_scan(void) {
    fs_reset();
    fs_add("devcheck.napp", kGood);
    fs_add("notes.txt",     "not an app at all");
    fs_add("subdir",        nullptr, /*is_dir*/1);
    fs_add("nodot",         "also not an app");
    fs_add(".napp",         "an extension and no name");

    eq(scan(), 1, "only the .napp file is an app");
    const NappItem *it = at(0);
    ok(it != nullptr, "and it is there");
    if (it) {
        streq(it->key,   "app_devcheck",  "the key is the stem behind the prefix");
        streq(it->label, "Device Check",  "the label is app.name");
        streq(it->file,  "devcheck.napp", "the file name is kept for the read");
        streq(it->ver,   "1.0.0",         "the version came off the header");
        eq(it->cat, CAT_SYSTEM, "and so did the category");
        eq(it->fault, NAPP_OK, "nothing wrong with it");
    }
    ok(by_key("app_devcheck") == it, "and it is findable by key");
    ok(by_key("devcheck") == nullptr, "the bare stem is not the key");
    ok(by_key(nullptr) == nullptr, "and asking for nothing finds nothing");

    // An extension a desktop capitalised. The filesystem is case-sensitive, so
    // this is a real file somebody really produced.
    fs_reset();
    fs_add("Shouty.NAPP", kGood);
    eq(scan(), 1, "a capitalised extension is still an app");
    if (at(0)) streq(at(0)->key, "app_Shouty", "and the stem keeps its own case");

    // A file the listing knows about and the read cannot get at.
    fs_reset();
    fs_add("ghost.napp", nullptr);
    eq(scan(), 1, "a file that will not read is still listed");
    if (at(0)) {
        eq(at(0)->fault, NAPP_UNREADABLE, "with the reason on it");
        streq(at(0)->label, "ghost", "and its file name to show, since there is no app.name");
    }

    // No app.name: listed, faulted, and labelled with the file name rather than
    // the prefixed key, which is bookkeeping and not what it is called.
    fs_reset();
    fs_add("anon.napp", "Row = ps\n");
    eq(scan(), 1, "an unnamed app is listed");
    if (at(0)) {
        eq(at(0)->fault, NAPP_NO_NAME, "as a fault");
        streq(at(0)->label, "anon", "labelled with the file name");
    }

    fs_reset();
    eq(scan(), 0, "an empty directory is no apps");
    ok(at(0) == nullptr, "and there is nothing to index");

    // More apps than the table holds.
    fs_reset();
    static char names[NAPP_MAX + 4][24];
    for (int i = 0; i < NAPP_MAX + 4; i++) {
        snprintf(names[i], sizeof(names[i]), "app%d.napp", i);
        fs_add(names[i], kGood);
    }
    eq(scan(), NAPP_MAX, "the app cap is honoured");
    ok(at(NAPP_MAX) == nullptr, "and there is no entry past it");

    // A file landing WHILE the scan is running. One pass takes the stale count
    // and reports two apps out of three; the second pass sees all of them.
    fs_reset();
    fs_add("one.napp", kGood);
    fs_add("two.napp", kGood);
    g_appears      = "three.napp";
    g_appears_body = kGood;
    eq(scan(), 3, "a listing that grew underneath is read again");
}

static void test_dirty(void) {
    fs_reset();
    fs_add("one.napp", kGood);
    scan();
    eq(count(), 1, "one app");

    // The flag starts SET, so the first ask always looks at the disk — on a
    // device where the runner has never started, nothing has scanned, and "no
    // apps" is not the same answer as "not looked yet". It is still set here
    // because nothing in this test file has consumed it.
    ok(rescan_if_dirty(), "the first ask always looks");

    fs_add("two.napp", kGood);
    ok(!rescan_if_dirty(), "nothing rescans until something says so");
    eq(count(), 1, "so the second app is not seen yet");

    mark_dirty();
    ok(rescan_if_dirty(), "and it rescans when it is told to");
    eq(count(), 2, "picking up the new one");
    ok(!rescan_if_dirty(), "the flag is cleared by the rescan");
}

// --- loading one ---------------------------------------------------------------------

static void test_load(void) {
    fs_reset();
    fs_add("devcheck.napp", kGood);
    scan();

    NappFault why = NAPP_UNREADABLE;
    eq(load(*at(0), &why), 2, "two rows come back");
    eq(why, NAPP_OK, "with nothing wrong");
    eq(count_rows(), 2, "and the count agrees");
    streq(row(0)->label,  "Tasks", "the first row");
    streq(row(1)->action, "df",    "and the second's command");
    ok(row(2) == nullptr, "there is no third");
    ok(row(-1) == nullptr, "and no minus-first");

    fs_reset();
    fs_add("empty.napp", "app.name: Nothing To Do\n");
    scan();
    eq(load(*at(0), &why), 0, "an app with no rows will not open");
    eq(why, NAPP_NO_ROWS, "and says so");

    fs_reset();
    fs_add("gone.napp", nullptr);
    scan();
    eq(load(*at(0), &why), 0, "a file that will not read will not open");
    eq(why, NAPP_UNREADABLE, "and says so");

    // Too large to read in one piece. Asked by SIZE before the read, so this is
    // a refusal rather than a silent truncation that loses the author's last
    // rows with no sign anything was missing.
    static char huge[NAPP_TEXT_MAX + 64];
    unsigned o = (unsigned)snprintf(huge, sizeof(huge), "app.name: Huge\n");
    while (o < sizeof(huge) - 12) o += (unsigned)snprintf(huge + o, sizeof(huge) - o, "R = ps\n");
    fs_reset();
    fs_add("huge.napp", huge);
    scan();
    eq(load(*at(0), &why), 0, "a file past the buffer is refused");
    eq(why, NAPP_TOO_BIG, "as too big, not as no rows");

    // app.kind past the scan's 256-byte window. The scan cannot see it and says
    // the app is fine; the load re-reads the header from the WHOLE file, which
    // is the only reason a kind:py app with a long description is caught.
    static char late[NAPP_TEXT_MAX];
    o = (unsigned)snprintf(late, sizeof(late), "app.name: Late\napp.desc: ");
    while (o < NAPP_HEAD_MAX + 32) o += (unsigned)snprintf(late + o, sizeof(late) - o, "padding ");
    snprintf(late + o, sizeof(late) - o, "\napp.kind: py\nRow = ps\n");
    fs_reset();
    fs_add("late.napp", late);
    scan();
    eq(at(0)->fault, NAPP_OK, "the scan's short look sees nothing wrong");
    eq(load(*at(0), &why), 0, "the load reads the whole header and refuses it");
    eq(why, NAPP_KIND, "for its kind");

    // A row-bearing app with no name still opens. The name is worth saying and
    // is not worth refusing over — the catalogue already showed the file name.
    fs_reset();
    fs_add("anon.napp", "Do it = ps\n");
    scan();
    eq(load(*at(0), &why), 1, "an unnamed app with a row opens");
    eq(why, NAPP_NO_NAME, "and the missing name is reported alongside");
}

// --- where a download may land ----------------------------------------------------------
//
// The only thing between a URL and a write to the filesystem, so every way it
// can be talked into the wrong path is here.

static void test_url_filename(void) {
    char out[40];

    ok(url_filename("https://example.com/apps/subghz.napp", out, sizeof(out)),
       "an ordinary URL gives a name");
    streq(out, "subghz.napp", "and it is the last segment");

    ok(url_filename("https://x.dev/a.napp?raw=1&v=2", out, sizeof(out)), "a query string is fine");
    streq(out, "a.napp", "and is not part of the name");
    ok(url_filename("https://x.dev/a.napp#top", out, sizeof(out)), "and so is a fragment");
    streq(out, "a.napp", "also not part of the name");

    ok(url_filename("subghz.napp", out, sizeof(out)), "a bare name is a name");
    ok(url_filename("https://x.dev/My-App_2.napp", out, sizeof(out)),
       "dashes, underscores and digits are allowed");

    // A URL full of dot-dot is not refused, and it does not need to be: only the
    // LAST segment is ever used and it is a plain name. Asserted this way round
    // because the first version of this test asserted the refusal, which was a
    // claim about the wrong thing — what matters is what comes OUT.
    ok(url_filename("https://x.dev/../../etc/passwd.napp", out, sizeof(out)),
       "dot-dot segments earlier in a URL are simply not part of the name");
    streq(out, "passwd.napp", "only the last segment is used");
    ok(!strchr(out, '/') && out[0] != '.',
       "so what comes out is always a plain name in the apps folder");

    // Everything that has to be refused rather than repaired.
    ok(!url_filename("https://x.dev/.napp", out, sizeof(out)),
       "and so is a name that is only an extension");
    ok(!url_filename("https://x.dev/.hidden.napp", out, sizeof(out)),
       "a leading dot is refused whatever follows it");
    ok(!url_filename("https://x.dev/apps/", out, sizeof(out)),
       "a trailing slash leaves no name");
    ok(!url_filename("https://x.dev/a b.napp", out, sizeof(out)),
       "a space is refused");
    ok(!url_filename("https://x.dev/a;rm.napp", out, sizeof(out)),
       "and so is anything else outside the allowed set");
    ok(!url_filename("https://x.dev/notes.txt", out, sizeof(out)),
       "a file that is not an app is refused rather than renamed");
    ok(!url_filename("https://x.dev/plain", out, sizeof(out)),
       "and so is one with no extension at all");
    ok(!url_filename("", out, sizeof(out)), "an empty URL gives nothing");
    ok(!url_filename(nullptr, out, sizeof(out)), "and neither does no URL");

    // The extension is matched the way the scan matches it.
    ok(url_filename("https://x.dev/Shouty.NAPP", out, sizeof(out)),
       "a capitalised extension is still an app");

    // Exactly at the field, and one over. The buffer is what an install writes
    // a path out of, so a name that did not fit must be refused rather than cut.
    char url[128];
    for (unsigned len = 30; len <= 41; len++) {
        char stem[64];
        for (unsigned i = 0; i < len - 5; i++) stem[i] = 'a';
        stem[len - 5] = 0;
        snprintf(url, sizeof(url), "https://x.dev/%s.napp", stem);
        const bool got = url_filename(url, out, sizeof(out));
        const bool want = len < sizeof(out);
        checks++;
        if (got != want) {
            failures++;
            printf("    FAIL a %u-character name: got %d want %d\n", len, got, want);
        }
    }
}

// --- the shipped example ----------------------------------------------------------------
//
// The file in examples/ is what an author is told to copy, so it is read off
// the real host filesystem and put through the real parser. A sample that does
// not parse is worse than no sample.

static void test_example(void) {
    const char *path = "../apps/novad1/examples/devcheck.napp";
    FILE *f = fopen(path, "rb");
    ok(f != nullptr, "the shipped example exists");
    if (!f) return;

    static char text[4096];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;

    // With ROOM LEFT, and a specific amount of it: the sample is what an author
    // copies and then adds rows to, so fitting exactly is not the same as being
    // a usable starting point. This failed the first time it was written — the
    // sample was 1751 bytes against a 768-byte buffer — and the buffer is the
    // size it is because of this line.
    ok(n < NAPP_TEXT_MAX, "the shipped example fits in the buffer the device reads it with");
    const size_t room = NAPP_ROWS_MAX * 48;      // every row it could hold, generously
    ok(n + room < NAPP_TEXT_MAX, "with room for a full complement of rows on top of it");

    // Its header is inside the window the SCAN reads, so the home screen
    // labels the icon with the app's name rather than with the file name.
    NappItem head_only;
    static char window[NAPP_HEAD_MAX];
    snprintf(window, sizeof(window), "%.*s", (int)sizeof(window) - 1, text);
    parse_header(window, &head_only);
    streq(head_only.label, "Device Check", "and its header is inside the scan's window");

    NappItem it;
    parse_header(text, &it);
    eq(it.fault, NAPP_OK, "the example's header is complete");
    streq(it.label, "Device Check", "and names itself");
    eq(it.cat, CAT_SYSTEM, "in the folder it asked for");

    NappRow rows[NAPP_ROWS_MAX];
    const int r = parse_rows(text, rows, NAPP_ROWS_MAX);
    eq(r, 4, "the example has its four rows");
    streq(rows[0].label, "Tasks", "starting with the first one");

    // Every command in the sample must be a FIRMWARE command. A sample row
    // naming a package would be refused on the device, which is exactly the
    // trap the sample's own comments warn about.
    static const char *packaged[] = { "gpio", "i2cscan", "dht", "ws2812",
                                      "calc", "probe", "bench", "stress" };
    bool clean = true;
    for (int i = 0; i < r; i++)
        for (unsigned p = 0; p < sizeof(packaged) / sizeof(packaged[0]); p++) {
            const size_t len = strlen(packaged[p]);
            if (!strncmp(rows[i].action, packaged[p], len) &&
                (rows[i].action[len] == 0 || rows[i].action[len] == ' ')) clean = false;
        }
    ok(clean, "and not one of them calls a packaged command");
}

int main(void) {
    test_header();
    test_rows();
    test_scan();
    test_dirty();
    test_load();
    test_url_filename();
    test_example();
    printf("  %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
