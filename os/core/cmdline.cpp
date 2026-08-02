#include "cmdline.h"

#include <string.h>

int cmdline_split_args(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
    }
    return argc;
}

char *cmdline_next_connector(char *s, Connector *kind, int *skip) {
    bool in_q = false;
    for (char *p = s; *p; p++) {
        if (*p == '"') { in_q = !in_q; continue; }
        if (in_q) continue;
        if (p[0] == '&' && p[1] == '&') { *kind = CON_AND; *skip = 2; return p; }
        if (p[0] == '|' && p[1] == '|') { *kind = CON_OR;  *skip = 2; return p; }
        if (p[0] == ';')                { *kind = CON_SEQ; *skip = 1; return p; }
    }
    return nullptr;
}

char *cmdline_next_pipe(char *s) {
    bool in_q = false;
    for (char *p = s; *p; p++) {
        if (*p == '"') { in_q = !in_q; continue; }
        if (in_q) continue;
        if (p[0] == '|' && p[1] == '|') { p++; continue; }   // '||' is not a pipe
        if (p[0] == '|') return p;
    }
    return nullptr;
}

char *cmdline_trim(char *s) {
    while (*s == ' ') s++;
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = 0;
    return s;
}

char *cmdline_split_redirect(char *seg, bool *append) {
    bool in_q = false;
    char *last = nullptr;
    int  len = 0;
    // The LAST unquoted '>' wins, so `echo a > b > c` writes c — which is what
    // every other shell does, and beats silently taking the first.
    for (char *p = seg; *p; p++) {
        if (*p == '"') { in_q = !in_q; continue; }
        if (in_q || *p != '>') continue;
        last = p;
        len = (p[1] == '>') ? 2 : 1;
        if (len == 2) p++;
    }
    if (!last) return nullptr;
    *append = (len == 2);
    char *file = cmdline_trim(last + len);
    *last = 0;
    // "cmd >" with no filename is a typo, not a redirect to a file named "".
    return *file ? file : nullptr;
}
