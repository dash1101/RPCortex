#include "cmdline.h"

#include <string.h>

// One rule for every scanner in this file, so the splitter and the operator
// scans cannot disagree about where a quote starts and ends.
//
// A quote OPENS only where a word begins: at the start of the text, after a
// space, or straight after an operator. In the middle of a word it is plain
// text, which is what keeps `wifi connect Dad's WiFi` and `echo don't` working
// the way they did when every command took its arguments raw. A quote closes
// only on its own kind, so "it's" and '{"k":1}' keep the other kind as text.
static bool word_start(const char *s, const char *p) {
    if (p == s) return true;
    char c = p[-1];
    return c == ' ' || c == ';' || c == '&' || c == '|' || c == '>';
}

// Advance the quote state over the character at `p`. `*q` is the quote character
// we are inside, or 0. Returns true when this character opened or closed a
// quote (it is then neither text nor an operator).
static bool quote_edge(const char *s, const char *p, char *q) {
    if (*q) {
        if (*p != *q) return false;
        *q = 0;
        return true;
    }
    if ((*p == '"' || *p == '\'') && word_start(s, p)) { *q = *p; return true; }
    return false;
}

int cmdline_split_args(char *line, char **argv, int max) {
    int argc = 0;
    char *r = line;
    while (*r && argc < max) {
        while (*r == ' ') r++;
        if (!*r) break;
        // Quotes are removed as the token is copied down over itself, so the
        // write pointer never passes the read pointer. A word that opens with a
        // quote runs to the matching one and then carries on to the next space
        // ('a b'c is one word), and '' is an empty argument, not none.
        char *w = r;
        argv[argc++] = w;
        char q = (*r == '"' || *r == '\'') ? *r++ : 0;
        while (*r && (q || *r != ' ')) {
            if (q && *r == q) { q = 0; r++; continue; }
            *w++ = *r++;
        }
        char *next = *r ? r + 1 : r;
        *w = 0;
        r = next;
    }
    return argc;
}

char *cmdline_next_connector(char *s, Connector *kind, int *skip) {
    char q = 0;
    for (char *p = s; *p; p++) {
        if (quote_edge(s, p, &q) || q) continue;
        if (p[0] == '&' && p[1] == '&') { *kind = CON_AND; *skip = 2; return p; }
        if (p[0] == '|' && p[1] == '|') { *kind = CON_OR;  *skip = 2; return p; }
        if (p[0] == ';')                { *kind = CON_SEQ; *skip = 1; return p; }
    }
    return nullptr;
}

char *cmdline_next_pipe(char *s) {
    char q = 0;
    for (char *p = s; *p; p++) {
        if (quote_edge(s, p, &q) || q) continue;
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
    char q = 0;
    char *last = nullptr;
    int  len = 0;
    // The LAST unquoted '>' wins, so `echo a > b > c` writes c — which is what
    // every other shell does, and beats silently taking the first.
    for (char *p = seg; *p; p++) {
        if (quote_edge(seg, p, &q) || q || *p != '>') continue;
        last = p;
        len = (p[1] == '>') ? 2 : 1;
        if (len == 2) p++;
    }
    if (!last) return nullptr;
    *append = (len == 2);
    char *file = cmdline_trim(last + len);
    *last = 0;
    // `> 'out.txt'` names out.txt, as it does everywhere else. Only a pair that
    // wraps the whole name is taken off; anything odder is left for the caller
    // to refuse.
    size_t n = strlen(file);
    if (n >= 2 && (file[0] == '"' || file[0] == '\'') && file[n - 1] == file[0]) {
        file[n - 1] = 0;
        file++;
    }
    // "cmd >" with no filename is a typo, not a redirect to a file named "".
    return *file ? file : nullptr;
}
