// Pure text helpers for the grep/wc/head/tail commands, split out so the
// counting and line-splitting logic host-tests without a filesystem. The
// commands in shell/text.cpp read a file into a buffer and hand it here.
#ifndef RPC_TEXTCORE_H
#define RPC_TEXTCORE_H

#include <stdint.h>

// wc: lines (newline count), words (whitespace-delimited runs), bytes (= len).
void text_count(const char *buf, uint32_t len,
                uint32_t *lines, uint32_t *words, uint32_t *bytes);

// Visit each line in turn. The line is NUL-terminated for the callback (the
// newline is temporarily replaced and restored), so a callback can use string
// functions on it. `n` is the 1-based line number.
typedef void (*LineFn)(void *ctx, const char *line, uint32_t n);
void text_for_lines(char *buf, uint32_t len, LineFn cb, void *ctx);

// Number of lines, for head/tail bounds.
uint32_t text_line_count(const char *buf, uint32_t len);

#endif  // RPC_TEXTCORE_H
