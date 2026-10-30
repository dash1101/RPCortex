// Desc: SHA-1, HMAC-SHA1, base32 and TOTP - see novatotp.h.
// File: novatotp.cpp
#include "novatotp.h"

#include <string.h>

namespace nova {
namespace totp {

static uint32_t rol(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void sha1_begin(Sha1 *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->len = 0;
}

void sha1_add(Sha1 *s, const void *data, unsigned n) {
    const uint8_t *p = (const uint8_t *)data;
    while (n) {
        unsigned at = s->len & 63u;
        unsigned take = 64u - at;
        if (take > n) take = n;
        memcpy(s->block + at, p, take);
        s->len += take; p += take; n -= take;
        if (((s->len) & 63u) == 0) sha1_block(s, s->block);
    }
}

void sha1_end(Sha1 *s, uint8_t out[20]) {
    uint32_t bits_hi = s->len >> 29, bits_lo = s->len << 3;       // the length in bits, as a 64-bit pair
    uint8_t pad = 0x80;
    sha1_add(s, &pad, 1);
    pad = 0;
    while ((s->len & 63u) != 56u) sha1_add(s, &pad, 1);
    uint8_t lenb[8] = { (uint8_t)(bits_hi >> 24), (uint8_t)(bits_hi >> 16), (uint8_t)(bits_hi >> 8), (uint8_t)bits_hi,
                        (uint8_t)(bits_lo >> 24), (uint8_t)(bits_lo >> 16), (uint8_t)(bits_lo >> 8), (uint8_t)bits_lo };
    sha1_add(s, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8); out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void hmac_sha1(const uint8_t *key, unsigned klen, const uint8_t *msg, unsigned mlen, uint8_t out[20]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (klen > 64) {
        Sha1 s; sha1_begin(&s); sha1_add(&s, key, klen); sha1_end(&s, k);
    } else if (klen) {
        memcpy(k, key, klen);
    }
    uint8_t pad[64], inner[20];
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x36);
    Sha1 s;
    sha1_begin(&s); sha1_add(&s, pad, 64); sha1_add(&s, msg, mlen); sha1_end(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)(k[i] ^ 0x5C);
    sha1_begin(&s); sha1_add(&s, pad, 64); sha1_add(&s, inner, 20); sha1_end(&s, out);
}

unsigned b32_decode(const char *text, uint8_t *out, unsigned cap) {
    unsigned bits = 0, n = 0;
    uint32_t val = 0;
    for (const char *p = text ? text : ""; *p; p++) {
        char ch = *p;
        if (ch == ' ' || ch == '-') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        int idx;
        if (ch >= 'A' && ch <= 'Z')      idx = ch - 'A';
        else if (ch >= '2' && ch <= '7') idx = ch - '2' + 26;
        else continue;                              // '=' and anything else adds nothing
        val = ((val << 5) | (uint32_t)idx) & 0xFFFFu;     // only the low bits can still become output
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap) out[n++] = (uint8_t)((val >> bits) & 0xFF);
        }
    }
    return n;
}

void code(const uint8_t *key, unsigned klen, uint32_t unix_time, uint32_t step, unsigned digits, char *out) {
    if (!step) step = 30;
    if (digits < 1) digits = 6;
    if (digits > 9) digits = 9;
    uint32_t ctr = unix_time / step;
    uint8_t msg[8] = { 0, 0, 0, 0, (uint8_t)(ctr >> 24), (uint8_t)(ctr >> 16), (uint8_t)(ctr >> 8), (uint8_t)ctr };
    uint8_t h[20];
    hmac_sha1(key, klen, msg, 8, h);
    unsigned off = h[19] & 0x0F;
    uint32_t bin = ((uint32_t)(h[off] & 0x7F) << 24) | ((uint32_t)h[off + 1] << 16) |
                   ((uint32_t)h[off + 2] << 8) | h[off + 3];
    uint32_t mod = 1;
    for (unsigned i = 0; i < digits; i++) mod *= 10;
    uint32_t v = bin % mod;
    for (int i = (int)digits - 1; i >= 0; i--) { out[i] = (char)('0' + v % 10); v /= 10; }
    out[digits] = 0;
}

bool code_b32(const char *secret_b32, uint32_t unix_time, char *out) {
    uint8_t key[64];
    unsigned n = b32_decode(secret_b32, key, sizeof(key));
    if (!n) { out[0] = 0; return false; }
    code(key, n, unix_time, 30, 6, out);
    return true;
}

uint32_t unix_from_civil(int year, int month, int day, int hour, int minute, int second) {
    // Howard Hinnant's days_from_civil.
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int days = era * 146097 + (int)doe - 719468;
    return (uint32_t)days * 86400u + (uint32_t)hour * 3600u + (uint32_t)minute * 60u + (uint32_t)second;
}

}  // namespace totp
}  // namespace nova
