// Text helpers: wc counting, line iteration (grep/head/tail rely on it), line
// count (tail bounds). The off-by-ones live here.
#include "textcore.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) { checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); } }

struct Cap { char out[512]; int n; };
static void grab(void *ctx, const char *line, uint32_t num) {
    Cap *c = (Cap *)ctx;
    char b[80]; snprintf(b, sizeof(b), "[%u:%s]", num, line);
    strcat(c->out, b); c->n++;
}

int main(void) {
    uint32_t l, w, b;
    text_count("a b c\nd e\n", 10, &l, &w, &b);
    ck(l == 2 && w == 5 && b == 10, "wc: two lines, five words, ten bytes");
    text_count("", 0, &l, &w, &b);
    ck(l == 0 && w == 0 && b == 0, "wc: empty");
    text_count("   \n\t\n", 6, &l, &w, &b);
    ck(l == 2 && w == 0, "wc: whitespace-only lines have no words");
    text_count("oneword", 7, &l, &w, &b);
    ck(l == 0 && w == 1 && b == 7, "wc: no trailing newline -> 0 newlines, 1 word");

    ck(text_line_count("a\nb\nc\n", 6) == 3, "line count with trailing newline");
    ck(text_line_count("a\nb\nc", 5) == 3, "line count without trailing newline");
    ck(text_line_count("", 0) == 0, "line count of empty is 0");

    // line iteration: numbering, NUL-termination, buffer restored
    char buf[] = "alpha\nbeta\ngamma\n";
    Cap c{{0}, 0};
    text_for_lines(buf, (uint32_t)strlen(buf), grab, &c);
    ck(c.n == 3, "iterates every line");
    ck(strcmp(c.out, "[1:alpha][2:beta][3:gamma]") == 0, "lines numbered and NUL-terminated");
    ck(strcmp(buf, "alpha\nbeta\ngamma\n") == 0, "the buffer is restored after iteration");

    // a final line with no newline is still visited
    char buf2[] = "x\ny";
    Cap c2{{0}, 0};
    text_for_lines(buf2, 3, grab, &c2);
    ck(c2.n == 2 && strcmp(c2.out, "[1:x][2:y]") == 0, "final line without newline is visited");

    // empty buffer: no lines
    Cap c3{{0}, 0};
    text_for_lines(buf2, 0, grab, &c3);
    ck(c3.n == 0, "empty buffer yields no lines");

    printf("\n%d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
