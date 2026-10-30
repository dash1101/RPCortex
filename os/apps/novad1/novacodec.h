// Desc: What every file-format codec shares - an output sink and the small parsers.
// File: novacodec.h
//
// The codecs (.sub, .ir, .nfc, WiGLE) are the pure layer of the package: text in,
// numbers out, and the other way round, with no fw_* call anywhere. That is what
// lets them be proved on a host against the MicroPython modules that wrote the same
// files first.
//
// They allocate nothing. A package has no operator new and a 12 KB heap, so a
// codec works in the caller's buffers and writes through a Sink, which the caller
// points at a file, a RAM buffer or a test. There is no stdlib number parsing
// either: atoi and strtol are not in the symbol table a package links against, so
// the few parsers below stand in for them.
#ifndef NOVA_CODEC_H
#define NOVA_CODEC_H

#include <stdint.h>
#include <string.h>

namespace nova {
namespace codec {

// Where a writer puts its output. put() takes bytes that are NOT terminated and
// returns false when the destination is full or failed, which stops the writer.
struct Sink {
    bool (*put)(void *ctx, const char *s, unsigned n);
    void  *ctx;
};

inline bool put_str(const Sink &k, const char *s) { return k.put(k.ctx, s, (unsigned)strlen(s)); }
inline bool put_chr(const Sink &k, char c)        { return k.put(k.ctx, &c, 1); }

// Decimal, unsigned and signed. No padding: the formats these codecs write never
// pad a number except the 6-digit fraction, which put_fixed6 owns.
inline bool put_u32(const Sink &k, uint32_t v) {
    char b[11];
    unsigned n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char o[11];
    for (unsigned i = 0; i < n; i++) o[i] = b[n - 1 - i];
    return k.put(k.ctx, o, n);
}

inline bool put_i32(const Sink &k, int32_t v) {
    if (v < 0) {
        if (!put_chr(k, '-')) return false;
        return put_u32(k, (uint32_t)0 - (uint32_t)v);
    }
    return put_u32(k, (uint32_t)v);
}

// A value in millionths as "0.330000" - what "{:.6f}" gave the MicroPython codec,
// without a float to do it with.
inline bool put_fixed6(const Sink &k, uint32_t ppm) {
    if (!put_u32(k, ppm / 1000000u) || !put_chr(k, '.')) return false;
    char d[6];
    uint32_t f = ppm % 1000000u;
    for (int i = 5; i >= 0; i--) { d[i] = (char)('0' + f % 10); f /= 10; }
    return k.put(k.ctx, d, 6);
}

// Two upper-case hex digits, as Flipper writes bytes.
inline bool put_hex2(const Sink &k, uint8_t v) {
    static const char H[] = "0123456789ABCDEF";
    char d[2] = { H[v >> 4], H[v & 15] };
    return k.put(k.ctx, d, 2);
}

// --- parsing ----------------------------------------------------------------------------

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Advance past blanks. Python's str.strip() / str.split() treat these the same.
inline const char *skip_ws(const char *p, const char *e) {
    while (p < e && is_space(*p)) p++;
    return p;
}

// Trim both ends of [s, e) in place by moving the bounds.
inline void trim(const char **s, const char **e) {
    while (*s < *e && is_space(**s)) (*s)++;
    while (*e > *s && is_space((*e)[-1])) (*e)--;
}

// The end of the line that starts at p: the newline, or the terminator.
inline const char *eol(const char *p) {
    while (*p && *p != '\n') p++;
    return p;
}

// Does [s, e) start with the NUL-terminated prefix?
inline bool starts(const char *s, const char *e, const char *prefix) {
    while (*prefix) { if (s >= e || *s != *prefix) return false; s++; prefix++; }
    return true;
}

// int(token) for a plain decimal token with an optional sign - the only forms a
// timing list or a frequency is written in. False when anything else is in it, which
// is where Python raised ValueError and the callers skip the token, and also for a
// value outside 32 bits: no timing, frequency or count is that large, and a
// 64-bit multiply is a library call on the RP2040.
inline bool parse_int(const char *s, const char *e, int32_t *out) {
    if (s >= e) return false;
    bool neg = false;
    if (*s == '-' || *s == '+') { neg = (*s == '-'); s++; }
    if (s >= e) return false;
    uint32_t v = 0;
    for (; s < e; s++) {
        if (*s < '0' || *s > '9') return false;
        uint32_t d = (uint32_t)(*s - '0');
        if (v > (0x7fffffffu - d) / 10u) return false;
        v = v * 10u + d;
    }
    *out = neg ? -(int32_t)v : (int32_t)v;
    return true;
}

// int(token, 16), which accepts a leading 0x. Up to 64 bits.
inline bool parse_hex(const char *s, const char *e, uint64_t *out) {
    if (e - s >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (s >= e) return false;
    uint64_t v = 0;
    for (; s < e; s++) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return false;
        if (v >> 60) return false;
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return true;
}

// The next whitespace-separated token in [*p, e), or false at the end.
inline bool next_token(const char **p, const char *e, const char **ts, const char **te) {
    const char *q = skip_ws(*p, e);
    if (q >= e) { *p = e; return false; }
    *ts = q;
    while (q < e && !is_space(*q)) q++;
    *te = q;
    *p = q;
    return true;
}

// Exact compare of [s, e) with a NUL-terminated word. File keys are case-sensitive,
// as they were in the MicroPython parsers and as a Flipper writes them.
inline bool eq_span(const char *s, const char *e, const char *word) {
    while (*word) { if (s >= e || *s != *word) return false; s++; word++; }
    return s == e;
}

// Case-insensitive compare of [s, e) with a lower-case NUL-terminated word.
inline bool ieq_span(const char *s, const char *e, const char *lower) {
    while (*lower) {
        if (s >= e) return false;
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *lower++) return false;
    }
    return s == e;
}

}  // namespace codec
}  // namespace nova

#endif  // NOVA_CODEC_H
