#include "path.h"
#include <stdio.h>
#include <string.h>

void path_resolve(const char *cwd, const char *in, char *out, size_t cap) {
    char work[192];
    if (in[0] == '/') snprintf(work, sizeof(work), "%s", in);
    else if (strcmp(cwd, "/") == 0) snprintf(work, sizeof(work), "/%s", in);
    else snprintf(work, sizeof(work), "%s/%s", cwd, in);

    char parts[16][40];
    int n = 0;
    char *p = work;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;                     // "."  -> skip
        if (len == 2 && start[0] == '.' && start[1] == '.') { if (n) n--; continue; }  // ".." -> pop
        if (n < 16 && len < 40) { memcpy(parts[n], start, len); parts[n][len] = 0; n++; }
    }
    if (n == 0) { snprintf(out, cap, "/"); return; }
    size_t w = 0;
    for (int i = 0; i < n && w < cap; i++)
        w += (size_t)snprintf(out + w, cap - w, "/%s", parts[i]);
}
