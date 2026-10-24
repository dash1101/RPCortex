// Sealing a saved password: the HMAC it is built on, the exact output for known
// inputs, and every way a sealed value can be wrong.
//
// The known-answer values were produced by a separate Python implementation of
// the construction written down in core/secret.h, so a mistake in the C++ cannot
// be repeated in the expected values. The HMAC vectors are RFC 4231.

#include "secret.h"
#include "sha256.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}
static void hexs(const uint8_t *d, size_t n, char *out) { hex_encode(d, n, out); out[n * 2] = 0; }

static void hmac_case(const uint8_t *k, size_t kl, const uint8_t *m, size_t ml, const char *want, const char *what) {
    uint8_t d[32]; char h[65];
    hmac_sha256(k, kl, m, ml, d);
    hexs(d, 32, h);
    ck(!strcmp(h, want), what);
}


// --- a second, test-side implementation of the construction ----------------
//
// Some inputs cannot be made by secret_seal (a plaintext with a NUL in it, a tag
// altered after sealing), so the checks that need them build the text here from
// the written-down construction and the HMAC primitive alone.
static void ref_b64(const uint8_t *in, size_t n, char *out) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = A[(v >> 18) & 63]; out[o++] = A[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = A[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = A[v & 63];
    }
    out[o] = 0;
}
// Seal `n` raw bytes (which may include zeros). `tag_override`, when given, replaces
// the computed tag after the keystream was derived from the real one, which is what
// a forger who can edit the file but cannot compute the tag would be doing.
static void ref_seal(const uint8_t key[32], const char *ctx, const uint8_t *plain, size_t n,
                     const uint8_t *tag_override, char *out) {
    uint8_t ek[32], mk[32], d[32], buf[1 + 255 + 64], blob[6 + 64];
    hmac_sha256(key, 32, (const uint8_t *)"rpc.secret.enc.v1", 17, ek);
    hmac_sha256(key, 32, (const uint8_t *)"rpc.secret.mac.v1", 17, mk);
    size_t cl = strlen(ctx);
    buf[0] = (uint8_t)cl; memcpy(buf + 1, ctx, cl); memcpy(buf + 1 + cl, plain, n);
    hmac_sha256(mk, 32, buf, 1 + cl + n, d);
    uint8_t tag[6]; memcpy(tag, d, 6);
    for (size_t off = 0, j = 0; off < n; off += 32, j++) {
        uint8_t bi[7]; memcpy(bi, tag, 6); bi[6] = (uint8_t)j;
        uint8_t ks[32]; hmac_sha256(ek, 32, bi, 7, ks);
        for (size_t i = 0; i < 32 && off + i < n; i++) blob[6 + off + i] = plain[off + i] ^ ks[i];
    }
    memcpy(blob, tag_override ? tag_override : tag, 6);
    out[0] = '~'; out[1] = '1';
    ref_b64(blob, 6 + n, out + 2);
}

int main(void) {
    // --- the primitive: RFC 4231 -------------------------------------------
    {
        uint8_t k20[20]; memset(k20, 0x0b, 20);
        hmac_case(k20, 20, (const uint8_t *)"Hi There", 8,
                  "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "HMAC case 1");
        hmac_case((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28,
                  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "HMAC case 2");
        uint8_t ka[20]; memset(ka, 0xaa, 20); uint8_t dd[50]; memset(dd, 0xdd, 50);
        hmac_case(ka, 20, dd, 50,
                  "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", "HMAC case 3");
        uint8_t kl[131]; memset(kl, 0xaa, 131);
        const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
        hmac_case(kl, 131, (const uint8_t *)m6, strlen(m6),
                  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "HMAC case 6 (key longer than a block)");
        const char *m7 = "This is a test using a larger than block-size key and a larger than block-size data. "
                         "The key needs to be hashed before being used by the HMAC algorithm.";
        hmac_case(kl, 131, (const uint8_t *)m7, strlen(m7),
                  "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", "HMAC case 7 (long key, long data)");
        uint8_t d[32]; char h[65];
        sha256("abc", 3, d); hexs(d, 32, h);
        ck(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc");
    }

    uint8_t key[32]; for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    char out[SECRET_SEAL_MAX], back[80];

    // --- known answers from the independent implementation ----------------
    struct KA { const char *ctx, *pw, *want; } ka[] = {
        {"HomeNet",  "correct horse battery", "~1RvadHQCvEJ2C6oN0OV4lrBvZJrbL-0cTjv4w"},
        {"x",        "a",                     "~1Iaoe9KL6cA"},
        {"Net",      "s3cr3t-Pass_word!",     "~1VagyaCX92fOsNLyBniGXAj-Q2WGbsRY"},
    };
    for (auto &t : ka) {
        ck(secret_seal(key, t.ctx, t.pw, out, sizeof(out)) && !strcmp(out, t.want), "known-answer seal matches the reference");
        ck(secret_open(key, t.ctx, t.want, back, sizeof(back)) == SECRET_OK && !strcmp(back, t.pw),
           "known-answer value opens to the original");
    }
    {
        char p63[64]; memset(p63, 'p', 63); p63[63] = 0;
        ck(secret_seal(key, "Cafe WiFi", p63, out, sizeof(out)) &&
           !strcmp(out, "~1ucC_s_wyCzuxhmYTgpkndKbqgdEYBtY40gD4DSAcGdSWWn2Ah27bq4DposytQ6eWYrfC6PDM455sevr276jTHt9Lj-6H"),
           "a 63-character passphrase matches the reference");
        ck(strlen(out) == 94, "and seals to 94 characters, inside the registry's 95");
    }

    // --- round trips at every length, and the size bound ---------------------
    {
        bool all = true, bound = true, alphabet = true;
        for (int n = 1; n <= SECRET_MAX_PLAIN; n++) {
            char pw[64];
            for (int i = 0; i < n; i++) pw[i] = (char)('!' + (i * 7 + n) % 90);
            pw[n] = 0;
            if (!secret_seal(key, "Some SSID", pw, out, sizeof(out))) { all = false; continue; }
            if (strlen(out) > 94) bound = false;
            for (const char *c = out + 2; *c; c++)
                if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
                    alphabet = false;
            if (secret_open(key, "Some SSID", out, back, sizeof(back)) != SECRET_OK || strcmp(back, pw)) all = false;
        }
        ck(all, "every length from 1 to 63 round-trips");
        ck(bound, "no sealed value exceeds 94 characters");
        ck(alphabet, "sealed text uses only base64url characters: no '=', spaces or quotes");
    }

    // --- the same input seals the same way; different input does not --------
    {
        char a[SECRET_SEAL_MAX], b[SECRET_SEAL_MAX], c[SECRET_SEAL_MAX], d[SECRET_SEAL_MAX];
        secret_seal(key, "Net", "hunter22", a, sizeof(a));
        secret_seal(key, "Net", "hunter22", b, sizeof(b));
        secret_seal(key, "Net2", "hunter22", c, sizeof(c));
        secret_seal(key, "Net", "hunter23", d, sizeof(d));
        ck(!strcmp(a, b), "sealing is deterministic, so an unchanged save rewrites nothing");
        ck(strcmp(a, c) != 0, "a different network name seals differently");
        ck(strcmp(a, d) != 0, "a different password seals differently");
        ck(strstr(a, "hunter") == nullptr, "the password does not appear in the sealed text");
    }

    // --- rejections -----------------------------------------------------------
    {
        secret_seal(key, "HomeNet", "correct horse battery", out, sizeof(out));
        uint8_t other[32]; memcpy(other, key, 32); other[31] ^= 1;
        ck(secret_open(other, "HomeNet", out, back, sizeof(back)) == SECRET_BAD && back[0] == 0,
           "a key that differs in one bit does not open it, and nothing is returned");
        ck(secret_open(key, "HomeNet2", out, back, sizeof(back)) == SECRET_BAD && back[0] == 0,
           "a different network name does not open it: it cannot be moved between entries");
        ck(secret_open(key, "homenet", out, back, sizeof(back)) == SECRET_BAD,
           "the network name is case-sensitive");

        // Every position, replaced by a different valid character.
        size_t n = strlen(out);
        int accepted = 0;
        for (size_t i = 2; i < n; i++) {
            char t[SECRET_SEAL_MAX]; memcpy(t, out, n + 1);
            t[i] = (t[i] == 'A') ? 'B' : 'A';
            if (secret_open(key, "HomeNet", t, back, sizeof(back)) == SECRET_OK) accepted++;
        }
        ck(accepted == 0, "changing any single character is detected");

        char t[SECRET_SEAL_MAX];
        memcpy(t, out, n + 1); t[n - 1] = 0;
        ck(secret_open(key, "HomeNet", t, back, sizeof(back)) == SECRET_BAD, "a truncated value is rejected");
        memcpy(t, out, n + 1); t[n] = 'A'; t[n + 1] = 0;
        ck(secret_open(key, "HomeNet", t, back, sizeof(back)) == SECRET_BAD, "an extended value is rejected");
        ck(secret_open(key, "HomeNet", "~1", back, sizeof(back)) == SECRET_BAD, "a marker with nothing after it is rejected");
        ck(secret_open(key, "HomeNet", "~1AAAAAA", back, sizeof(back)) == SECRET_BAD, "a tag-sized value with no body is rejected");
        ck(secret_open(key, "HomeNet", "~1ab$d", back, sizeof(back)) == SECRET_BAD, "a character outside the alphabet is rejected");
        ck(secret_open(key, "HomeNet", "~1abcde", back, sizeof(back)) == SECRET_BAD, "an impossible length is rejected");
        ck(secret_open(key, "HomeNet", out, back, 5) == SECRET_BAD, "an output buffer that is too small is refused, not overrun");
        ck(secret_open(key, "HomeNet", out, back, 22) == SECRET_OK, "and one that is exactly big enough works");
        ck(secret_open(key, "HomeNet", out, back, 21) == SECRET_BAD, "one byte short does not");
    }

    // --- values that are not sealed ------------------------------------------
    {
        ck(secret_open(key, "Net", "hunter2", back, sizeof(back)) == SECRET_NOT_SEALED && back[0] == 0,
           "an old plaintext value is reported as not sealed, and is not copied out");
        ck(secret_open(key, "Net", "", back, sizeof(back)) == SECRET_NOT_SEALED, "an empty value is not sealed");
        ck(!secret_is_sealed("hunter2") && !secret_is_sealed("") && !secret_is_sealed(nullptr) && !secret_is_sealed("~"),
           "marker detection");
        ck(secret_is_sealed("~1x"), "marker detection: sealed");
        ck(secret_seal(key, "Open", "", out, sizeof(out)) && out[0] == 0, "an empty password stays empty");
    }


    // --- the test-side construction agrees with the module ---------------------
    {
        char a[SECRET_SEAL_MAX], b[SECRET_SEAL_MAX];
        secret_seal(key, "HomeNet", "correct horse battery", a, sizeof(a));
        ref_seal(key, "HomeNet", (const uint8_t *)"correct horse battery", 21, nullptr, b);
        ck(!strcmp(a, b), "the test-side construction produces the module's output");
    }

    // --- the tag is checked in full -----------------------------------------------
    // Keep the first four tag bytes, vary the last two. The keystream stays tied to
    // the original tag, so an attacker would be guessing; a verifier that compared
    // only some of the tag would accept about one guess in 256 per byte skipped.
    // 65,535 guesses is enough to catch a compare of two bytes or fewer. A compare
    // of four is a smaller security margin rather than a different behaviour, and
    // 2^24 guesses to show it is too slow for a test that runs on every build.
    {
        const uint8_t *pw = (const uint8_t *)"correct horse battery";
        uint8_t real[6];
        {   // recover the real tag the same way ref_seal computes it
            uint8_t mk[32], d[32], buf[1 + 7 + 21];
            hmac_sha256(key, 32, (const uint8_t *)"rpc.secret.mac.v1", 17, mk);
            buf[0] = 7; memcpy(buf + 1, "HomeNet", 7); memcpy(buf + 8, pw, 21);
            hmac_sha256(mk, 32, buf, 29, d); memcpy(real, d, 6);
        }
        int accepted = 0, tried = 0;
        for (int hi = 0; hi < 256; hi++) for (int lo = 0; lo < 256; lo++) {
            if (hi == real[4] && lo == real[5]) continue;
            uint8_t forged[6]; memcpy(forged, real, 6); forged[4] = (uint8_t)hi; forged[5] = (uint8_t)lo;
            char text[SECRET_SEAL_MAX];
            ref_seal(key, "HomeNet", pw, 21, forged, text);
            tried++;
            if (secret_open(key, "HomeNet", text, back, sizeof(back)) == SECRET_OK) accepted++;
        }
        ck(tried == 65535 && accepted == 0, "none of 65,535 values with a wrong tag tail is accepted");
    }

    // --- a value that decrypts to something with a NUL in it is refused ---------
    {
        const uint8_t withnul[5] = {'a', 'b', 0, 'c', 'd'};
        char text[SECRET_SEAL_MAX];
        ref_seal(key, "Net", withnul, 5, nullptr, text);       // correctly tagged
        ck(secret_open(key, "Net", text, back, sizeof(back)) == SECRET_BAD && back[0] == 0,
           "a correctly tagged value containing a NUL is refused rather than silently shortened");
    }

    // --- sealing refuses what it cannot do ------------------------------------
    {
        char p64[65]; memset(p64, 'q', 64); p64[64] = 0;
        ck(!secret_seal(key, "Net", p64, out, sizeof(out)), "a password over 63 characters is refused");
        ck(!secret_seal(key, "Net", "hunter22", out, 10) && out[0] == 0, "a buffer that is too small is refused, and left empty");
        char exact[SECRET_SEAL_MAX];
        secret_seal(key, "Net", "hunter22", exact, sizeof(exact));
        size_t need = strlen(exact) + 1;
        ck(secret_seal(key, "Net", "hunter22", out, need), "a buffer of exactly the needed size works");
        ck(!secret_seal(key, "Net", "hunter22", out, need - 1), "and one byte less does not");
        ck(!secret_seal(nullptr, "Net", "x", out, sizeof(out)) && !secret_seal(key, nullptr, "x", out, sizeof(out)) &&
           !secret_seal(key, "Net", nullptr, out, sizeof(out)) && !secret_seal(key, "Net", "x", nullptr, 10) &&
           !secret_seal(key, "Net", "x", out, 0), "missing arguments are refused");
    }

    printf("  secret: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
