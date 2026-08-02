#include "out.h"

#include <stdio.h>
#include <stdarg.h>

static bool g_had_error;

void out_clear_error(void) { g_had_error = false; }
bool out_had_error(void)   { return g_had_error; }

// The shape v1's _fmt built:
//   <colour>[<white><symbol><colour>] [<white><prefix><colour>] <reset><msg>
static void emit(const char *colour, const char *symbol, const char *p,
                 const char *fmt, va_list ap) {
    printf("%s[%s%s%s]", colour, C_WHITE, symbol, colour);
    if (p) printf(" %s[%s%s%s]", colour, C_WHITE, p, colour);
    printf(" %s", C_RESET);
    vprintf(fmt, ap);
    printf("\n");
}

#define TAGGED(name, colour, symbol)                       \
    void name(const char *fmt, ...) {                      \
        va_list ap; va_start(ap, fmt);                     \
        emit(colour, symbol, nullptr, fmt, ap);            \
        va_end(ap);                                        \
    }
#define TAGGED_P(name, colour, symbol)                     \
    void name(const char *p, const char *fmt, ...) {       \
        va_list ap; va_start(ap, fmt);                     \
        emit(colour, symbol, p, fmt, ap);                  \
        va_end(ap);                                        \
    }

TAGGED  (out_ok,    C_CYAN,   "@")
TAGGED  (out_info,  C_HEADER, ":")
TAGGED  (out_warn,  C_WARN,   "?")
TAGGED_P(out_okp,   C_CYAN,   "@")
TAGGED_P(out_infop, C_HEADER, ":")
TAGGED_P(out_warnp, C_WARN,   "?")

// The two that set the error flag get explicit bodies rather than the macro.
void out_err(const char *fmt, ...) {
    g_had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!", nullptr, fmt, ap);
    va_end(ap);
}

void out_errp(const char *p, const char *fmt, ...) {
    g_had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!", p, fmt, ap);
    va_end(ap);
}

void out_fatal(const char *fmt, ...) {
    g_had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!!!", nullptr, fmt, ap);
    va_end(ap);
}

void out_multi(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void out_blank(void) { putchar('\n'); }

void out_prompt(const char *msg) {
    printf("%s%s %s••>  %s", C_RESET, msg, C_CYAN, C_RESET);
}

// Copy `s` and append spaces until it is `width` visible characters wide,
// skipping ANSI CSI sequences ("\033[" ... final byte in @-~) when counting.
void out_pad(const char *s, int width, char *dst, int cap) {
    int vis = 0, o = 0;
    for (const char *p = s; *p && o + 1 < cap; ) {
        if (p[0] == '\033' && p[1] == '[') {
            while (*p && o + 1 < cap) {                 // copy the escape, count none
                char c = *p++;
                dst[o++] = c;
                if (c >= '@' && c <= '~' && c != '[') break;
            }
            continue;
        }
        // UTF-8 continuation bytes are part of the character before them.
        if (((unsigned char)*p & 0xC0) != 0x80) vis++;
        dst[o++] = *p++;
    }
    while (vis < width && o + 1 < cap) { dst[o++] = ' '; vis++; }
    dst[o] = 0;
}
