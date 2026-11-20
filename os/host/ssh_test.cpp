// The SSH core, piece by piece, against published vectors and against a
// second implementation wherever the first could share a mistake with itself.
//
//   - wire encodings: the examples in RFC 4251 section 5
//   - X25519: the vectors in RFC 7748 sections 5.2 and 6.1
//   - the key derivation: rebuilt here from RFC 4253 7.2 with the OS's own
//     SHA-256 (core/sha256.cpp), not mbedTLS
//   - signatures: RSA, ECDSA and Ed25519 signatures made by OpenSSH's
//     ssh-keygen, and an Ed25519 login signed by RFC 8032's own code
//   - fingerprints: the strings ssh-keygen -l printed for fixed keys
//   - the CTR ciphers' MAC: recomputed with core/sha256.cpp's HMAC over the
//     bytes the RFC says it covers
//   - a client core and a server core talking to each other in memory, for
//     the protocol rules that need a misbehaving peer to provoke: strict KEX
//     against an injected packet (Terrapin), the window, the attempt limit,
//     re-keying started from either end mid-stream
//
// Interoperability with OpenSSH itself is ssh_interop_test.sh.
#include "sshserver.h"
#include "sshclient.h"
#include "sshsess.h"
#include "sshkeys.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "ssh_vectors.inc"

extern "C" int mbedtls_hardware_poll(void *, unsigned char *out, size_t len, size_t *olen) {
    static int fd = -1;
    if (fd < 0) fd = open("/dev/urandom", O_RDONLY);
    if (read(fd, out, len) != (ssize_t)len) return -1;
    *olen = len;
    return 0;
}

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

static uint32_t unhex(const char *h, uint8_t *out, uint32_t cap) {
    uint32_t n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) {
        unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v;
    }
    return n;
}
static bool hexeq(const uint8_t *d, uint32_t n, const char *want) {
    uint8_t w[256];
    uint32_t wn = unhex(want, w, sizeof(w));
    return wn == n && memcmp(d, w, n) == 0;
}

// --- RFC 4251 section 5 -----------------------------------------------------------------

static void wire_tests() {
    uint8_t b[64];
    SshW w;
    struct { const char *v, *rep; } mp[] = {
        { "",                "00000000" },
        { "09a378f9b2e332a7", "0000000809a378f9b2e332a7" },
        { "80",              "000000020080" },
        { "0000000080",      "000000020080" },        // leading zeros are not encoded
    };
    for (auto &m : mp) {
        uint8_t v[16]; uint32_t vn = unhex(m.v, v, sizeof(v));
        sshw_init(&w, b, sizeof(b));
        sshw_mpint(&w, v, vn);
        ck(!w.err && hexeq(b, w.len, m.rep), "mpint encoding matches RFC 4251");
    }
    // The negative examples must be refused, not read as large numbers.
    uint8_t neg[16]; uint32_t nn = unhex("00000002edcc", neg, sizeof(neg));
    SshR r; sshr_init(&r, neg, nn);
    const uint8_t *mag; uint32_t ml;
    ck(!sshr_mpint(&r, &mag, &ml) && r.err, "negative mpint (-1234) refused");
    nn = unhex("0000000180", neg, sizeof(neg));    // 0x80 with no 00 in front: negative
    sshr_init(&r, neg, nn);
    ck(!sshr_mpint(&r, &mag, &ml), "mpint with its top bit set is negative");

    sshw_init(&w, b, sizeof(b));
    sshw_cstr(&w, "zlib,none");
    ck(hexeq(b, w.len, "000000097a6c69622c6e6f6e65"), "name-list encoding matches RFC 4251");

    // Lengths that run past the end set the error and stay set.
    uint8_t s[] = { 0, 0, 0, 9, 'a', 'b' };
    sshr_init(&r, s, sizeof(s));
    const uint8_t *d; uint32_t dn;
    ck(!sshr_string(&r, &d, &dn) && r.err, "string longer than the packet refused");
    ck(sshr_u32(&r) == 0 && r.err, "error is sticky");
    uint8_t huge[] = { 0xff, 0xff, 0xff, 0xff };
    sshr_init(&r, huge, sizeof(huge));
    ck(!sshr_string(&r, &d, &dn), "length 0xffffffff refused without wrapping");
    uint8_t withnul[] = { 0, 0, 0, 3, 'a', 0, 'b' };
    char out[8];
    sshr_init(&r, withnul, sizeof(withnul));
    ck(!sshr_cstr(&r, out, sizeof(out)), "a name with a NUL inside refused");
    // The writer refuses to run off its buffer.
    sshw_init(&w, b, 6);
    sshw_cstr(&w, "toolong");
    ck(w.err && w.len <= 6, "writer overflow flagged");

    // Negotiation: the client's order wins, whole names only.
    char pick[32];
    const char *cl = "aes256-ctr,aes128-gcm@openssh.com", *sv = "aes128-gcm@openssh.com,aes256-ctr";
    ck(ssh_negotiate((const uint8_t *)cl, (uint32_t)strlen(cl), (const uint8_t *)sv, (uint32_t)strlen(sv),
                     pick, sizeof(pick)) && !strcmp(pick, "aes256-ctr"), "negotiation follows the client");
    const char *a = "aes128-ctr-foo", *bb = "aes128-ctr";
    ck(!ssh_namelist_has((const uint8_t *)a, (uint32_t)strlen(a), "aes128-ctr"), "whole names only");
    ck(!ssh_negotiate((const uint8_t *)a, (uint32_t)strlen(a), (const uint8_t *)bb, (uint32_t)strlen(bb),
                      pick, sizeof(pick)), "no common algorithm");
}

// --- RFC 7748 -------------------------------------------------------------------------------

static void x25519_tests() {
    struct { const char *k, *u, *out; } v[] = {
        { "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
          "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552" },
        { "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
          "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
          "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957" },
    };
    for (auto &t : v) {
        uint8_t k[32], u[32], o[32];
        unhex(t.k, k, 32); unhex(t.u, u, 32);
        ck(ssh_x25519(o, k, u) && hexeq(o, 32, t.out), "X25519 matches RFC 7748 5.2");
    }
    uint8_t a[32], b[32], nine[32] = { 9 }, pa[32], pb[32], ka[32], kb[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
    ck(ssh_x25519(pa, a, nine) &&
       hexeq(pa, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"), "Alice's public key");
    ck(ssh_x25519(pb, b, nine) &&
       hexeq(pb, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"), "Bob's public key");
    const char *K = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742";
    ck(ssh_x25519(ka, a, pb) && hexeq(ka, 32, K), "Alice's shared secret");
    ck(ssh_x25519(kb, b, pa) && hexeq(kb, 32, K), "Bob's shared secret");

    // The SSH layer: two ephemeral keys agree, the secret comes out as the
    // X25519 bytes themselves (RFC 8731 3.1), and bad public values are refused.
    for (int alg = SSH_KEX_C25519; alg <= SSH_KEX_P256; alg++) {
        SshEcdh e1, e2;
        uint8_t q1[65], q2[65], k1[32], k2[32];
        uint32_t n1 = ssh_ecdh_start(&e1, alg, q1, sizeof(q1));
        uint32_t n2 = ssh_ecdh_start(&e2, alg, q2, sizeof(q2));
        ck(n1 == (alg == SSH_KEX_C25519 ? 32u : 65u) && n2 == n1, "ephemeral public key length");
        ck(ssh_ecdh_finish(&e1, q2, n2, k1) == 32 && ssh_ecdh_finish(&e2, q1, n1, k2) == 32 &&
           !memcmp(k1, k2, 32), "both sides agree");
        uint8_t shortq[64];
        memcpy(shortq, q2, 31);
        ck(ssh_ecdh_finish(&e1, shortq, n2 - 1, k1) == 0, "wrong-length public value refused");
        if (alg == SSH_KEX_P256) {
            uint8_t off[65]; memcpy(off, q2, 65); off[64] ^= 1;   // not on the curve
            ck(ssh_ecdh_finish(&e1, off, 65, k1) == 0, "a point off the curve is refused");
        } else {
            uint8_t zero[32] = { 0 };       // a low-order point: the secret would be all zero
            ck(ssh_ecdh_finish(&e1, zero, 32, k1) == 0, "an all-zero result is refused");
        }
        ssh_ecdh_free(&e1); ssh_ecdh_free(&e2);
    }
    // The ECDH secret for Curve25519 is the X25519 output, not the integer.
    SshEcdh e;
    uint8_t q[32], k[32];
    ssh_ecdh_start(&e, SSH_KEX_C25519, q, sizeof(q));
    uint8_t d_le[32];
    mbedtls_mpi_write_binary_le(&e.d, d_le, 32);
    ssh_ecdh_finish(&e, pb, 32, k);
    uint8_t want[32];
    ssh_x25519(want, d_le, pb);
    ck(!memcmp(k, want, 32), "the C25519 secret is the X25519 byte string");
    ssh_ecdh_free(&e);
}

// --- RFC 4253 section 7.2 ---------------------------------------------------------------------

static void mpint_into(Sha256Ctx *c, const uint8_t *k, uint32_t n) {
    while (n && !k[0]) { k++; n--; }
    bool pad = n && (k[0] & 0x80);
    uint8_t len[4] = { 0, 0, 0, (uint8_t)(n + pad) };
    sha256_update(c, len, 4);
    if (pad) { uint8_t z = 0; sha256_update(c, &z, 1); }
    sha256_update(c, k, n);
}

static void kdf_tests() {
    uint8_t K[32], H[32], sid[32];
    for (int i = 0; i < 32; i++) { K[i] = (uint8_t)(0x80 + i); H[i] = (uint8_t)i; sid[i] = (uint8_t)(0xA0 ^ i); }
    uint8_t got[64];
    ssh_kdf(K, 32, H, 'C', sid, got, 64);
    // K1 = HASH(K || H || "C" || session_id); K2 = HASH(K || H || K1).
    uint8_t k1[32], k2[32];
    Sha256Ctx c;
    sha256_init(&c); mpint_into(&c, K, 32); sha256_update(&c, H, 32);
    uint8_t x = 'C'; sha256_update(&c, &x, 1); sha256_update(&c, sid, 32); sha256_final(&c, k1);
    sha256_init(&c); mpint_into(&c, K, 32); sha256_update(&c, H, 32); sha256_update(&c, k1, 32);
    sha256_final(&c, k2);
    ck(!memcmp(got, k1, 32), "KDF first block (K has its top bit set: mpint pads)");
    ck(!memcmp(got + 32, k2, 32), "KDF extension block");
    uint8_t short16[16];
    ssh_kdf(K, 32, H, 'C', sid, short16, 16);
    ck(!memcmp(short16, k1, 16), "a short key is the front of K1");
}

// --- keys and signatures ------------------------------------------------------------------------

static void sig_tests() {
    static uint8_t key[600], data[200], sig[600];
    uint32_t kn = unhex(kRSA_KEY, key, sizeof(key));
    uint32_t dn = unhex(kRSA_DATA, data, sizeof(data));
    uint32_t sn = unhex(kRSA_SIG, sig, sizeof(sig));
    ck(!strcmp(kRSA_ALG, "rsa-sha2-512"), "fixture is rsa-sha2-512");
    ck(ssh_sig_verify(key, kn, sig, sn, data, dn, "rsa-sha2-512"), "OpenSSH's RSA signature verifies");
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "rsa-sha2-256"), "...not under another algorithm name");
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "ssh-rsa"), "...and never as SHA-1 ssh-rsa");
    data[dn - 1] ^= 1;
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "rsa-sha2-512"), "RSA: altered data fails");
    data[dn - 1] ^= 1;
    sig[sn - 1] ^= 1;
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "rsa-sha2-512"), "RSA: altered signature fails");

    kn = unhex(kEC_KEY, key, sizeof(key));
    dn = unhex(kEC_DATA, data, sizeof(data));
    sn = unhex(kEC_SIG, sig, sizeof(sig));
    ck(ssh_sig_verify(key, kn, sig, sn, data, dn, "ecdsa-sha2-nistp256"), "OpenSSH's ECDSA signature verifies");
    data[0] ^= 1;
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "ecdsa-sha2-nistp256"), "ECDSA: altered data fails");
    data[0] ^= 1;
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "rsa-sha2-256"), "ECDSA key, RSA algorithm: refused");

    // Ed25519 (RFC 8709), made by ssh-keygen -Y sign.
    kn = unhex(kED_KEY, key, sizeof(key));
    dn = unhex(kED_DATA, data, sizeof(data));
    sn = unhex(kED_SIG, sig, sizeof(sig));
    char kt[24];
    ck(ssh_key_type(key, kn, kt, sizeof(kt)) && !strcmp(kt, "ssh-ed25519"), "Ed25519 key type");
    ck(ssh_sig_verify(key, kn, sig, sn, data, dn, "ssh-ed25519"), "OpenSSH's Ed25519 signature verifies");
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "ecdsa-sha2-nistp256"), "...not under another algorithm name");
    data[dn - 1] ^= 1;
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "ssh-ed25519"), "Ed25519: altered data fails");
    data[dn - 1] ^= 1;
    sig[sn - 64] ^= 0x10;                                   // inside R
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "ssh-ed25519"), "Ed25519: altered R fails");
    sig[sn - 64] ^= 0x10;
    sig[sn - 32] ^= 0x01;                                   // the low byte of S
    ck(!ssh_sig_verify(key, kn, sig, sn, data, dn, "ssh-ed25519"), "Ed25519: altered S fails");
    sig[sn - 32] ^= 0x01;
    ck(ssh_sig_verify(key, kn, sig, sn, data, dn, "ssh-ed25519"), "...and the original still verifies");
    {
        // The blobs are exactly RFC 8709's shape: one string each after the
        // name, of exactly 32 and 64 bytes, and nothing after.
        uint8_t k2[64], s2[128];
        memcpy(k2, key, kn); k2[kn] = 0;
        ck(!ssh_sig_verify(k2, kn + 1, sig, sn, data, dn, "ssh-ed25519"), "Ed25519: a byte after the key");
        memcpy(s2, sig, sn); s2[sn] = 0;
        ck(!ssh_sig_verify(key, kn, s2, sn + 1, data, dn, "ssh-ed25519"), "Ed25519: a byte after the signature");
        memcpy(k2, key, kn); k2[kn - 33] = 31;              // the key's length says 31
        ck(!ssh_sig_verify(k2, kn - 1, sig, sn, data, dn, "ssh-ed25519"), "Ed25519: a 31-byte key");
        memcpy(s2, sig, sn); s2[sn - 65] = 63;              // the signature's length says 63
        ck(!ssh_sig_verify(key, kn, s2, sn - 1, data, dn, "ssh-ed25519"), "Ed25519: a 63-byte signature");
        // An ECDSA key named in an Ed25519 request, and the other way round.
        uint8_t ek[200]; uint32_t ekn = unhex(kEC_KEY, ek, sizeof(ek));
        ck(!ssh_sig_verify(ek, ekn, sig, sn, data, dn, "ssh-ed25519"), "ECDSA key, Ed25519 algorithm: refused");
        uint8_t es[200]; uint32_t esn = unhex(kEC_SIG, es, sizeof(es));
        ck(!ssh_sig_verify(key, kn, es, esn, data, dn, "ecdsa-sha2-nistp256"), "Ed25519 key, ECDSA signature: refused");
    }

    // Our own host key: sign, verify, and reload from the secret alone.
    SshHostKey hk, again;
    ck(ssh_hostkey_generate(&hk), "host key generated");
    uint8_t blob[160], s2[160];
    uint32_t bn = ssh_hostkey_blob(&hk, blob, sizeof(blob));
    uint32_t s2n = ssh_hostkey_sign(&hk, (const uint8_t *)"exchange hash", 13, s2, sizeof(s2));
    ck(bn == 104 && s2n > 0, "blob and signature");
    ck(ssh_sig_verify(blob, bn, s2, s2n, (const uint8_t *)"exchange hash", 13, "ecdsa-sha2-nistp256"),
       "our signature verifies");
    uint8_t s3[160];
    uint32_t s3n = ssh_hostkey_sign(&hk, (const uint8_t *)"exchange hash", 13, s3, sizeof(s3));
    ck(s3n == s2n && !memcmp(s2, s3, s2n), "signatures are deterministic (RFC 6979)");
    ck(ssh_hostkey_load(&again, hk.d) && !memcmp(again.q, hk.q, 65), "reload from the secret");
    uint8_t zero[32] = { 0 };
    ck(!ssh_hostkey_load(&again, zero), "a zero secret is refused");

    // Fingerprints, as ssh-keygen -l printed them.
    char fp[64];
    uint8_t b2[600];
    static const char kEcLine[] =
        "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBBMta+ixzbK25GWFtSUYwcBw17senHD3"
        "a68joevDcqKxRx9f3L3HhpE85vq8HuyDW41aXNo4ReahEu+BmBgi/UQ=";
    uint32_t n2 = ssh_b64_decode(kEcLine, (uint32_t)strlen(kEcLine), b2, sizeof(b2));
    ssh_fingerprint(b2, n2, fp, sizeof(fp));
    ck(!strcmp(fp, "SHA256:aAaO7xXPv7rSw893E3aHYjlB4JBaiHw06vfp15NIRvU"), "ECDSA fingerprint matches ssh-keygen");
    kn = unhex(kRSA_KEY, key, sizeof(key));
    ssh_fingerprint(key, kn, fp, sizeof(fp));
    ck(!strcmp(fp, "SHA256:NBxU8jCzwe5muefMkBnYfVp0EAJ1k1pJXuvz/N/2hsc"), "RSA fingerprint matches ssh-keygen");
    kn = unhex(kED_KEY, key, sizeof(key));
    ssh_fingerprint(key, kn, fp, sizeof(fp));
    ck(!strcmp(fp, kED_FP), "Ed25519 fingerprint matches ssh-keygen");
}

static void keyfile_tests() {
    SshHostKey hk; ssh_hostkey_generate(&hk);
    uint8_t blob[160]; uint32_t bn = ssh_hostkey_blob(&hk, blob, sizeof(blob));
    char line[400];
    uint32_t ln = ssh_pubkey_line(blob, bn, "me@device", line, sizeof(line));
    ck(ln > 0 && !strncmp(line, "ecdsa-sha2-nistp256 AAAA", 24), "public key line");

    char text[2048];
    snprintf(text, sizeof(text), "# keys\r\n\n%s\r\n", line);
    ck(ssh_authkeys_match(text, (uint32_t)strlen(text), blob, bn), "listed key found (CRLF, comments)");
    snprintf(text, sizeof(text), "no-pty %s\n", line);
    ck(!ssh_authkeys_match(text, (uint32_t)strlen(text), blob, bn), "a line with options is not honoured");
    snprintf(text, sizeof(text), "command=\"ls\" %s\n", line);
    ck(!ssh_authkeys_match(text, (uint32_t)strlen(text), blob, bn), "command= is not honoured either");
    // The word in front says RSA, the blob says ECDSA: refused.
    snprintf(text, sizeof(text), "ssh-rsa %s\n", line + 20);
    ck(!ssh_authkeys_match(text, (uint32_t)strlen(text), blob, bn), "type word must match the blob");
    snprintf(text, sizeof(text), "%.*s\n", (int)ln - 12, line);    // truncated base64
    ck(!ssh_authkeys_match(text, (uint32_t)strlen(text), blob, bn), "truncated key does not match");

    // Ed25519: the line ssh-keygen wrote, found by the blob inside it; and
    // the type word in front must agree with the blob in both directions.
    uint8_t ed[64];
    uint32_t edn = unhex(kED_KEY, ed, sizeof(ed));
    snprintf(text, sizeof(text), "%s\nssh-ed25519 %s fixture\n", line, kED_LINE);
    ck(ssh_authkeys_match(text, (uint32_t)strlen(text), ed, edn), "an ssh-ed25519 line is found");
    ck(ssh_authkeys_match(text, (uint32_t)strlen(text), blob, bn), "...beside an ECDSA one");
    snprintf(text, sizeof(text), "ecdsa-sha2-nistp256 %s fixture\n", kED_LINE);
    ck(!ssh_authkeys_match(text, (uint32_t)strlen(text), ed, edn), "an Ed25519 blob under an ECDSA type word");
    snprintf(text, sizeof(text), "ssh-ed25519 %s\n", line + 20);
    ck(!ssh_authkeys_match(text, (uint32_t)strlen(text), blob, bn), "an ECDSA blob under the ssh-ed25519 word");
    snprintf(text, sizeof(text), "no-pty ssh-ed25519 %s\n", kED_LINE);
    ck(!ssh_authkeys_match(text, (uint32_t)strlen(text), ed, edn), "an Ed25519 line with options is not honoured");
    char edline[200], want[200];
    uint32_t eln = ssh_pubkey_line(ed, edn, "c", edline, sizeof(edline));
    snprintf(want, sizeof(want), "ssh-ed25519 %s c", kED_LINE);
    ck(eln > 0 && !strcmp(edline, want), "Ed25519 public key line is ssh-keygen's");

    char kh[2048];
    // known_hosts: an Ed25519 host key recorded, then a different one.
    uint32_t kl = ssh_known_hosts_line("ed.host", 22, ed, edn, kh, sizeof(kh));
    ck(kl > 0 && ssh_known_hosts_check(kh, kl, "ed.host", 22, ed, edn) == SSH_KH_MATCH, "known Ed25519 host");
    uint8_t ed2[64];
    memcpy(ed2, ed, edn);
    ed2[edn - 1] ^= 1;
    ck(ssh_known_hosts_check(kh, kl, "ed.host", 22, ed2, edn) == SSH_KH_CHANGED, "changed Ed25519 key detected");
    ck(ssh_known_hosts_check(kh, kl, "ed.host", 22, blob, bn) == SSH_KH_UNKNOWN,
       "an ECDSA key for a host known by its Ed25519 key is new, not changed");

    kl = ssh_known_hosts_line("Device.local", 2222, blob, bn, kh, sizeof(kh));
    ck(kl > 0 && !strncmp(kh, "[Device.local]:2222 ecdsa", 25) && kh[kl - 1] == '\n', "known_hosts line");
    ck(ssh_known_hosts_check(kh, kl, "device.local", 2222, blob, bn) == SSH_KH_MATCH, "known host (case-insensitive)");
    ck(ssh_known_hosts_check(kh, kl, "device.local", 22, blob, bn) == SSH_KH_UNKNOWN, "other port is another host");
    SshHostKey other; ssh_hostkey_generate(&other);
    uint8_t ob[160]; uint32_t obn = ssh_hostkey_blob(&other, ob, sizeof(ob));
    ck(ssh_known_hosts_check(kh, kl, "device.local", 2222, ob, obn) == SSH_KH_CHANGED, "changed key detected");
    kl = ssh_known_hosts_line("10.0.0.5", 22, blob, bn, kh, sizeof(kh));
    ck(!strncmp(kh, "10.0.0.5 ", 9), "port 22 is the bare name");
    char multi[2048];
    snprintf(multi, sizeof(multi), "|1|abc= %s\nalias,10.0.0.5 %s", kh + 9, kh + 9);
    ck(ssh_known_hosts_check(multi, (uint32_t)strlen(multi), "10.0.0.5", 22, blob, bn) == SSH_KH_MATCH,
       "comma-separated hosts; hashed lines skipped");
}

// --- the packet ciphers ---------------------------------------------------------------------------

static void cipher_tests() {
    struct { int alg, mac; const char *name; } modes[] = {
        { SSH_AES128_GCM, SSH_MAC_NONE, "aes128-gcm" }, { SSH_AES256_GCM, SSH_MAC_NONE, "aes256-gcm" },
        { SSH_AES128_CTR, SSH_MAC_HMAC256, "aes128-ctr+hmac" }, { SSH_AES256_CTR, SSH_MAC_HMAC256_ETM, "aes256-ctr+etm" },
    };
    uint8_t key[32], iv[16], mk[32];
    for (int i = 0; i < 32; i++) { key[i] = (uint8_t)i; mk[i] = (uint8_t)(0x55 ^ i); }
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(0xF0 + i);
    for (auto &m : modes) {
        SshCipher tx, rx;
        ssh_cipher_init(&tx); ssh_cipher_init(&rx);
        ck(ssh_cipher_set(&tx, m.alg, m.mac, key, iv, mk) && ssh_cipher_set(&rx, m.alg, m.mac, key, iv, mk), m.name);
        for (uint32_t seq = 7; seq < 10; seq++) {
            uint8_t pkt[128], plain[128];
            uint32_t len = 32;
            pkt[0] = 0; pkt[1] = 0; pkt[2] = 0; pkt[3] = (uint8_t)len;
            for (uint32_t i = 4; i < 4 + len; i++) pkt[i] = (uint8_t)(i * 7 + seq);
            memcpy(plain, pkt, 4 + len);
            uint32_t tag = ssh_cipher_seal(&tx, seq, pkt, len);
            ck(tag == ssh_cipher_taglen(m.alg, m.mac), "tag length");
            ck(memcmp(pkt + 4, plain + 4, len) != 0, "body is encrypted");
            bool plain_len = ssh_cipher_len_plain(m.alg, m.mac);
            ck(plain_len == !memcmp(pkt, plain, 4), "length clear exactly when the mode says so");
            if (m.mac != SSH_MAC_NONE) {
                // The MAC, recomputed with the OS's own HMAC over what the RFC
                // says it covers: the plaintext for encrypt-and-MAC, the
                // ciphertext for -etm, each after the sequence number.
                uint8_t in[4 + 4 + 64], want[32];
                in[0] = (uint8_t)(seq >> 24); in[1] = (uint8_t)(seq >> 16); in[2] = (uint8_t)(seq >> 8); in[3] = (uint8_t)seq;
                memcpy(in + 4, m.mac == SSH_MAC_HMAC256 ? plain : pkt, 4 + len);
                hmac_sha256(mk, 32, in, 8 + len, want);
                ck(!memcmp(pkt + 4 + len, want, 32), "MAC covers the right bytes");
            }
            uint8_t copy[128];
            memcpy(copy, pkt, 4 + len + tag);
            if (!plain_len) ssh_cipher_open_first(&rx, pkt);
            ck(ssh_cipher_open(&rx, seq, pkt, len) && !memcmp(pkt, plain, 4 + len), "round trip");
            // A flipped bit is caught, by a fresh receiver standing where the
            // real one stood before this packet (the first one only, where
            // that is the start of the stream). And the right packet under
            // the wrong sequence number fails too, for the MAC modes.
            if (seq == 7) {
                SshCipher rx2; ssh_cipher_init(&rx2);
                ssh_cipher_set(&rx2, m.alg, m.mac, key, iv, mk);
                uint8_t bent[128];
                memcpy(bent, copy, sizeof(bent));
                bent[10] ^= 0x40;
                if (!plain_len) ssh_cipher_open_first(&rx2, bent);
                ck(!ssh_cipher_open(&rx2, seq, bent, len), "tampering detected");
                ssh_cipher_free(&rx2);
                if (m.mac != SSH_MAC_NONE) {
                    ssh_cipher_init(&rx2);
                    ssh_cipher_set(&rx2, m.alg, m.mac, key, iv, mk);
                    memcpy(bent, copy, sizeof(bent));
                    if (!plain_len) ssh_cipher_open_first(&rx2, bent);
                    ck(!ssh_cipher_open(&rx2, seq + 1, bent, len), "wrong sequence number detected");
                    ssh_cipher_free(&rx2);
                }
            }
        }
        ssh_cipher_free(&tx); ssh_cipher_free(&rx);
    }
}

// --- client and server in memory -------------------------------------------------------------------

struct Srv {
    SshServer s;
    bool started; uint32_t got; char exec[256]; bool closed;
    const char *pw;
};
static bool sv_user_ok(void *, const char *u) { return !strcmp(u, "alice"); }
static bool sv_password(void *ctx, const char *, const char *pw) { return !strcmp(pw, ((Srv *)ctx)->pw); }
static SshHostKey g_userkey;
static bool sv_pubkey(void *, const char *, const uint8_t *blob, uint32_t n) {
    uint8_t b[160]; uint32_t bn = ssh_hostkey_blob(&g_userkey, b, sizeof(b));
    return bn == n && !memcmp(b, blob, n);
}
static void sv_start(void *ctx) { Srv *v = (Srv *)ctx; v->started = true; snprintf(v->exec, sizeof(v->exec), "%s", v->s.exec); }
static void sv_data(void *ctx, const uint8_t *, uint32_t n) { Srv *v = (Srv *)ctx; v->got += n; ssh_srv_consumed(&v->s, n); }
static void sv_close(void *ctx) { ((Srv *)ctx)->closed = true; }
static const SshServerOps kSvOps = { sv_user_ok, sv_password, sv_pubkey, nullptr, sv_start, sv_data,
                                     nullptr, nullptr, nullptr, sv_close };

struct Cli { SshClient k; uint32_t got; const char *pw; bool hk_ok; int prompts; };
static bool cl_hostkey(void *ctx, const uint8_t *, uint32_t) { return ((Cli *)ctx)->hk_ok; }
static bool cl_prompt(void *ctx, const char *, bool, char *out, uint32_t cap) {
    Cli *c = (Cli *)ctx; c->prompts++;
    if (!c->pw) return false;
    snprintf(out, cap, "%s", c->pw); return true;
}
static void cl_data(void *ctx, const uint8_t *, uint32_t n, bool) { ((Cli *)ctx)->got += n; }
static const SshClientOps kClOps = { cl_hostkey, cl_prompt, nullptr, cl_data };

// Optional tap on the client->server bytes, for injecting a packet.
typedef uint32_t (*Tap)(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, void *st);

// Bytes from one end to the other, as many as the receiver will take now. A
// tap rewrites them on the way, which only ever happens to a small first
// flight that the receiver takes whole.
static void move(SshConn *from, SshConn *to, Tap tap, void *st) {
    uint32_t n0;
    const uint8_t *p = ssh_out(from, &n0);
    if (!n0) return;
    if (tap) {
        static uint8_t tmp[16384];
        uint32_t n = tap(p, n0, tmp, sizeof(tmp), st);
        uint32_t at = 0;
        while (at < n && !ssh_closed(to)) { at += ssh_feed(to, tmp + at, n - at); ssh_process(to); }
        ssh_out_consume(from, n0);
        return;
    }
    uint32_t took = ssh_feed(to, p, n0);
    ssh_out_consume(from, took);
    ssh_process(to);
}

static void pump(Srv *sv, Cli *cl, int rounds, Tap tap = nullptr, void *st = nullptr) {
    for (int i = 0; i < rounds; i++) {
        ssh_process(&cl->k.c);
        ssh_process(&sv->s.c);
        move(&cl->k.c, &sv->s.c, tap, st);
        move(&sv->s.c, &cl->k.c, nullptr, nullptr);
        if (ssh_closed(&cl->k.c) && ssh_closed(&sv->s.c)) break;
    }
}

static void setup(Srv *sv, Cli *cl, const char *srv_pw, const char *cli_pw, const char *cmd) {
    static SshHostKey hk;
    if (!hk.ok) ssh_hostkey_generate(&hk);
    memset(sv, 0, sizeof(*sv)); memset(cl, 0, sizeof(*cl));
    sv->pw = srv_pw;
    ssh_srv_init(&sv->s, &hk, &kSvOps, sv);
    ssh_cli_init(&cl->k, "alice", &kClOps, cl);
    cl->pw = cli_pw; cl->hk_ok = true;
    cl->k.command = cmd;
}

static void session_tests() {
    Srv sv; Cli cl;

    // A whole login by password, a command, output both ways, the exit status.
    setup(&sv, &cl, "pw", "pw", "do-it");
    pump(&sv, &cl, 40);
    ck(sv.s.authed && sv.started && !strcmp(sv.exec, "do-it"), "password login and exec");
    ck(sv.s.c.strict && cl.k.c.strict, "strict KEX agreed between our own two ends");
    static uint8_t big[20000];
    for (uint32_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
    uint32_t sent = 0;
    for (int i = 0; i < 400 && sent < sizeof(big); i++) {
        sent += ssh_srv_send(&sv.s, big + sent, sizeof(big) - sent);
        pump(&sv, &cl, 1);
    }
    pump(&sv, &cl, 10);
    ck(sent == sizeof(big) && cl.got == sizeof(big), "20 KB server->client across window adjustments");
    // Re-key started by the server, then by the client, with data in between.
    ssh_rekey(&sv.s.c);
    uint8_t chunk[600] = { 1 };
    uint32_t before = cl.got;
    for (int i = 0; i < 20; i++) { ssh_srv_send(&sv.s, chunk, sizeof(chunk)); pump(&sv, &cl, 1); }
    ck(!ssh_closed(&sv.s.c) && !sv.s.c.kex_running && cl.got > before, "server-initiated re-key");
    ssh_rekey(&cl.k.c);
    for (int i = 0; i < 20; i++) { ssh_cli_send(&cl.k, chunk, 100); pump(&sv, &cl, 1); }
    ck(!ssh_closed(&cl.k.c) && !cl.k.c.kex_running && sv.got >= 100, "client-initiated re-key");
    ck(!memcmp(cl.k.c.session_id, sv.s.c.session_id, 32), "session id survives re-keying");
    ssh_srv_exit(&sv.s, 42);
    pump(&sv, &cl, 20);
    ck(cl.k.have_status && cl.k.exit_status == 42, "exit status delivered");
    ck(ssh_closed(&cl.k.c), "client finished");
    ssh_srv_free(&sv.s); ssh_cli_free(&cl.k);

    // Public key login with the device's own key.
    ssh_hostkey_generate(&g_userkey);
    setup(&sv, &cl, "nope", nullptr, "k");
    cl.k.idkey = &g_userkey;
    pump(&sv, &cl, 40);
    ck(sv.s.authed && cl.prompts == 0, "publickey login, no password asked");
    ssh_srv_free(&sv.s); ssh_cli_free(&cl.k);

    // Wrong passwords: three prompts, then the client gives up.
    setup(&sv, &cl, "right", "wrong", "k");
    pump(&sv, &cl, 60);
    ck(!sv.s.authed && cl.prompts == SSH_CLI_PROMPTS && sv.s.fails == SSH_CLI_PROMPTS, "client stops after three");
    ck(sv.s.penalty_ms > 0, "a wrong password sets a penalty");
    ssh_srv_free(&sv.s); ssh_cli_free(&cl.k);

    // The host key check is honoured: refused means no password is sent.
    setup(&sv, &cl, "pw", "pw", "k");
    cl.hk_ok = false;
    pump(&sv, &cl, 40);
    ck(ssh_closed(&cl.k.c) && cl.prompts == 0 && !sv.s.authed, "refused host key: nothing sent");
    ssh_srv_free(&sv.s); ssh_cli_free(&cl.k);
}

// The server's half of an ssh-ed25519 login, message by message, on a
// connection forced past the key exchange as the fuzzer does it: nothing is
// encrypted, so what the server queued can be read back as it is.
static bool ed_pubkey(void *, const char *, const uint8_t *blob, uint32_t n) {
    uint8_t b[64]; uint32_t bn = unhex(kED_UA_BLOB, b, sizeof(b));
    return n == bn && !memcmp(blob, b, n);
}
static const SshServerOps kEdOps = { sv_user_ok, sv_password, ed_pubkey, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr };

static void ed_server(SshServer *s) {
    static SshHostKey hk;
    if (!hk.ok) ssh_hostkey_generate(&hk);
    ssh_srv_init(s, &hk, &kEdOps, nullptr);
    s->c.state = SSH_ST_OPEN;
    s->c.first_kex_done = true;
    s->c.have_session_id = true;
    for (int i = 0; i < 32; i++) s->c.session_id[i] = (uint8_t)i;
    s->service_ok = true;
}

// The type of every packet queued since the last request, and the payload
// (after the type) of the last one.
static int queued(SshConn *c, uint8_t *types, int cap, const uint8_t **last, uint32_t *last_n) {
    uint32_t n; const uint8_t *p = ssh_out(c, &n);
    int k = 0;
    uint32_t at = 0;
    while (at + 6 <= n && k < cap) {
        uint32_t len = ((uint32_t)p[at] << 24) | ((uint32_t)p[at + 1] << 16) |
                       ((uint32_t)p[at + 2] << 8) | p[at + 3];
        types[k++] = p[at + 5];
        if (last) { *last = p + at + 6; *last_n = len - 2 - p[at + 4]; }
        at += 4 + len;
    }
    return k;
}

static void ed_request(SshServer *s, const char *alg, const uint8_t *blob, uint32_t bn,
                       const uint8_t *sig, uint32_t sn) {
    uint8_t buf[512];
    SshW w; sshw_init(&w, buf, sizeof(buf));
    sshw_cstr(&w, "alice");
    sshw_cstr(&w, "ssh-connection");
    sshw_cstr(&w, "publickey");
    sshw_bool(&w, sig != nullptr);
    sshw_cstr(&w, alg);
    sshw_string(&w, blob, bn);
    if (sig) sshw_string(&w, sig, sn);
    uint32_t n; ssh_out(&s->c, &n); ssh_out_consume(&s->c, n);
    SshR r; sshr_init(&r, buf, w.len);
    s->c.dispatch(&s->c, SSH_MSG_USERAUTH_REQUEST, &r);
}

static void ed25519_login_tests() {
    uint8_t blob[64], sig[128];
    uint32_t bn = unhex(kED_UA_BLOB, blob, sizeof(blob));
    uint32_t sn = unhex(kED_UA_SIG, sig, sizeof(sig));
    uint8_t types[8];
    const uint8_t *last = nullptr; uint32_t ln = 0;
    SshServer s;

    // The query form: no signature, and the answer is PK_OK naming the
    // algorithm and the key back (RFC 4252 section 7).
    ed_server(&s);
    ed_request(&s, "ssh-ed25519", blob, bn, nullptr, 0);
    int k = queued(&s.c, types, 8, &last, &ln);
    bool echoed = false;
    if (k == 1 && last) {
        SshR r; sshr_init(&r, last, ln);
        char alg[32]; const uint8_t *eb; uint32_t ebn;
        echoed = sshr_cstr(&r, alg, sizeof(alg)) && sshr_string(&r, &eb, &ebn) && sshr_left(&r) == 0 &&
                 !strcmp(alg, "ssh-ed25519") && ebn == bn && !memcmp(eb, blob, bn);
    }
    ck(k == 1 && types[0] == SSH_MSG_USERAUTH_PK_OK && echoed, "Ed25519 query answered with PK_OK");
    ck(!s.authed && s.fails == 0, "...and a query is neither a login nor a failure");

    // Signed: logged in.
    ed_request(&s, "ssh-ed25519", blob, bn, sig, sn);
    k = queued(&s.c, types, 8, nullptr, nullptr);
    ck(k == 1 && types[0] == SSH_MSG_USERAUTH_SUCCESS && s.authed && !strcmp(s.user, "alice"),
       "an Ed25519 signature logs in");
    ssh_srv_free(&s);

    // The same request on another session: the signature covers the session
    // id, so it does not carry over.
    ed_server(&s);
    s.c.session_id[31] ^= 1;
    ed_request(&s, "ssh-ed25519", blob, bn, sig, sn);
    k = queued(&s.c, types, 8, nullptr, nullptr);
    ck(k == 1 && types[0] == SSH_MSG_USERAUTH_FAILURE && !s.authed && s.fails == 1 && s.penalty_ms > 0,
       "an Ed25519 signature from another session is refused, and counts");
    ssh_srv_free(&s);

    // One bit of S changed.
    ed_server(&s);
    sig[sn - 32] ^= 0x01;
    ed_request(&s, "ssh-ed25519", blob, bn, sig, sn);
    sig[sn - 32] ^= 0x01;
    ck(!s.authed && s.fails == 1, "a damaged Ed25519 signature is refused");
    ssh_srv_free(&s);

    // The key under another algorithm's name, as a query: refused, no PK_OK.
    ed_server(&s);
    ed_request(&s, "ecdsa-sha2-nistp256", blob, bn, nullptr, 0);
    k = queued(&s.c, types, 8, nullptr, nullptr);
    ck(k == 1 && types[0] == SSH_MSG_USERAUTH_FAILURE && s.fails == 1,
       "an Ed25519 key under the ECDSA name is refused");
    ssh_srv_free(&s);
}

// The host key algorithms the device's own ssh offers. sshc.cpp never sets
// hostkey_algs, so this default is what every connection from the device
// asks for: ssh-ed25519 has to be in it, and last, so a host recorded under
// its ECDSA or RSA key goes on presenting that one. (The interop suite asks
// for ssh-ed25519 explicitly, which does not test this list.)
static void client_offer_test() {
    SshClient k;
    ssh_cli_init(&k, "alice", &kClOps, nullptr);
    ssh_process(&k.c);
    uint32_t n; const uint8_t *p = ssh_out(&k.c, &n);
    const uint8_t *nl = (const uint8_t *)memchr(p, '\n', n);
    bool ok = false;
    if (nl) {
        // After the version line: length, padding length, type, 16-byte
        // cookie, then the kex and host key name-lists (RFC 4253 7.1).
        uint32_t at = (uint32_t)(nl - p) + 1;
        if (at + 22 <= n && p[at + 5] == SSH_MSG_KEXINIT) {
            SshR r; sshr_init(&r, p + at + 22, n - at - 22);
            const uint8_t *kx, *hk; uint32_t kxn, hkn;
            static const char last[] = ",ssh-ed25519";
            const uint32_t ln = sizeof(last) - 1;
            ok = sshr_string(&r, &kx, &kxn) && sshr_string(&r, &hk, &hkn) &&
                 hkn > ln && !memcmp(hk + hkn - ln, last, ln) &&
                 ssh_namelist_has(hk, hkn, "ecdsa-sha2-nistp256");
        }
    }
    ck(ok, "the device's ssh offers ssh-ed25519 host keys, after ECDSA and RSA");
    ssh_cli_free(&k);
}

// Terrapin: an IGNORE slipped in ahead of the client's KEXINIT. With strict
// KEX the server must refuse the connection.
struct Inject { bool done; };
static uint32_t inject_ignore(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap, void *st) {
    Inject *j = (Inject *)st;
    memcpy(out, in, n);
    if (j->done) return n;
    const uint8_t *nl = (const uint8_t *)memchr(in, '\n', n);
    if (!nl) return n;
    uint32_t vlen = (uint32_t)(nl - in) + 1;
    // An unencrypted IGNORE: len 12, pad 10, type 2, empty string... sized to
    // the 8-byte block: 4 + 12 = 16.
    uint8_t ign[16] = { 0, 0, 0, 12, 6, SSH_MSG_IGNORE, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6 };
    memcpy(out, in, vlen);
    memcpy(out + vlen, ign, sizeof(ign));
    memcpy(out + vlen + sizeof(ign), in + vlen, n - vlen);
    j->done = true;
    (void)cap;
    return n + sizeof(ign);
}

static void hostile_tests() {
    Srv sv; Cli cl;
    setup(&sv, &cl, "pw", "pw", "k");
    Inject j{ false };
    pump(&sv, &cl, 20, inject_ignore, &j);
    ck(j.done && ssh_closed(&sv.s.c) && !sv.s.authed, "Terrapin injection refused under strict KEX");
    ck(strstr(sv.s.c.why, "first packet") != nullptr, "...for the right reason");
    ssh_srv_free(&sv.s); ssh_cli_free(&cl.k);

    // A service message before the key exchange is over: refused.
    static SshHostKey hk; ssh_hostkey_generate(&hk);
    SshServer s;
    ssh_srv_init(&s, &hk, &kSvOps, nullptr);
    ssh_process(&s.c);
    const char *early = "SSH-2.0-test\r\n";
    ssh_feed(&s.c, (const uint8_t *)early, (uint32_t)strlen(early));
    uint8_t svc[32] = { 0, 0, 0, 28, 4, SSH_MSG_SERVICE_REQUEST, 0, 0, 0, 12 };
    memcpy(svc + 10, "ssh-userauth", 12);
    memset(svc + 22, 0, 4);
    ssh_feed(&s.c, svc, 32);
    ssh_process(&s.c);
    ck(ssh_closed(&s.c), "service request before encryption refused");
    ssh_srv_free(&s);

    // Not SSH-2.
    ssh_srv_init(&s, &hk, &kSvOps, nullptr);
    ssh_feed(&s.c, (const uint8_t *)"SSH-1.5-old\r\n", 13);
    ssh_process(&s.c);
    ck(ssh_closed(&s.c) && s.c.disc_reason == SSH_DISC_VERSION_NOT_SUPPORTED, "SSH-1 refused");
    ssh_srv_free(&s);
    // A server must not accept banner lines from a client.
    ssh_srv_init(&s, &hk, &kSvOps, nullptr);
    ssh_feed(&s.c, (const uint8_t *)"hello\r\nSSH-2.0-x\r\n", 18);
    ssh_process(&s.c);
    ck(ssh_closed(&s.c), "server refuses pre-version lines");
    ssh_srv_free(&s);
    // An absurd packet length is refused at once rather than waited for.
    ssh_srv_init(&s, &hk, &kSvOps, nullptr);
    ssh_feed(&s.c, (const uint8_t *)"SSH-2.0-x\r\n", 11);
    uint8_t hugelen[8] = { 0x7f, 0xff, 0xff, 0xf0, 0, 0, 0, 0 };
    ssh_feed(&s.c, hugelen, 8);
    ssh_process(&s.c);
    ck(ssh_closed(&s.c), "huge packet length refused");
    ssh_srv_free(&s);
}

// More than the window allows ends the connection. The client core respects
// the window, so it is told the window is bigger than it is.
static void window_test() {
    Srv sv; Cli cl;
    setup(&sv, &cl, "pw", "pw", "k");
    pump(&sv, &cl, 40);
    // Stop the server reopening the window so the second packet overruns it.
    static SshServerOps noconsume = kSvOps;
    noconsume.on_data = [](void *ctx, const uint8_t *, uint32_t n) { ((Srv *)ctx)->got += n; };
    sv.s.ops = &noconsume;
    cl.k.peer_window = 1u << 20;
    static uint8_t blob[SSH_SRV_WINDOW];
    ssh_cli_send(&cl.k, blob, 1024);
    pump(&sv, &cl, 2);
    ssh_cli_send(&cl.k, blob, 1024);
    pump(&sv, &cl, 2);
    ck(!ssh_closed(&sv.s.c), "exactly the window is fine");
    ssh_cli_send(&cl.k, blob, 1);
    pump(&sv, &cl, 2);
    ck(ssh_closed(&sv.s.c), "one byte past the window ends the connection");
    ssh_srv_free(&sv.s); ssh_cli_free(&cl.k);
}

// --- the session rings ------------------------------------------------------------------------------

static void sess_tests() {
    static SshSess s;
    ssh_sess_init(&s, true);
    const char *t = "a\nb\r\nc";
    uint32_t n = ssh_sess_write(&s, t, (uint32_t)strlen(t));
    uint8_t out[32]; uint32_t g = ssh_ring_get(&s.out, out, sizeof(out));
    ck(n == strlen(t) && g == 7 && !memcmp(out, "a\r\nb\r\nc", 7), "LF becomes CR LF; CR LF stays");
    // Fill the ring to one byte short: a \n must not be split into a lone \r.
    static char fill[SSH_SESS_OUT];
    memset(fill, 'x', sizeof(fill));
    ssh_sess_write(&s, fill, SSH_SESS_OUT - 1);
    n = ssh_sess_write(&s, "\n", 1);
    ck(n == 0 && ssh_ring_used(&s.out) == SSH_SESS_OUT - 1, "a line ending is never split");
    static uint8_t drain[SSH_SESS_OUT];
    ssh_ring_get(&s.out, drain, sizeof(drain));
    ck(ssh_sess_write(&s, "\n", 1) == 1, "and goes once there is room");

    // Ctrl+C: a byte at the prompt, an interrupt while a command runs.
    ssh_sess_init(&s, true);
    uint32_t dropped = ssh_sess_input(&s, (const uint8_t *)"ab\x03", 3);
    ck(dropped == 0 && !s.intr && ssh_ring_used(&s.in) == 3, "Ctrl+C at the prompt is a byte");
    ssh_ring_get(&s.in, out, 3);
    s.running = true;
    dropped = ssh_sess_input(&s, (const uint8_t *)"x\x03y", 3);
    ck(dropped == 1 && s.intr && ssh_ring_used(&s.in) == 2, "Ctrl+C during a command interrupts it");
    ck(ssh_sess_getch(&s) == 'x' && ssh_sess_getch(&s) == 'y' && s.consumed == 2, "type-ahead kept, counted");
    // Without a terminal, bytes go out as they are.
    ssh_sess_init(&s, false);
    ssh_sess_write(&s, "a\nb", 3);
    g = ssh_ring_get(&s.out, out, sizeof(out));
    ck(g == 3 && !memcmp(out, "a\nb", 3), "no pty: no CR added");
}

int main(void) {
    wire_tests();
    x25519_tests();
    kdf_tests();
    sig_tests();
    keyfile_tests();
    cipher_tests();
    session_tests();
    ed25519_login_tests();
    client_offer_test();
    hostile_tests();
    window_test();
    sess_tests();
    printf("  ssh: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
