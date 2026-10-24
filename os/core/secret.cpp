#include "secret.h"
#include "sha256.h"

#include <string.h>

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64_encode(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = B64[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = B64[v & 63];
    }
    out[o] = 0;
    return o;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// Returns the byte count, or -1 on a bad character or an impossible length.
static int b64_decode(const char *in, uint8_t *out, size_t cap) {
    size_t n = strlen(in);
    if (n % 4 == 1) return -1;                       // no input encodes to this
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4] = {0, 0, 0, 0};
        size_t k = (n - i < 4) ? n - i : 4;
        for (size_t j = 0; j < k; j++) {
            v[j] = b64_val(in[i + j]);
            if (v[j] < 0) return -1;
        }
        uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                     ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        size_t bytes = k - 1;                        // 4 chars give 3, 3 give 2, 2 give 1
        if (o + bytes > cap) return -1;
        out[o++] = (uint8_t)(w >> 16);
        if (bytes > 1) out[o++] = (uint8_t)(w >> 8);
        if (bytes > 2) out[o++] = (uint8_t)w;
    }
    return (int)o;
}

static void subkeys(const uint8_t key[SECRET_KEY_LEN], uint8_t ek[32], uint8_t mk[32]) {
    static const uint8_t E[] = "rpc.secret.enc.v1";
    static const uint8_t M[] = "rpc.secret.mac.v1";
    hmac_sha256(key, SECRET_KEY_LEN, E, sizeof(E) - 1, ek);
    hmac_sha256(key, SECRET_KEY_LEN, M, sizeof(M) - 1, mk);
}

static void make_tag(const uint8_t mk[32], const char *ctx, const uint8_t *plain, size_t n,
                     uint8_t tag[SECRET_TAG_LEN]) {
    // len(context) || context || plain, built in one small buffer.
    uint8_t buf[1 + 255 + SECRET_MAX_PLAIN];
    size_t cl = strlen(ctx);
    buf[0] = (uint8_t)cl;
    memcpy(buf + 1, ctx, cl);
    memcpy(buf + 1 + cl, plain, n);
    uint8_t d[32];
    hmac_sha256(mk, 32, buf, 1 + cl + n, d);
    memcpy(tag, d, SECRET_TAG_LEN);
    memset(buf, 0, sizeof(buf)); memset(d, 0, sizeof(d));
}

static void apply_keystream(const uint8_t ek[32], const uint8_t tag[SECRET_TAG_LEN],
                            const uint8_t *in, uint8_t *out, size_t n) {
    for (size_t off = 0, j = 0; off < n; off += 32, j++) {
        uint8_t block_in[SECRET_TAG_LEN + 1];
        memcpy(block_in, tag, SECRET_TAG_LEN);
        block_in[SECRET_TAG_LEN] = (uint8_t)j;
        uint8_t ks[32];
        hmac_sha256(ek, 32, block_in, sizeof(block_in), ks);
        size_t take = (n - off < 32) ? n - off : 32;
        for (size_t i = 0; i < take; i++) out[off + i] = in[off + i] ^ ks[i];
        memset(ks, 0, sizeof(ks));
    }
}

bool secret_is_sealed(const char *s) { return s && s[0] == '~' && s[1] == '1'; }

bool secret_seal(const uint8_t key[SECRET_KEY_LEN], const char *context,
                 const char *plain, char *out, size_t cap) {
    if (!key || !context || !plain || !out || cap == 0) return false;
    size_t n = strlen(plain);
    if (n == 0) { out[0] = 0; return true; }
    if (n > SECRET_MAX_PLAIN || strlen(context) > 255) return false;

    uint8_t ek[32], mk[32];
    subkeys(key, ek, mk);

    uint8_t blob[SECRET_TAG_LEN + SECRET_MAX_PLAIN];
    make_tag(mk, context, (const uint8_t *)plain, n, blob);
    apply_keystream(ek, blob, (const uint8_t *)plain, blob + SECRET_TAG_LEN, n);

    size_t blob_len = SECRET_TAG_LEN + n;
    size_t text_len = 2 + (blob_len * 4 + 2) / 3;     // marker + unpadded base64
    bool fits = text_len + 1 <= cap;
    if (fits) {
        out[0] = '~'; out[1] = '1';
        b64_encode(blob, blob_len, out + 2);
    } else {
        out[0] = 0;
    }
    memset(ek, 0, sizeof(ek)); memset(mk, 0, sizeof(mk)); memset(blob, 0, sizeof(blob));
    return fits;
}

SecretResult secret_open(const uint8_t key[SECRET_KEY_LEN], const char *context,
                         const char *sealed, char *plain, size_t cap) {
    if (plain && cap) plain[0] = 0;
    if (!key || !context || !sealed || !plain || cap == 0) return SECRET_BAD;
    if (!secret_is_sealed(sealed)) return SECRET_NOT_SEALED;
    if (strlen(context) > 255) return SECRET_BAD;

    uint8_t blob[SECRET_TAG_LEN + SECRET_MAX_PLAIN];
    int bl = b64_decode(sealed + 2, blob, sizeof(blob));
    if (bl <= SECRET_TAG_LEN) return SECRET_BAD;
    size_t n = (size_t)bl - SECRET_TAG_LEN;
    if (n + 1 > cap) { memset(blob, 0, sizeof(blob)); return SECRET_BAD; }

    uint8_t ek[32], mk[32];
    subkeys(key, ek, mk);

    uint8_t pt[SECRET_MAX_PLAIN];
    apply_keystream(ek, blob, blob + SECRET_TAG_LEN, pt, n);

    uint8_t tag[SECRET_TAG_LEN];
    make_tag(mk, context, pt, n, tag);

    // Compare every byte regardless of where they first differ.
    uint8_t diff = 0;
    for (size_t i = 0; i < SECRET_TAG_LEN; i++) diff |= (uint8_t)(tag[i] ^ blob[i]);

    // A plaintext with a NUL in it is not a password this code could have sealed.
    for (size_t i = 0; i < n; i++) if (pt[i] == 0) diff |= 1;

    SecretResult r = SECRET_BAD;
    if (diff == 0) { memcpy(plain, pt, n); plain[n] = 0; r = SECRET_OK; }
    memset(ek, 0, sizeof(ek)); memset(mk, 0, sizeof(mk));
    memset(blob, 0, sizeof(blob)); memset(pt, 0, sizeof(pt));
    return r;
}
