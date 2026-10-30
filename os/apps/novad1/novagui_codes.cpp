// Desc: Saved codes - see novagui_codes.h.
// File: novagui_codes.cpp
#include "novagui_codes.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {
namespace codes {

const Kind kSubGhz = { NOVA_CODES "/subghz", ".sub", "Sub-GHz" };
const Kind kIr     = { NOVA_CODES "/ir",     ".ir",  "IR"      };
const Kind kNfc    = { NOVA_CODES "/nfc",    ".nfc", "NFC"     };

void ensure_dir(const Kind &k) {
    // Each level in turn: littlefs does not create a parent.
    if (!fw_file_exists(NOVA_ROOT))  fw_mkdir(NOVA_ROOT);
    if (!fw_file_exists(NOVA_CODES)) fw_mkdir(NOVA_CODES);
    if (!fw_file_exists(k.dir))      fw_mkdir(k.dir);
}

bool clean_name(const char *typed, char *out, unsigned cap) {
    if (!cap) return false;
    unsigned n = 0;
    for (const char *p = typed ? typed : ""; *p && n + 1 < cap && n < CODES_NAME_MAX - 1; p++) {
        char c = *p;
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == ' ';
        out[n++] = keep ? c : '_';
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '_')) n--;
    unsigned s = 0;
    while (s < n && (out[s] == ' ' || out[s] == '_')) s++;
    if (s) memmove(out, out + s, n - s);
    n -= s;
    out[n] = 0;
    return n > 0;
}

void path_for(const Kind &k, const char *name, char *out, unsigned cap) {
    snprintf(out, cap, "%s/%s%s", k.dir, name, k.ext);
}

void unique_name(const Kind &k, const char *base, char *out, unsigned cap) {
    char path[96];
    nova::copy(out, cap, base);
    for (int i = 2; i < 100; i++) {
        path_for(k, out, path, sizeof(path));
        if (!fw_file_exists(path)) return;
        // Leave room for the suffix inside the name limit.
        char stem[CODES_NAME_MAX];
        nova::copy(stem, sizeof(stem), base);
        char suffix[8];
        snprintf(suffix, sizeof(suffix), "-%d", i);
        unsigned room = CODES_NAME_MAX - 1 - (unsigned)strlen(suffix);
        if (strlen(stem) > room) stem[room] = 0;
        snprintf(out, cap, "%s%s", stem, suffix);
    }
}

// --- the file sink --------------------------------------------------------------------------

namespace {
struct FileSink {
    char     path[96];
    char     buf[192];
    unsigned n;
    bool     failed;
    bool     open;
};
FileSink g_fs;

bool flush_fs(void) {
    if (!g_fs.n) return true;
    bool ok = fw_file_append(g_fs.path, g_fs.buf, g_fs.n) != 0;
    g_fs.n = 0;
    if (!ok) g_fs.failed = true;
    return ok;
}

bool fs_put(void *, const char *s, unsigned n) {
    if (!g_fs.open || g_fs.failed) return false;
    while (n) {
        unsigned room = (unsigned)sizeof(g_fs.buf) - g_fs.n;
        unsigned take = n < room ? n : room;
        memcpy(g_fs.buf + g_fs.n, s, take);
        g_fs.n += take; s += take; n -= take;
        if (g_fs.n == sizeof(g_fs.buf) && !flush_fs()) return false;
    }
    return true;
}
}  // namespace

codec::Sink file_sink_open(const char *path) {
    nova::copy(g_fs.path, sizeof(g_fs.path), path);
    g_fs.n = 0;
    g_fs.failed = false;
    g_fs.open = true;
    // Truncate: the first append would otherwise tack onto whatever was there.
    if (!fw_file_write(path, "", 0)) g_fs.failed = true;
    codec::Sink k = { fs_put, nullptr };
    return k;
}

bool file_sink_done(void) {
    if (!g_fs.open) return false;
    flush_fs();
    g_fs.open = false;
    if (g_fs.failed) { fw_file_remove(g_fs.path); return false; }
    return true;
}

// --- the list --------------------------------------------------------------------------------

static List g_list;

List &list(void) { return g_list; }

static bool has_ext(const char *name, const char *ext) {
    unsigned nl = (unsigned)strlen(name), el = (unsigned)strlen(ext);
    if (nl <= el) return false;
    for (unsigned i = 0; i < el; i++) {
        char a = name[nl - el + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return (unsigned char)x - (unsigned char)y;
        if (!x) return 0;
    }
}

void scan(const Kind &k) {
    char keep[CODES_NAME_MAX];
    keep[0] = 0;
    if (g_list.n > 0 && g_list.sel >= 0 && g_list.sel < g_list.n) nova::copy(keep, sizeof(keep), g_list.names[g_list.sel]);

    g_list.n = 0;
    g_list.more = false;
    int count = fw_dir_count(k.dir);
    for (int i = 0; i < count; i++) {
        FwDirEntry e;
        if (fw_dir_entry(k.dir, (unsigned)i, &e) != 1 || e.is_dir || !has_ext(e.name, k.ext)) continue;
        if (g_list.n >= CODES_MAX) { g_list.more = true; continue; }
        // Insertion in order, so the list needs no sort afterwards.
        char stem[CODES_NAME_MAX];
        unsigned len = (unsigned)strlen(e.name) - (unsigned)strlen(k.ext);
        if (len >= sizeof(stem)) len = sizeof(stem) - 1;
        memcpy(stem, e.name, len);
        stem[len] = 0;
        int at = g_list.n;
        while (at > 0 && ci_cmp(g_list.names[at - 1], stem) > 0) {
            memcpy(g_list.names[at], g_list.names[at - 1], CODES_NAME_MAX);
            at--;
        }
        nova::copy(g_list.names[at], CODES_NAME_MAX, stem);
        g_list.n++;
    }
    g_list.sel = 0;
    for (int i = 0; i < g_list.n && keep[0]; i++)
        if (!strcmp(g_list.names[i], keep)) { g_list.sel = i; break; }
    g_list.top = 0;
}

void move(int dir) {
    if (g_list.n <= 0) return;
    g_list.sel = (g_list.sel + dir + g_list.n) % g_list.n;
}

const char *selected(void) {
    return (g_list.n > 0 && g_list.sel >= 0 && g_list.sel < g_list.n) ? g_list.names[g_list.sel] : nullptr;
}

void draw(Canvas &c, const char *empty) {
    if (g_list.n <= 0) {
        ui::text_block(c, 2, ui::TOP, empty, 4);
        return;
    }
    const int rows = ui::rows_for(c);
    if (g_list.sel < g_list.top) g_list.top = g_list.sel;
    if (g_list.sel >= g_list.top + rows) g_list.top = g_list.sel - rows + 1;
    const int w = c.width() - ui::SB_W - 1;
    int y = ui::TOP;
    for (int i = g_list.top; i < g_list.n && i < g_list.top + rows; i++, y += ui::ROWH) {
        const bool on = i == g_list.sel;
        if (on) c.fill_rect(0, y - 1, w, ui::ROWH, 1);
        c.text_fit(2, y, g_list.names[i], on ? 0 : 1, w - 4, false);
    }
    if (g_list.n > rows) {
        // The scroll bar: a track, and a thumb sized and placed by what is showing.
        const int track = rows * ui::ROWH;
        int thumb = track * rows / g_list.n;
        if (thumb < 3) thumb = 3;
        int ty = ui::TOP - 1 + (track - thumb) * g_list.top / (g_list.n - rows);
        c.fill_rect(c.width() - ui::SB_W, ty, ui::SB_W, thumb, 1);
    }
}

// --- rename and delete -------------------------------------------------------------------------

namespace {
const Kind *g_act_kind;
char        g_act_name[CODES_NAME_MAX];
void      (*g_act_done)(void *);
void       *g_act_ctx;
}

static void rename_typed(void *, const char *text) {
    char name[CODES_NAME_MAX];
    if (!g_act_kind || !clean_name(text, name, sizeof(name))) { ui::notice("Rename", "That is not a name."); return; }
    if (!strcmp(name, g_act_name)) return;
    char from[96], to[96];
    path_for(*g_act_kind, g_act_name, from, sizeof(from));
    path_for(*g_act_kind, name, to, sizeof(to));
    if (fw_file_exists(to)) { ui::notice("Rename", "There is one with that name already."); return; }
    if (!fw_file_rename(from, to)) { ui::notice("Rename", "It could not be renamed."); return; }
    if (g_act_done) g_act_done(g_act_ctx);
}

void ask_rename(const Kind &k, const char *name, void (*done)(void *), void *ctx) {
    g_act_kind = &k;
    nova::copy(g_act_name, sizeof(g_act_name), name);
    g_act_done = done; g_act_ctx = ctx;
    ui::keyboard("Rename", name, false, rename_typed, nullptr, nullptr);
}

static void delete_yes(void *) {
    char path[96];
    if (!g_act_kind) return;
    path_for(*g_act_kind, g_act_name, path, sizeof(path));
    if (!fw_file_remove(path)) { ui::notice("Delete", "It could not be deleted."); return; }
    if (g_act_done) g_act_done(g_act_ctx);
}

void ask_delete(const Kind &k, const char *name, void (*done)(void *), void *ctx) {
    g_act_kind = &k;
    nova::copy(g_act_name, sizeof(g_act_name), name);
    g_act_done = done; g_act_ctx = ctx;
    static char q[48];
    snprintf(q, sizeof(q), "Delete %.28s?", name);
    ui::confirm(q, "Delete", delete_yes, nullptr);
}

}  // namespace codes
}  // namespace screens
}  // namespace nova
