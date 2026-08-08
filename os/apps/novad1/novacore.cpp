// Desc: Registry access, storage paths and the small helpers every module needs.
// File: novacore.cpp
#include "novacore.h"

#include <string.h>
#include <stdio.h>

namespace nova {

// --- settings ---------------------------------------------------------------

// A rotating set of buffers, so two reg() calls in one expression do not tread
// on each other.
//
//     printf("%s / %s", reg("A", ""), reg("B", ""))
//
// evaluates both before either is used, and with a single buffer both would be
// the second value. Four is enough for anything the suite actually writes, and
// the cost is 388 bytes of bss on a device that has them.
#define REG_RING 4
static char     g_ring[REG_RING][NOVA_VAL_MAX];
static unsigned g_ring_at;

const char *reg(const char *key, const char *def) {
    char *buf = g_ring[g_ring_at];
    g_ring_at = (g_ring_at + 1) % REG_RING;
    if (fw_reg_get(key, buf, NOVA_VAL_MAX)) return buf;
    return def ? def : "";
}

int reg_int(const char *key, int def) {
    return (int)fw_reg_get_int(key, (int32_t)def);
}

// "on/off", "true/false", "yes/no", "1/0" — all of them, because six years of a
// registry edited by hand and by three different screens produced all six, and
// a setting that reads as off because it says "true" is a bug nobody finds.
bool reg_bool(const char *key, bool def) {
    char v[NOVA_VAL_MAX];
    if (!fw_reg_get(key, v, sizeof(v)) || !v[0]) return def;
    if (ieq(v, "on") || ieq(v, "true") || ieq(v, "yes") || ieq(v, "1")) return true;
    if (ieq(v, "off") || ieq(v, "false") || ieq(v, "no") || ieq(v, "0")) return false;
    return def;
}

bool reg_is(const char *key, const char *value, bool def_match) {
    char v[NOVA_VAL_MAX];
    if (!fw_reg_get(key, v, sizeof(v)) || !v[0]) return def_match;
    return ieq(v, value);
}

void reg_set(const char *key, const char *value)  { fw_reg_set(key, value); }
void reg_set_bool(const char *key, bool value)    { fw_reg_set(key, value ? "on" : "off"); }

void reg_set_int(const char *key, int value) {
    char b[16];
    snprintf(b, sizeof(b), "%d", value);
    fw_reg_set(key, b);
}

void reg_save(void) { fw_reg_save(); }

// --- text -------------------------------------------------------------------

unsigned copy(char *dst, unsigned cap, const char *src) {
    if (!dst || !cap) return 0;
    if (!src) { dst[0] = 0; return 0; }
    unsigned n = 0;
    while (src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = 0;
    return n;
}

void ellipsize(char *dst, unsigned cap, const char *src, unsigned chars) {
    if (!dst || !cap) return;
    if (!src) { dst[0] = 0; return; }
    unsigned len = (unsigned)strlen(src);
    if (chars + 1 > cap) chars = cap - 1;
    if (len <= chars) { copy(dst, cap, src); return; }
    // Two dots rather than three: at six pixels a character, the third dot is a
    // character of the name that could have been shown instead.
    if (chars < 3) { copy(dst, chars + 1, src); return; }
    memcpy(dst, src, chars - 2);
    dst[chars - 2] = '.';
    dst[chars - 1] = '.';
    dst[chars]     = 0;
}

static inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool ieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) { if (lower(*a) != lower(*b)) return false; a++; b++; }
    return *a == *b;
}

unsigned split_csv(char *editable, char **out, unsigned max) {
    if (!editable || !out || !max) return 0;
    unsigned n = 0;
    char *p = editable;
    while (*p && n < max) {
        while (*p == ' ') p++;
        out[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    // Trailing spaces on a field make "a, b" and "a,b" different strings that
    // mean the same thing, which is how a favourite stops matching its app.
    for (unsigned i = 0; i < n; i++) {
        char *e = out[i] + strlen(out[i]);
        while (e > out[i] && e[-1] == ' ') *--e = 0;
    }
    return n;
}

bool csv_has(const char *csv, const char *needle) {
    if (!csv || !needle || !*needle) return false;
    unsigned nl = (unsigned)strlen(needle);
    const char *p = csv;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        unsigned len = (unsigned)(e - p);
        while (len && p[len - 1] == ' ') len--;
        if (len == nl && strncmp(p, needle, nl) == 0) return true;
        p = e;
    }
    return false;
}

// Step over leading spaces AND any ANSI escape sequence, up to `end`.
// CSI is ESC '[' then parameter bytes then one final byte in 0x40..0x7e;
// anything else after ESC is a two-byte sequence and ends there.
static const char *skip_blank(const char *p, const char *end) {
    while (p < end) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (*p == '\033') {
            p++;
            if (p < end && *p == '[') {
                p++;
                while (p < end && !(*p >= 0x40 && *p <= 0x7e)) p++;
            }
            if (p < end) p++;              // the final byte of the sequence
            continue;
        }
        break;
    }
    return p;
}

int listing_index_of(const char *listing, const char *name) {
    if (!listing || !name || !*name) return -1;
    unsigned nl = (unsigned)strlen(name);

    for (const char *p = listing; *p; ) {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);

        const char *q = skip_blank(p, end);
        if (q < end && *q >= '1' && *q <= '9') {
            int n = 0;
            while (q < end && *q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
            // strstr would run past the newline into the next row; bound it.
            for (const char *s = q; s + nl <= end; s++)
                if (strncmp(s, name, nl) == 0) return n;
        }

        if (!eol) break;
        p = eol + 1;
    }
    return -1;
}

bool csv_add(char *csv, unsigned cap, const char *field) {
    if (!csv || !field || !*field) return false;
    if (csv_has(csv, field)) return false;
    unsigned have = (unsigned)strlen(csv);
    unsigned need = (unsigned)strlen(field) + (have ? 1u : 0u);
    if (have + need + 1 > cap) return false;
    if (have) csv[have++] = ',';
    copy(csv + have, cap - have, field);
    return true;
}

bool csv_remove(char *csv, unsigned cap, const char *field) {
    if (!csv || !cap || !field || !*field || !csv_has(csv, field)) return false;
    // Rebuilt rather than spliced. Splicing has to get the separator right at
    // both ends and at both edges of the string, and that is four cases where
    // rebuilding has none.
    //
    // The work copy is sized from the CALLER's buffer, not from a constant. A
    // fixed size here is the same bug in a second place: too small silently
    // truncates the list, too large wastes stack on a device where the package
    // gets eleven kilobytes of it.
    char work[NOVA_VAL_MAX * 2];
    if (cap > sizeof(work)) return false;      // more than this never fits a registry value
    copy(work, sizeof(work), csv);
    char *fields[24];
    unsigned n = split_csv(work, fields, 24);
    csv[0] = 0;
    unsigned at = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!fields[i][0] || strcmp(fields[i], field) == 0) continue;
        if (at && at + 1 < cap) csv[at++] = ',';
        at += copy(csv + at, cap - at, fields[i]);
    }
    csv[at] = 0;
    return true;
}

// --- paths ------------------------------------------------------------------

void path_join(char *out, unsigned cap, const char *a, const char *b) {
    snprintf(out, cap, "%s/%s", a, b);
}

void path_join3(char *out, unsigned cap, const char *a, const char *b, const char *c) {
    snprintf(out, cap, "%s/%s/%s", a, b, c);
}

void paths_init(void) {
    // Each level in turn: littlefs will not create a parent, so /nova/codes/ir
    // in one call fails on a device that has never had /nova.
    static const char *dirs[] = {
        NOVA_ROOT, NOVA_CODES, NOVA_SCRIPTS, NOVA_LOGS,
        NOVA_CODES "/ir", NOVA_CODES "/subghz", NOVA_CODES "/nfc", NOVA_CODES "/lora",
    };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (!fw_file_exists(dirs[i])) fw_mkdir(dirs[i]);
}

// --- time -------------------------------------------------------------------

bool time_hhmm(char *out, unsigned cap) {
    FwTime t;
    if (!fw_time_get(&t)) { copy(out, cap, "--:--"); return false; }
    if (reg_bool("Apps.NovaD1_Clock24", true)) {
        snprintf(out, cap, "%02d:%02d", t.hour, t.minute);
    } else {
        int h = t.hour % 12;
        if (!h) h = 12;
        snprintf(out, cap, "%d:%02d%s", h, t.minute, t.hour < 12 ? "am" : "pm");
    }
    return true;
}

bool time_date(char *out, unsigned cap) {
    static const char *dow[]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *month[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    FwTime t;
    if (!fw_time_get(&t)) { copy(out, cap, "-- --- ----"); return false; }
    int wd = t.weekday >= 0 && t.weekday < 7 ? t.weekday : 0;
    int mo = t.month >= 1 && t.month <= 12 ? t.month - 1 : 0;
    snprintf(out, cap, "%s %d %s", dow[wd], t.day, month[mo]);
    return true;
}

}  // namespace nova
