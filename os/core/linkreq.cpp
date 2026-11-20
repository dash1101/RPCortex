#include "linkreq.h"
#include <string.h>

bool link_parse(const char *line, LinkReq *out) {
    if (!line || !out) return false;
    uint32_t n = 0;
    while (line[n] && line[n] != ' ') n++;
    if (n == 0 || n >= sizeof(out->verb)) return false;
    memcpy(out->verb, line, n);
    out->verb[n] = 0;
    out->rest = line[n] == ' ' ? line + n + 1 : line + n;
    return true;
}

bool link_norm_hex(char *hex) {
    if (!hex) return false;
    uint32_t i = 0;
    for (; hex[i]; i++) {
        char c = hex[i];
        if (c >= 'A' && c <= 'F') hex[i] = (char)(c - 'A' + 'a');
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return i == 64;
}

bool link_parse_put(const char *rest, uint32_t *size, char sha_hex[65], const char **path) {
    if (!rest || !size || !sha_hex || !path) return false;
    // size: 1..10 decimal digits, fitting in 32 bits
    const char *p = rest;
    uint64_t v = 0;
    uint32_t digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        if (++digits > 10 || v > 0xFFFFFFFFull) return false;
        p++;
    }
    if (!digits || *p != ' ') return false;
    p++;
    // digest: exactly 64 characters then a space
    uint32_t h = 0;
    while (p[h] && p[h] != ' ') h++;
    if (h != 64 || p[h] != ' ') return false;
    memcpy(sha_hex, p, 64);
    sha_hex[64] = 0;
    if (!link_norm_hex(sha_hex)) return false;
    p += 65;
    if (!*p) return false;                    // no path
    *size = (uint32_t)v;
    *path = p;
    return true;
}
