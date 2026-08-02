#include "textcore.h"

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void text_count(const char *buf, uint32_t len,
                uint32_t *lines, uint32_t *words, uint32_t *bytes) {
    uint32_t l = 0, w = 0;
    bool in_word = false;
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] == '\n') l++;
        if (is_space(buf[i])) in_word = false;
        else if (!in_word) { in_word = true; w++; }
    }
    if (lines) *lines = l;
    if (words) *words = w;
    if (bytes) *bytes = len;
}

uint32_t text_line_count(const char *buf, uint32_t len) {
    if (len == 0) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < len; i++) if (buf[i] == '\n') n++;
    // A final line with no trailing newline still counts as a line.
    if (len > 0 && buf[len - 1] != '\n') n++;
    return n;
}

void text_for_lines(char *buf, uint32_t len, LineFn cb, void *ctx) {
    uint32_t start = 0, n = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            if (i == len && start == i) break;      // no trailing empty line
            char saved = (i < len) ? buf[i] : 0;
            if (i < len) buf[i] = 0;
            cb(ctx, buf + start, ++n);
            if (i < len) buf[i] = saved;
            start = i + 1;
        }
    }
}
