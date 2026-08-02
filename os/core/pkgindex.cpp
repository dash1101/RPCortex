#include "pkgindex.h"
#include <string.h>
#include <stdio.h>

// Does a line at `p` start with "name," ? Used for exact-name matching so a
// package "d1" is never confused with "d1x".
static bool line_is(const char *p, const char *name) {
    size_t nl = strlen(name);
    return strncmp(p, name, nl) == 0 && p[nl] == ',';
}

bool pkgindex_has(const char *buf, uint32_t len, const char *name) {
    const char *p = buf;
    const char *end = buf + len;
    while (p < end && *p) {
        if (line_is(p, name)) return true;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

uint32_t pkgindex_add(char *buf, uint32_t len, uint32_t cap,
                      const char *name, const char *version) {
    if (pkgindex_has(buf, len, name)) return len;      // keep one line per package
    int add = snprintf(buf + len, cap - len, "%s,%s\n", name, version);
    if (add < 0 || (uint32_t)add >= cap - len) return len;   // no room; unchanged
    return len + (uint32_t)add;
}

uint32_t pkgindex_remove(char *buf, uint32_t len, uint32_t cap, const char *name) {
    (void)cap;
    uint32_t w = 0;
    const char *p = buf;
    const char *end = buf + len;
    while (p < end && *p) {
        const char *nl = strchr(p, '\n');
        uint32_t linelen = nl ? (uint32_t)(nl - p + 1) : (uint32_t)strlen(p);
        if (!line_is(p, name)) {
            if (w != (uint32_t)(p - buf)) memmove(buf + w, p, linelen);
            w += linelen;
        }
        if (!nl) break;
        p = nl + 1;
    }
    buf[w < cap ? w : cap - 1] = 0;
    return w;
}

void pkgindex_walk(char *buf, uint32_t len, PkgIndexFn cb, void *ctx) {
    char *p = buf;
    char *end = buf + len;
    while (p < end && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        if (p[0] && p[0] != '#') {
            char *comma = strchr(p, ',');
            const char *ver = "?";
            if (comma) { *comma = 0; ver = comma + 1; }
            cb(ctx, p, ver);
            if (comma) *comma = ',';     // restore, so the buffer is reusable
        }
        if (!nl) break;
        *nl = '\n';
        p = nl + 1;
    }
}
