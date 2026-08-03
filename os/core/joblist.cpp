#include "joblist.h"

#include <string.h>
#include <stdio.h>

// A line that carries no command: blank, or a comment. Kept in the file so a
// header survives editing, but never counted or indexed.
static bool is_skip(const char *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] == ' ' || p[i] == '\t' || p[i] == '\r') continue;
        return p[i] == '#';
    }
    return true;                 // all whitespace
}

// Walk raw lines, skip or not, with their span. The one place line boundaries
// are worked out, so every operation agrees on where a line starts and ends.
template <typename F>
static void for_lines(const char *buf, uint32_t len, F fn) {
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i != len && buf[i] != '\n') continue;
        uint32_t n = i - start;
        while (n && (buf[start + n - 1] == '\r')) n--;
        if (i != start || n) fn(start, i, n);
        start = i + 1;
        if (start > len) break;
    }
}

uint32_t joblist_count(const char *buf, uint32_t len) {
    uint32_t n = 0;
    for_lines(buf, len, [&](uint32_t s, uint32_t, uint32_t l) {
        if (!is_skip(buf + s, l)) n++;
    });
    return n;
}

void joblist_walk(const char *buf, uint32_t len, JobWalkFn cb, void *ctx) {
    uint32_t idx = 0;
    char line[160];
    for_lines(buf, len, [&](uint32_t s, uint32_t, uint32_t l) {
        if (is_skip(buf + s, l)) return;
        if (l >= sizeof(line)) l = sizeof(line) - 1;
        memcpy(line, buf + s, l);
        line[l] = 0;
        cb(ctx, ++idx, line);
    });
}

bool joblist_get(const char *buf, uint32_t len, uint32_t index, char *out, uint32_t cap) {
    struct Ctx { uint32_t want; char *out; uint32_t cap; bool found; } c{index, out, cap, false};
    joblist_walk(buf, len, [](void *v, uint32_t i, const char *line) {
        Ctx *c = (Ctx *)v;
        if (i == c->want && !c->found) { snprintf(c->out, c->cap, "%s", line); c->found = true; }
    }, &c);
    return c.found;
}

uint32_t joblist_add(char *buf, uint32_t len, uint32_t cap, const char *line) {
    if (!line || !line[0]) return len;
    uint32_t add = (uint32_t)strlen(line);
    // Room for a separating newline if the file does not end with one, the line
    // itself, its newline, and a terminator.
    bool need_nl = (len > 0 && buf[len - 1] != '\n');
    if (len + (need_nl ? 1u : 0u) + add + 2 > cap) return len;
    if (need_nl) buf[len++] = '\n';
    memcpy(buf + len, line, add);
    len += add;
    buf[len++] = '\n';
    buf[len] = 0;
    return len;
}

uint32_t joblist_remove(char *buf, uint32_t len, uint32_t index) {
    // Find the byte span of the index-th entry INCLUDING its newline, then close
    // the gap. Removing the newline too is what stops a deletion leaving a blank
    // line behind that shifts nothing but looks like it did.
    struct Ctx { uint32_t want, idx, from, to; bool found; } c{index, 0, 0, 0, false};
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len && !c.found; i++) {
        if (i != len && buf[i] != '\n') continue;
        uint32_t n = i - start;
        while (n && buf[start + n - 1] == '\r') n--;
        if (!is_skip(buf + start, n)) {
            if (++c.idx == c.want) {
                c.from  = start;
                c.to    = (i < len) ? i + 1 : i;   // take the newline with it
                c.found = true;
            }
        }
        start = i + 1;
        if (start > len) break;
    }
    if (!c.found) return len;
    memmove(buf + c.from, buf + c.to, len - c.to);
    len -= (c.to - c.from);
    buf[len] = 0;
    return len;
}

bool joblist_split_interval(const char *line, uint32_t *secs, const char **cmd) {
    if (!line) return false;
    while (*line == ' ' || *line == '\t') line++;
    if (*line < '0' || *line > '9') return false;
    uint32_t v = 0;
    while (*line >= '0' && *line <= '9') {
        v = v * 10 + (uint32_t)(*line - '0');
        if (v > 86400 * 30) return false;          // a month is not an interval
        line++;
    }
    if (*line != ' ' && *line != '\t') return false;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return false;
    if (secs) *secs = v;
    if (cmd)  *cmd  = line;
    return true;
}
