#include "path.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool path_resolve(const char *cwd, const char *in, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!in) in = "";
    if (!cwd || !cwd[0]) cwd = "/";

    // The whole input, cwd included, joined. Too long to hold is too long to
    // be a path this filesystem can open, so it fails rather than being cut.
    char work[256];
    int wn;
    if (in[0] == '/')               wn = snprintf(work, sizeof(work), "%s", in);
    else if (!strcmp(cwd, "/"))     wn = snprintf(work, sizeof(work), "/%s", in);
    else                            wn = snprintf(work, sizeof(work), "%s/%s", cwd, in);
    if (wn < 0 || (size_t)wn >= sizeof(work)) return false;

    // Components as (start, length) into `work`, so no name is ever copied into
    // a fixed slot that might be too small for it.
    uint16_t at[64], len[64];
    int n = 0;
    char *p = work;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        size_t l = (size_t)(p - start);
        if (l == 1 && start[0] == '.') continue;                          // "."
        if (l == 2 && start[0] == '.' && start[1] == '.') { if (n) n--; continue; }  // ".."
        if (n == 64) return false;
        at[n] = (uint16_t)(start - work); len[n] = (uint16_t)l; n++;
    }

    if (n == 0) {
        if (cap < 2) return false;
        out[0] = '/'; out[1] = 0;
        return true;
    }
    size_t w = 0;
    for (int i = 0; i < n; i++) {
        if (w + 1 + len[i] + 1 > cap) { out[0] = 0; return false; }
        out[w++] = '/';
        memcpy(out + w, work + at[i], len[i]);
        w += len[i];
    }
    out[w] = 0;
    return true;
}
