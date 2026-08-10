// Desc: Third-party apps — the scan, the manifest parse, and the fault reasons.
// File: novaapps.cpp
#include "novaapps.h"

#include "novacore.h"

#include <string.h>
#include <stdio.h>

#include "rpc_app.h"

namespace nova {
namespace napps {

// --- small string work ----------------------------------------------------------
//
// Local rather than shared: novacore's helpers work on whole strings, and every
// one of these works on a SPAN, because a manifest is parsed in place and there
// is nowhere to put a copy.

static const char *napp_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// Copy [s, e) into out, with the trailing blanks and any \r taken off.
static void napp_take(char *out, unsigned cap, const char *s, const char *e) {
    s = napp_skip_ws(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    unsigned n = 0;
    while (s < e && n + 1 < cap) out[n++] = *s++;
    out[n] = 0;
}

// The end of the line starting at p — the newline, or the terminator.
static const char *napp_eol(const char *p) {
    while (*p && *p != '\n') p++;
    return p;
}

// A line that carries nothing: blank, or a comment.
static bool napp_ignorable(const char *s, const char *e) {
    s = napp_skip_ws(s);
    return s >= e || *s == '#' || *s == '\r';
}

// --- faults ------------------------------------------------------------------------

const char *fault_text(NappFault f) {
    switch (f) {
        case NAPP_NO_NAME:    return "No app.name line, so there is nothing to call it.";
        case NAPP_NO_ROWS:    return "No rows. A row is 'Label = command'.";
        // Named rather than generic, because kind: py is exactly what somebody
        // porting a MicroPython app will write, and "unsupported" would leave
        // them looking for a typo.
        case NAPP_KIND:       return "This build reads app.kind rows only. A kind:py app runs code, and packages cannot run each other's code here.";
        case NAPP_UNREADABLE: return "The file could not be read. It may have been deleted.";
        case NAPP_TOO_BIG:    return "The file is too large to read in one piece.";
        default:              return "";
    }
}

// --- the header ----------------------------------------------------------------------

Category category_from(const char *name) {
    for (int i = 0; i < CAT_COUNT; i++)
        if (nova::ieq(name, category_name((Category)i))) return (Category)i;
    return CAT_TOOLS;
}

void parse_header(const char *text, NappItem *out) {
    if (!out) return;
    out->label[0] = 0;
    out->ver[0]   = 0;
    out->cat      = CAT_TOOLS;
    out->fault    = NAPP_NO_NAME;

    if (!text) return;

    // kind is tracked separately: an unnamed app with a bad kind should report
    // the kind, since that is the thing the author has to change.
    bool bad_kind = false;

    for (const char *p = text; *p; ) {
        const char *e = napp_eol(p);
        const char *next = *e ? e + 1 : e;

        const char *s = napp_skip_ws(p);
        if (napp_ignorable(s, e) || strncmp(s, "app.", 4)) { p = next; continue; }

        const char *colon = s;
        while (colon < e && *colon != ':') colon++;
        if (colon >= e) { p = next; continue; }

        char key[16];
        napp_take(key, sizeof(key), s + 4, colon);
        char val[NAPP_HEAD_MAX];
        napp_take(val, sizeof(val), colon + 1, e);

        if      (nova::ieq(key, "name"))     { nova::copy(out->label, sizeof(out->label), val); }
        else if (nova::ieq(key, "ver"))      { nova::copy(out->ver,   sizeof(out->ver),   val); }
        else if (nova::ieq(key, "category")) { out->cat = category_from(val); }
        // Absent means "rows". A file written before the key existed is a
        // perfectly good app, and demanding the line would break every one of
        // them for no gain.
        else if (nova::ieq(key, "kind"))     { bad_kind = val[0] && !nova::ieq(val, "rows"); }

        p = next;
    }

    if (bad_kind)          out->fault = NAPP_KIND;
    else if (out->label[0]) out->fault = NAPP_OK;
}

// --- the rows -------------------------------------------------------------------------

int parse_rows(char *text, NappRow *out, int max) {
    if (!text || !out || max <= 0) return 0;
    int n = 0;

    for (char *p = text; *p && n < max; ) {
        char *e = (char *)napp_eol(p);
        char *next = *e ? e + 1 : e;
        *e = 0;                                  // the line stands alone from here

        char *s = (char *)napp_skip_ws(p);
        // A header line can hold an '=' inside app.desc, so the app. test comes
        // before the split rather than after it.
        if (napp_ignorable(s, e) || !strncmp(s, "app.", 4)) { p = next; continue; }

        char *eq = s;
        while (*eq && *eq != '=') eq++;
        if (!*eq) { p = next; continue; }        // a line with no action is not a row

        char *label_end = eq;
        *eq = 0;
        char *action = (char *)napp_skip_ws(eq + 1);
        while (label_end > s && (label_end[-1] == ' ' || label_end[-1] == '\t')) label_end--;
        *label_end = 0;
        // \r survives the split when the file has DOS line endings, and it draws
        // as a box glyph on the end of every command.
        char *ae = action + strlen(action);
        while (ae > action && (ae[-1] == '\r' || ae[-1] == ' ' || ae[-1] == '\t')) *--ae = 0;

        if (!s[0] || !action[0]) { p = next; continue; }

        out[n].label  = s;
        out[n].action = action;
        n++;
        p = next;
    }
    return n;
}

// --- discovery ---------------------------------------------------------------------------

static NappItem g_napp[NAPP_MAX];
static int      g_napp_n;
static bool     g_napp_dirty;

int count(void) { return g_napp_n; }

const NappItem *at(int i) {
    return (i >= 0 && i < g_napp_n) ? &g_napp[i] : nullptr;
}

const NappItem *by_key(const char *key) {
    if (!key) return nullptr;
    for (int i = 0; i < g_napp_n; i++)
        if (!strcmp(g_napp[i].key, key)) return &g_napp[i];
    return nullptr;
}

void mark_dirty(void) { g_napp_dirty = true; }

bool rescan_if_dirty(void) {
    if (!g_napp_dirty) return false;
    g_napp_dirty = false;
    scan();
    return true;
}

// Is this the name of an app file, and what is its catalogue key?
//
// Case-insensitively on the extension, because the device filesystem is
// case-sensitive and a "MyApp.NAPP" dropped from a desktop is a file somebody
// meant.
static bool napp_named(const char *name, char *stem, unsigned cap) {
    const unsigned n = (unsigned)strlen(name);
    const unsigned x = (unsigned)strlen(NOVA_APP_EXT);
    if (n <= x) return false;
    for (unsigned i = 0; i < x; i++) {
        char a = name[n - x + i], b = NOVA_APP_EXT[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    const unsigned pre = (unsigned)strlen(NAPP_KEY_PREFIX);
    if (!(n - x) || cap <= pre + 1) return false;
    memcpy(stem, NAPP_KEY_PREFIX, pre);
    unsigned keep = n - x;
    if (pre + keep + 1 > cap) keep = cap - pre - 1;
    memcpy(stem + pre, name, keep);
    stem[pre + keep] = 0;
    return true;
}

int scan(void) {
    // Twice, at most — the same bargain scripts_scan makes, and for the same
    // reason: the listing is read by index, and an entry added or removed
    // between two calls shifts every index after it.
    for (int attempt = 0; attempt < 2; attempt++) {
        g_napp_n = 0;
        const int n = fw_dir_count(NOVA_APPS_DIR);
        if (n <= 0) return 0;

        for (int i = 0; i < n && g_napp_n < NAPP_MAX; i++) {
            FwDirEntry de;
            if (fw_dir_entry(NOVA_APPS_DIR, (unsigned)i, &de) != 1) break;
            if (de.is_dir) continue;

            NappItem &it = g_napp[g_napp_n];
            if (!napp_named(de.name, it.key, sizeof(it.key))) continue;
            nova::copy(it.file, sizeof(it.file), de.name);

            char path[sizeof(it.file) + sizeof(NOVA_APPS_DIR) + 2];
            nova::path_join(path, sizeof(path), NOVA_APPS_DIR, it.file);

            // The HEAD only. Eight of these are read every time the home
            // rescans, and the rows are not wanted until somebody opens it.
            //
            // STATIC rather than on the stack. A package gets three kilobytes
            // and this is called from inside the runner's event loop; half a
            // kilobyte of frame is a sixth of the budget for a buffer that is
            // used for a few microseconds. Two tasks scanning at once would
            // garble a parse and nothing worse — both write the same shape —
            // and the only two callers reach it through rescan_if_dirty, which
            // clears the flag before it scans.
            static char head[NAPP_HEAD_MAX];
            const uint32_t got = fw_file_read(path, head, sizeof(head) - 1);
            head[got] = 0;
            if (!got) {
                it.label[0] = 0;
                it.ver[0]   = 0;
                it.cat      = CAT_TOOLS;
                it.fault    = NAPP_UNREADABLE;
            } else {
                parse_header(head, &it);
            }
            // A file with no app.name is still an app, and its FILE NAME is the
            // best name anybody has for it — the key would show the prefix,
            // which is bookkeeping and not what the file is called. The fault
            // stands, so the screen still says what to fix.
            if (!it.label[0]) {
                nova::copy(it.label, sizeof(it.label), it.key + strlen(NAPP_KEY_PREFIX));
            }
            g_napp_n++;
        }
        if (fw_dir_count(NOVA_APPS_DIR) == n) return g_napp_n;
    }
    return g_napp_n;
}

// --- loading one -----------------------------------------------------------------------

static char    g_napp_text[NAPP_TEXT_MAX];
static NappRow g_napp_rows[NAPP_ROWS_MAX];
static int     g_napp_rows_n;

int count_rows(void) { return g_napp_rows_n; }

const NappRow *row(int i) {
    return (i >= 0 && i < g_napp_rows_n) ? &g_napp_rows[i] : nullptr;
}

int load(const NappItem &it, NappFault *why) {
    g_napp_rows_n = 0;
    g_napp_text[0] = 0;
    NappFault f = NAPP_OK;

    char path[sizeof(it.file) + sizeof(NOVA_APPS_DIR) + 2];
    nova::path_join(path, sizeof(path), NOVA_APPS_DIR, it.file);

    // Asked BEFORE the read, so a file that grew past the buffer is reported as
    // too large rather than silently interpreted as far as it fitted — which
    // would drop the author's last rows with no sign that anything was missing.
    const uint32_t size = fw_file_size(path);
    if (size >= sizeof(g_napp_text)) {
        if (why) *why = NAPP_TOO_BIG;
        return 0;
    }

    const uint32_t got = fw_file_read(path, g_napp_text, sizeof(g_napp_text) - 1);
    g_napp_text[got] = 0;
    if (!got) {
        if (why) *why = NAPP_UNREADABLE;
        return 0;
    }

    // Re-read the header from the WHOLE file rather than trusting the scan's
    // 256-byte look: a long app.desc can push app.kind past it, and a kind this
    // build cannot interpret has to be caught before its rows are run.
    NappItem full = it;
    parse_header(g_napp_text, &full);
    if (full.fault == NAPP_KIND) {
        if (why) *why = NAPP_KIND;
        return 0;
    }
    if (full.fault != NAPP_OK) f = full.fault;

    g_napp_rows_n = parse_rows(g_napp_text, g_napp_rows, NAPP_ROWS_MAX);
    if (!g_napp_rows_n) {
        if (why) *why = NAPP_NO_ROWS;
        return 0;
    }
    // Rows are what the screen needs; a missing name is worth saying but not
    // worth refusing over, and the catalogue already showed the stem instead.
    if (why) *why = f;
    return g_napp_rows_n;
}

}  // namespace napps
}  // namespace nova
