#include "b64.h"

static const char kAlpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t b64_encode(const uint8_t *in, uint32_t n, char *out) {
    uint32_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = kAlpha[(v >> 18) & 63];
        out[o++] = kAlpha[(v >> 12) & 63];
        out[o++] = kAlpha[(v >> 6) & 63];
        out[o++] = kAlpha[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        out[o++] = kAlpha[(v >> 18) & 63];
        out[o++] = kAlpha[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? kAlpha[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

static int val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int32_t b64_decode(const char *in, uint32_t len, uint8_t *out, uint32_t cap) {
    if (len % 4) return -1;
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i += 4) {
        bool last = (i + 4 == len);
        int a = val(in[i]), b = val(in[i + 1]);
        if (a < 0 || b < 0) return -1;
        int c = -2, d = -2;                       // -2 means padding
        if (in[i + 2] == '=') {
            if (!last || in[i + 3] != '=') return -1;
        } else {
            c = val(in[i + 2]);
            if (c < 0) return -1;
            if (in[i + 3] == '=') { if (!last) return -1; }
            else { d = val(in[i + 3]); if (d < 0) return -1; }
        }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                     ((uint32_t)(c >= 0 ? c : 0) << 6) | (uint32_t)(d >= 0 ? d : 0);
        uint32_t take = (c < 0) ? 1 : (d < 0) ? 2 : 3;
        if (o + take > cap) return -1;
        out[o++] = (uint8_t)(v >> 16);
        if (take > 1) out[o++] = (uint8_t)(v >> 8);
        if (take > 2) out[o++] = (uint8_t)v;
    }
    return (int32_t)o;
}
