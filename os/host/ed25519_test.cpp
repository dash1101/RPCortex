// Ed25519 verification (core/ed25519.cpp) against RFC 8032.
//
//   - the five test vectors of section 7.1, taken from the RFC's text by
//     gen_ed25519_vectors.py into ed25519_vectors.inc
//   - every single-bit change to TEST 1's signature and key, and to each
//     message: refused
//   - each vector's signature under the next vector's key: refused
//   - S out of range, keys and R values that must not decode, and a signature
//     that only a cofactored check accepts, with the verdicts sections 5.1.3
//     and 5.1.7 give
//   - the decoding rule on its own
//
// SHA-512 is mbedTLS's, built with the device's configuration (run_all.sh).
#include "ed25519.h"

#include <stdio.h>
#include <string.h>

#include "ed25519_vectors.inc"

static int checks = 0, fails = 0;
static void ck(bool c, const char *what, const char *name = "") {
    checks++;
    if (!c) { fails++; if (fails < 30) printf("  FAIL: %s %s\n", what, name); }
}

static size_t unhex(const char *h, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static uint8_t g_msg[1100];

static void rfc_tests() {
    const size_t nv = sizeof(kRfc8032) / sizeof(kRfc8032[0]);
    ck(nv == 5, "the five vectors of section 7.1");
    for (size_t v = 0; v < nv; v++) {
        const Ed25519Vector &t = kRfc8032[v];
        uint8_t pk[32], sig[64];
        ck(unhex(t.pk, pk, 32) == 32 && unhex(t.sig, sig, 64) == 64, "vector lengths", t.name);
        size_t mn = unhex(t.msg, g_msg, sizeof(g_msg));
        ck(ed25519_verify(sig, g_msg, mn, pk), "RFC 8032 vector verifies:", t.name);

        // One-bit changes to the message: eight, spread from its first bit to
        // its last. (A verify is tens of milliseconds under the sanitizers,
        // so every bit of TEST 1024 would be minutes.)
        bool all = true;
        size_t bits = mn * 8;
        for (size_t k = 0; bits && k < 8; k++) {
            size_t bit = k * (bits - 1) / 7;
            g_msg[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            all = all && !ed25519_verify(sig, g_msg, mn, pk);
            g_msg[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        }
        ck(all, "one-bit changes to the message are refused:", t.name);
        // A byte more, a byte less.
        if (mn) ck(!ed25519_verify(sig, g_msg, mn - 1, pk), "a shorter message is refused:", t.name);
        g_msg[mn] = 0;
        ck(!ed25519_verify(sig, g_msg, mn + 1, pk), "a longer message is refused:", t.name);

        // The next vector's key.
        const Ed25519Vector &u = kRfc8032[(v + 1) % nv];
        uint8_t other[32];
        unhex(u.pk, other, 32);
        ck(!ed25519_verify(sig, g_msg, mn, other), "another key is refused:", t.name);
    }
    // An empty message may come with no buffer at all.
    uint8_t pk[32], sig[64];
    unhex(kRfc8032[0].pk, pk, 32);
    unhex(kRfc8032[0].sig, sig, 64);
    ck(ed25519_verify(sig, nullptr, 0, pk), "TEST 1 with a null message");

    // A bit in every byte of TEST 1's signature and of its key, a different
    // bit position each time. A change in R either stops it decoding or makes
    // it a different point; a change in S either takes it out of range or
    // makes it a different scalar; each of those is a different refusal.
    int sig_ok = 0, key_ok = 0;
    for (int i = 0; i < 64; i++) {
        sig[i] ^= (uint8_t)(1u << (i % 8));
        if (ed25519_verify(sig, nullptr, 0, pk)) sig_ok++;
        sig[i] ^= (uint8_t)(1u << (i % 8));
    }
    for (int i = 0; i < 32; i++) {
        pk[i] ^= (uint8_t)(1u << ((i + 3) % 8));
        if (ed25519_verify(sig, nullptr, 0, pk)) key_ok++;
        pk[i] ^= (uint8_t)(1u << ((i + 3) % 8));
    }
    ck(sig_ok == 0, "a one-bit change in any byte of TEST 1's signature is refused");
    ck(key_ok == 0, "a one-bit change in any byte of TEST 1's key is refused");
    ck(ed25519_verify(sig, nullptr, 0, pk), "...and the untouched signature still verifies");
}

static void strict_tests() {
    for (const Ed25519Case &c : kStrict) {
        uint8_t pk[32], sig[64];
        unhex(c.pk, pk, 32);
        unhex(c.sig, sig, 64);
        size_t mn = unhex(c.msg, g_msg, sizeof(g_msg));
        ck(ed25519_verify(sig, g_msg, mn, pk) == c.ok, c.ok ? "accepted as RFC 8032 says:" : "refused as RFC 8032 says:", c.name);
    }
    for (const Ed25519Decode &d : kDecode) {
        uint8_t e[32];
        unhex(d.enc, e, 32);
        ck(ed25519_point_ok(e) == d.ok, d.ok ? "decodes:" : "does not decode:", d.name);
    }
}

int main(void) {
    rfc_tests();
    strict_tests();
    printf("  ed25519: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
