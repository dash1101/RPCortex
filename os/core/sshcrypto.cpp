#include "sshcrypto.h"
#include "sshwire.h"
#include "ed25519.h"
#include "lock.h"

#include <string.h>
#include <stdlib.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/rsa.h"
#include "mbedtls/sha512.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

extern "C" int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

// --- randomness ----------------------------------------------------------------

static mbedtls_ctr_drbg_context g_drbg;
static bool    g_drbg_ok;
static RpcLock g_drbg_lock;

// The seed comes from the hardware source in EIGHT-BYTE pieces, never more.
//
// The SDK's mbedtls_hardware_poll copies MIN(len, 8) bytes per round to
// output + olen, measuring against the TOTAL length rather than what is left,
// so a request that is not a multiple of eight writes past the end of the
// buffer by up to seven bytes. mbedTLS's own entropy module happens to ask in
// multiples of eight; the DRBG asks for whatever its entropy length is. One
// word at a time keeps the overrun impossible whatever either side asks for.
static int entropy_cb(void *, unsigned char *out, size_t n) {
    while (n) {
        unsigned char w[8];
        size_t got = 0;
        if (mbedtls_hardware_poll(nullptr, w, sizeof(w), &got) != 0 || got != sizeof(w))
            return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
        size_t take = n < sizeof(w) ? n : sizeof(w);
        memcpy(out, w, take);
        mbedtls_platform_zeroize(w, sizeof(w));
        out += take; n -= take;
    }
    return 0;
}

int ssh_rng(void *, unsigned char *out, size_t n) {
    LockGuard g(&g_drbg_lock);
    if (!g_drbg_ok) {
        mbedtls_ctr_drbg_init(&g_drbg);
        static const unsigned char pers[] = "rpcortex-ssh";
        if (mbedtls_ctr_drbg_seed(&g_drbg, entropy_cb, nullptr, pers, sizeof(pers) - 1) != 0) {
            mbedtls_ctr_drbg_free(&g_drbg);
            return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
        }
        g_drbg_ok = true;
    }
    // The DRBG hands out at most MBEDTLS_CTR_DRBG_MAX_REQUEST bytes per call.
    while (n) {
        size_t take = n > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : n;
        int rc = mbedtls_ctr_drbg_random(&g_drbg, out, take);
        if (rc) return rc;
        out += take; n -= take;
    }
    return 0;
}

bool ssh_random(void *out, size_t n) { return ssh_rng(nullptr, (unsigned char *)out, n) == 0; }

void ssh_wipe(void *p, size_t n) { if (p && n) mbedtls_platform_zeroize(p, n); }

bool ssh_ct_equal(const void *a, const void *b, size_t n) {
    const volatile uint8_t *x = (const volatile uint8_t *)a;
    const volatile uint8_t *y = (const volatile uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

// --- hashing ---------------------------------------------------------------------

void ssh_hash_init(SshHash *h) {
    mbedtls_sha256_init(&h->ctx);
    mbedtls_sha256_starts(&h->ctx, 0);
}
void ssh_hash_raw(SshHash *h, const void *data, uint32_t n) {
    if (n) mbedtls_sha256_update(&h->ctx, (const unsigned char *)data, n);
}
void ssh_hash_u32(SshHash *h, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    ssh_hash_raw(h, b, 4);
}
void ssh_hash_string(SshHash *h, const void *data, uint32_t n) {
    ssh_hash_u32(h, n);
    ssh_hash_raw(h, data, n);
}
void ssh_hash_mpint(SshHash *h, const uint8_t *be, uint32_t n) {
    while (n && be[0] == 0) { be++; n--; }
    bool pad = n && (be[0] & 0x80);
    ssh_hash_u32(h, n + (pad ? 1 : 0));
    if (pad) { uint8_t z = 0; ssh_hash_raw(h, &z, 1); }
    ssh_hash_raw(h, be, n);
}
void ssh_hash_final(SshHash *h, uint8_t out[32]) {
    mbedtls_sha256_finish(&h->ctx, out);
    mbedtls_sha256_free(&h->ctx);
}

void ssh_sha256(const void *data, uint32_t n, uint8_t out[32]) {
    mbedtls_sha256((const unsigned char *)data, n, out, 0);
}

void ssh_kdf(const uint8_t *k, uint32_t kn, const uint8_t h[32], char letter,
             const uint8_t session_id[32], uint8_t *out, uint32_t outn) {
    uint8_t block[32];
    uint32_t have = 0;
    // K1 = HASH(K || H || X || session_id)
    SshHash s;
    ssh_hash_init(&s);
    ssh_hash_mpint(&s, k, kn);
    ssh_hash_raw(&s, h, 32);
    uint8_t x = (uint8_t)letter;
    ssh_hash_raw(&s, &x, 1);
    ssh_hash_raw(&s, session_id, 32);
    ssh_hash_final(&s, block);
    uint32_t take = outn < 32 ? outn : 32;
    memcpy(out, block, take);
    have = take;
    // Kn = HASH(K || H || K1 || ... || Kn-1), over the key AS BUILT SO FAR. Never
    // reached by the ciphers offered today (32 bytes is the most any of them
    // needs), and kept because the day a longer key is added is not the day to
    // remember this rule.
    while (have < outn) {
        ssh_hash_init(&s);
        ssh_hash_mpint(&s, k, kn);
        ssh_hash_raw(&s, h, 32);
        ssh_hash_raw(&s, out, have);
        ssh_hash_final(&s, block);
        take = outn - have < 32 ? outn - have : 32;
        memcpy(out + have, block, take);
        have += take;
    }
    ssh_wipe(block, sizeof(block));
}

// --- key exchange ----------------------------------------------------------------

static mbedtls_ecp_group_id group_of(int alg) {
    return alg == SSH_KEX_C25519 ? MBEDTLS_ECP_DP_CURVE25519 : MBEDTLS_ECP_DP_SECP256R1;
}

uint32_t ssh_ecdh_start(SshEcdh *e, int alg, uint8_t *pub, uint32_t cap) {
    e->alg = alg;
    e->live = true;
    mbedtls_ecp_group_init(&e->grp);
    mbedtls_mpi_init(&e->d);
    mbedtls_ecp_point_init(&e->Q);
    if (alg != SSH_KEX_C25519 && alg != SSH_KEX_P256) return 0;
    if (mbedtls_ecp_group_load(&e->grp, group_of(alg)) != 0) return 0;
    if (mbedtls_ecdh_gen_public(&e->grp, &e->d, &e->Q, ssh_rng, nullptr) != 0) return 0;
    size_t n = 0;
    if (mbedtls_ecp_point_write_binary(&e->grp, &e->Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &n, pub, cap) != 0) return 0;
    if ((alg == SSH_KEX_C25519 && n != 32) || (alg == SSH_KEX_P256 && n != 65)) return 0;
    return (uint32_t)n;
}

uint32_t ssh_ecdh_finish(SshEcdh *e, const uint8_t *peer, uint32_t pn, uint8_t k[32]) {
    if (!e->live) return 0;
    // RFC 8731: "Clients and servers MUST also abort if the length of the
    // received public keys are not the expected lengths."
    if ((e->alg == SSH_KEX_C25519 && pn != 32) || (e->alg == SSH_KEX_P256 && pn != 65))
        return 0;
    if (e->alg == SSH_KEX_P256 && peer[0] != 0x04) return 0;   // uncompressed only
    mbedtls_ecp_point P;
    mbedtls_mpi z;
    mbedtls_ecp_point_init(&P);
    mbedtls_mpi_init(&z);
    uint32_t out = 0;
    // RFC 5656 section 4: "All elliptic curve public keys MUST be validated
    // after they are received." check_pubkey does the SEC1 3.2.2 checks for
    // P-256 and refuses the known low-order points for Curve25519.
    if (mbedtls_ecp_point_read_binary(&e->grp, &P, peer, pn) == 0 &&
        mbedtls_ecp_check_pubkey(&e->grp, &P) == 0 &&
        mbedtls_ecdh_compute_shared(&e->grp, &z, &P, &e->d, ssh_rng, nullptr) == 0) {
        int rc = (e->alg == SSH_KEX_C25519) ? mbedtls_mpi_write_binary_le(&z, k, 32)
                                            : mbedtls_mpi_write_binary(&z, k, 32);
        if (rc == 0) {
            // RFC 7748 section 6 / RFC 8731: abort on an all-zero secret.
            uint8_t any = 0;
            for (int i = 0; i < 32; i++) any |= k[i];
            if (any) out = 32;
        }
    }
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&P);
    return out;
}

void ssh_ecdh_free(SshEcdh *e) {
    if (!e->live) return;
    mbedtls_mpi_free(&e->d);
    mbedtls_ecp_point_free(&e->Q);
    mbedtls_ecp_group_free(&e->grp);
    e->live = false;
}

bool ssh_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]) {
    uint8_t s[32];
    memcpy(s, scalar, 32);
    s[0] &= 248; s[31] &= 127; s[31] |= 64;            // RFC 7748 decodeScalar25519
    mbedtls_ecp_group grp; mbedtls_mpi d, z; mbedtls_ecp_point P;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&P);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_mpi_read_binary_le(&d, s, 32) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &P, u, 32) == 0 &&
              mbedtls_ecdh_compute_shared(&grp, &z, &P, &d, ssh_rng, nullptr) == 0 &&
              mbedtls_mpi_write_binary_le(&z, out, 32) == 0;
    mbedtls_ecp_point_free(&P); mbedtls_mpi_free(&z); mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    ssh_wipe(s, sizeof(s));
    return ok;
}

// --- the host key ----------------------------------------------------------------

static const char kEcdsaName[] = "ecdsa-sha2-nistp256";

bool ssh_hostkey_generate(SshHostKey *k) {
    k->ok = false;
    mbedtls_ecp_group grp; mbedtls_mpi d; mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&Q);
    size_t n = 0;
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_gen_keypair(&grp, &d, &Q, ssh_rng, nullptr) == 0 &&
              mbedtls_mpi_write_binary(&d, k->d, 32) == 0 &&
              mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &n,
                                             k->q, sizeof(k->q)) == 0 && n == 65;
    mbedtls_ecp_point_free(&Q); mbedtls_mpi_free(&d); mbedtls_ecp_group_free(&grp);
    k->ok = ok;
    if (!ok) ssh_wipe(k->d, sizeof(k->d));
    return ok;
}

bool ssh_hostkey_load(SshHostKey *k, const uint8_t d_in[32]) {
    k->ok = false;
    mbedtls_ecp_group grp; mbedtls_mpi d; mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&Q);
    size_t n = 0;
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_mpi_read_binary(&d, d_in, 32) == 0 &&
              mbedtls_ecp_check_privkey(&grp, &d) == 0 &&
              mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, ssh_rng, nullptr) == 0 &&
              mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &n,
                                             k->q, sizeof(k->q)) == 0 && n == 65;
    if (ok) memcpy(k->d, d_in, 32);
    mbedtls_ecp_point_free(&Q); mbedtls_mpi_free(&d); mbedtls_ecp_group_free(&grp);
    k->ok = ok;
    return ok;
}

uint32_t ssh_hostkey_blob(const SshHostKey *k, uint8_t *out, uint32_t cap) {
    SshW w; sshw_init(&w, out, cap);
    sshw_cstr(&w, kEcdsaName);
    sshw_cstr(&w, "nistp256");
    sshw_string(&w, k->q, 65);
    return w.err ? 0 : w.len;
}

uint32_t ssh_hostkey_sign(const SshHostKey *k, const uint8_t *data, uint32_t n,
                          uint8_t *out, uint32_t cap) {
    if (!k->ok) return 0;
    uint8_t hash[32];
    ssh_sha256(data, n, hash);
    mbedtls_ecp_group grp; mbedtls_mpi d, r, s;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    uint8_t rb[32], sb[32];
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_mpi_read_binary(&d, k->d, 32) == 0 &&
              mbedtls_ecdsa_sign_det_ext(&grp, &r, &s, &d, hash, sizeof(hash),
                                         MBEDTLS_MD_SHA256, ssh_rng, nullptr) == 0 &&
              mbedtls_mpi_write_binary(&r, rb, 32) == 0 &&
              mbedtls_mpi_write_binary(&s, sb, 32) == 0;
    mbedtls_mpi_free(&s); mbedtls_mpi_free(&r); mbedtls_mpi_free(&d); mbedtls_ecp_group_free(&grp);
    if (!ok) return 0;
    // string "ecdsa-sha2-nistp256", string (mpint r, mpint s)
    uint8_t inner[2 * (4 + 33)];
    SshW wi; sshw_init(&wi, inner, sizeof(inner));
    sshw_mpint(&wi, rb, 32);
    sshw_mpint(&wi, sb, 32);
    SshW w; sshw_init(&w, out, cap);
    sshw_cstr(&w, kEcdsaName);
    sshw_string(&w, inner, wi.len);
    return (w.err || wi.err) ? 0 : w.len;
}

// --- verifying -------------------------------------------------------------------

static bool streq(const uint8_t *d, uint32_t n, const char *s) {
    return n == strlen(s) && memcmp(d, s, n) == 0;
}

static const char kEd25519Name[] = "ssh-ed25519";

bool ssh_key_type(const uint8_t *keyblob, uint32_t kn, char *out, uint32_t cap) {
    SshR r; sshr_init(&r, keyblob, kn);
    const uint8_t *t; uint32_t tn;
    if (!sshr_string(&r, &t, &tn)) return false;
    const char *name = nullptr;
    if (streq(t, tn, "ssh-rsa")) name = "ssh-rsa";
    else if (streq(t, tn, kEcdsaName)) name = kEcdsaName;
    else if (streq(t, tn, kEd25519Name)) name = kEd25519Name;
    if (!name || strlen(name) >= cap) return false;
    strcpy(out, name);
    return true;
}

static bool verify_ecdsa(SshR *key, SshR *sig, const uint8_t *data, uint32_t dn) {
    const uint8_t *curve, *q; uint32_t cn, qn;
    if (!sshr_string(key, &curve, &cn) || !streq(curve, cn, "nistp256")) return false;
    if (!sshr_string(key, &q, &qn) || qn != 65 || q[0] != 0x04) return false;
    if (sshr_left(key) != 0) return false;
    const uint8_t *inner; uint32_t in_n;
    if (!sshr_string(sig, &inner, &in_n) || sshr_left(sig) != 0) return false;
    SshR ir; sshr_init(&ir, inner, in_n);
    const uint8_t *rb, *sb; uint32_t rn, sn;
    if (!sshr_mpint(&ir, &rb, &rn) || !sshr_mpint(&ir, &sb, &sn) || sshr_left(&ir) != 0)
        return false;
    if (rn == 0 || sn == 0 || rn > 32 || sn > 32) return false;

    uint8_t hash[32];
    ssh_sha256(data, dn, hash);
    mbedtls_ecp_group grp; mbedtls_ecp_point Q; mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q); mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &Q, q, qn) == 0 &&
              mbedtls_ecp_check_pubkey(&grp, &Q) == 0 &&
              mbedtls_mpi_read_binary(&r, rb, rn) == 0 &&
              mbedtls_mpi_read_binary(&s, sb, sn) == 0 &&
              mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &Q, &r, &s) == 0;
    mbedtls_mpi_free(&s); mbedtls_mpi_free(&r); mbedtls_ecp_point_free(&Q); mbedtls_ecp_group_free(&grp);
    return ok;
}

// RSA moduli this will believe. 2048 is the floor: anything smaller is
// factorable by somebody with a budget, and OpenSSH has refused to generate one
// for years. 8192 is the ceiling because the public operation is done here, on
// the connection's own task, before the client has proved anything.
#define RSA_MIN_BITS 2048
#define RSA_MAX_BITS 8192

static bool verify_rsa(SshR *key, SshR *sig, const uint8_t *data, uint32_t dn, const char *alg) {
    const uint8_t *e, *n; uint32_t en, nn;
    if (!sshr_mpint(key, &e, &en) || !sshr_mpint(key, &n, &nn) || sshr_left(key) != 0) return false;
    // A small odd public exponent. A huge one makes the public operation as
    // expensive as a private one, which is a cheap way to tie up the device.
    if (en == 0 || en > 4 || !(e[en - 1] & 1)) return false;
    uint32_t bits = nn * 8;
    if (nn) { uint8_t top = n[0]; while (!(top & 0x80)) { top <<= 1; bits--; } }
    if (bits < RSA_MIN_BITS || bits > RSA_MAX_BITS) return false;

    const uint8_t *s; uint32_t sn;
    if (!sshr_string(sig, &s, &sn) || sshr_left(sig) != 0) return false;
    // RFC 8332 section 3: the signature is an octet string no longer than the
    // modulus. Shorter ones exist in the wild (a leading zero byte dropped) and
    // are left-padded back to the modulus length, which is what PKCS#1 expects.
    if (sn == 0 || sn > nn) return false;

    bool sha512 = strcmp(alg, "rsa-sha2-512") == 0;
    mbedtls_md_type_t md = sha512 ? MBEDTLS_MD_SHA512 : MBEDTLS_MD_SHA256;
    uint8_t hash[64];
    uint32_t hn = sha512 ? 64 : 32;
    if (sha512) mbedtls_sha512(data, dn, hash, 0);
    else        ssh_sha256(data, dn, hash);

    uint8_t *padded = (uint8_t *)calloc(1, nn);
    if (!padded) return false;
    memcpy(padded + (nn - sn), s, sn);

    mbedtls_rsa_context ctx;
    mbedtls_rsa_init(&ctx);
    bool ok = mbedtls_rsa_import_raw(&ctx, n, nn, nullptr, 0, nullptr, 0, nullptr, 0, e, en) == 0 &&
              mbedtls_rsa_complete(&ctx) == 0 &&
              mbedtls_rsa_check_pubkey(&ctx) == 0 &&
              mbedtls_rsa_set_padding(&ctx, MBEDTLS_RSA_PKCS_V15, md) == 0 &&
              mbedtls_rsa_get_len(&ctx) == nn &&
              mbedtls_rsa_pkcs1_verify(&ctx, md, hn, hash, padded) == 0;
    mbedtls_rsa_free(&ctx);
    free(padded);
    return ok;
}

// RFC 8709 sections 4 and 6: the key is string "ssh-ed25519", string A (32
// bytes); the signature is string "ssh-ed25519", string R || S (64 bytes).
// Anything else in either blob, or another length, is refused before any
// arithmetic is done.
static bool verify_ed25519(SshR *key, SshR *sig, const uint8_t *data, uint32_t dn) {
    const uint8_t *a, *s; uint32_t an, sn;
    if (!sshr_string(key, &a, &an) || an != 32 || sshr_left(key) != 0) return false;
    if (!sshr_string(sig, &s, &sn) || sn != 64 || sshr_left(sig) != 0) return false;
    return ed25519_verify(s, data, dn, a);
}

bool ssh_sig_verify(const uint8_t *keyblob, uint32_t kn, const uint8_t *sigblob, uint32_t sbn,
                    const uint8_t *data, uint32_t dn, const char *alg) {
    SshR key; sshr_init(&key, keyblob, kn);
    SshR sig; sshr_init(&sig, sigblob, sbn);
    const uint8_t *kt, *st; uint32_t ktn, stn;
    if (!sshr_string(&key, &kt, &ktn) || !sshr_string(&sig, &st, &stn)) return false;
    // The signature must be the algorithm the protocol message named.
    if (!streq(st, stn, alg)) return false;
    if (streq(kt, ktn, kEcdsaName) && strcmp(alg, kEcdsaName) == 0)
        return verify_ecdsa(&key, &sig, data, dn);
    if (streq(kt, ktn, "ssh-rsa") &&
        (strcmp(alg, "rsa-sha2-256") == 0 || strcmp(alg, "rsa-sha2-512") == 0))
        return verify_rsa(&key, &sig, data, dn, alg);
    if (streq(kt, ktn, kEd25519Name) && strcmp(alg, kEd25519Name) == 0)
        return verify_ed25519(&key, &sig, data, dn);
    return false;
}

// --- fingerprints and base64 --------------------------------------------------------

uint32_t ssh_b64_encode(const uint8_t *in, uint32_t n, char *out, uint32_t cap) {
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, cap, &olen, in, n) != 0) return 0;
    return (uint32_t)olen;     // NUL-terminated by mbedTLS
}

uint32_t ssh_b64_decode(const char *in, uint32_t n, uint8_t *out, uint32_t cap) {
    size_t olen = 0;
    if (mbedtls_base64_decode(out, cap, &olen, (const unsigned char *)in, n) != 0) return 0;
    return (uint32_t)olen;
}

void ssh_fingerprint(const uint8_t *blob, uint32_t n, char *out, uint32_t cap) {
    uint8_t h[32];
    ssh_sha256(blob, n, h);
    char b64[48];
    uint32_t len = ssh_b64_encode(h, 32, b64, sizeof(b64));
    while (len && b64[len - 1] == '=') b64[--len] = 0;      // OpenSSH drops the padding
    if (cap) { out[0] = 0; }
    if (cap > 7 + len) { memcpy(out, "SHA256:", 7); memcpy(out + 7, b64, len + 1); }
}

// --- the packet ciphers ----------------------------------------------------------

static bool is_gcm(int alg) { return alg == SSH_AES128_GCM || alg == SSH_AES256_GCM; }
static bool is_ctr(int alg) { return alg == SSH_AES128_CTR || alg == SSH_AES256_CTR; }

uint32_t ssh_cipher_keylen(int alg) {
    return (alg == SSH_AES128_GCM || alg == SSH_AES128_CTR) ? 16
         : (alg == SSH_AES256_GCM || alg == SSH_AES256_CTR) ? 32 : 0;
}
// The draft and OpenSSH both: a 12-byte GCM nonce comes from key derivation,
// split as a 4-byte fixed field and an 8-byte invocation counter (RFC 5647 7.1).
uint32_t ssh_cipher_ivlen(int alg) { return is_gcm(alg) ? 12 : is_ctr(alg) ? 16 : 0; }
uint32_t ssh_cipher_block(int alg) { return alg == SSH_CIPHER_NONE ? 8 : 16; }
uint32_t ssh_cipher_taglen(int alg, int mac) {
    if (is_gcm(alg)) return 16;
    return mac == SSH_MAC_NONE ? 0 : 32;
}
bool ssh_cipher_len_plain(int alg, int mac) {
    return alg == SSH_CIPHER_NONE || is_gcm(alg) || mac == SSH_MAC_HMAC256_ETM;
}

void ssh_cipher_init(SshCipher *c) {
    memset(c, 0, sizeof(*c));
    c->ks_off = 16;
}

bool ssh_cipher_set(SshCipher *c, int alg, int mac, const uint8_t *key, const uint8_t *iv,
                    const uint8_t *mac_key) {
    ssh_cipher_free(c);
    c->alg = (uint8_t)alg;
    c->mac = is_gcm(alg) ? (uint8_t)SSH_MAC_NONE : (uint8_t)mac;
    uint32_t kl = ssh_cipher_keylen(alg);
    if (is_gcm(alg)) {
        mbedtls_gcm_init(&c->u.gcm);
        if (mbedtls_gcm_setkey(&c->u.gcm, MBEDTLS_CIPHER_ID_AES, key, kl * 8) != 0) return false;
        memcpy(c->iv, iv, 12);
    } else if (is_ctr(alg)) {
        mbedtls_aes_init(&c->u.aes);
        if (mbedtls_aes_setkey_enc(&c->u.aes, key, kl * 8) != 0) return false;
        memcpy(c->iv, iv, 16);
        c->ks_off = 16;
    }
    if (c->mac != SSH_MAC_NONE) {
        mbedtls_md_init(&c->hmac);
        c->hmac_live = true;
        if (mbedtls_md_setup(&c->hmac, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) != 0)
            return false;
        if (mbedtls_md_hmac_starts(&c->hmac, mac_key, 32) != 0) return false;
    }
    return true;
}

void ssh_cipher_free(SshCipher *c) {
    if (is_gcm(c->alg)) mbedtls_gcm_free(&c->u.gcm);
    else if (is_ctr(c->alg)) mbedtls_aes_free(&c->u.aes);
    if (c->hmac_live) mbedtls_md_free(&c->hmac);
    ssh_wipe(c, sizeof(*c));
    c->ks_off = 16;
}

// The GCM invocation counter is the low 8 bytes of the nonce, big-endian, and
// goes up by one per packet.
static void gcm_next(SshCipher *c) {
    for (int i = 11; i >= 4; i--) if (++c->iv[i]) break;
}

// AES-CTR over the stream. The keystream carries across packets: the counter
// block is the IV from key derivation, incremented as a 128-bit big-endian
// number per block (RFC 4344 section 4), never reset.
static void ctr_xor(SshCipher *c, uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (c->ks_off == 16) {
            mbedtls_aes_crypt_ecb(&c->u.aes, MBEDTLS_AES_ENCRYPT, c->iv, c->ks);
            for (int k = 15; k >= 0; k--) if (++c->iv[k]) break;
            c->ks_off = 0;
        }
        p[i] ^= c->ks[c->ks_off++];
    }
}

static void hmac_over(SshCipher *c, uint32_t seq, const uint8_t *p, uint32_t n, uint8_t out[32]) {
    uint8_t s[4] = { (uint8_t)(seq >> 24), (uint8_t)(seq >> 16), (uint8_t)(seq >> 8), (uint8_t)seq };
    mbedtls_md_hmac_reset(&c->hmac);
    mbedtls_md_hmac_update(&c->hmac, s, 4);
    mbedtls_md_hmac_update(&c->hmac, p, n);
    mbedtls_md_hmac_finish(&c->hmac, out);
}

uint32_t ssh_cipher_seal(SshCipher *c, uint32_t seq, uint8_t *p, uint32_t len) {
    if (is_gcm(c->alg)) {
        // AAD is the 4-byte length, which goes out in the clear (RFC 5647 7.3).
        mbedtls_gcm_crypt_and_tag(&c->u.gcm, MBEDTLS_GCM_ENCRYPT, len, c->iv, 12,
                                  p, 4, p + 4, p + 4, 16, p + 4 + len);
        gcm_next(c);
        return 16;
    }
    if (is_ctr(c->alg)) {
        if (c->mac == SSH_MAC_HMAC256_ETM) {
            ctr_xor(c, p + 4, len);                       // length stays plain
            hmac_over(c, seq, p, 4 + len, p + 4 + len);   // over the ciphertext
        } else {
            hmac_over(c, seq, p, 4 + len, p + 4 + len);   // over the plaintext
            ctr_xor(c, p, 4 + len);
        }
        return 32;
    }
    return 0;
}

void ssh_cipher_open_first(SshCipher *c, uint8_t *p) {
    if (is_ctr(c->alg) && c->mac == SSH_MAC_HMAC256) ctr_xor(c, p, 16);
}

bool ssh_cipher_open(SshCipher *c, uint32_t seq, uint8_t *p, uint32_t len) {
    if (is_gcm(c->alg)) {
        int rc = mbedtls_gcm_auth_decrypt(&c->u.gcm, len, c->iv, 12, p, 4,
                                          p + 4 + len, 16, p + 4, p + 4);
        gcm_next(c);
        return rc == 0;
    }
    if (is_ctr(c->alg)) {
        uint8_t mac[32];
        if (c->mac == SSH_MAC_HMAC256_ETM) {
            // Authenticate BEFORE decrypting anything: that is the whole point of
            // encrypt-then-MAC.
            hmac_over(c, seq, p, 4 + len, mac);
            if (!ssh_ct_equal(mac, p + 4 + len, 32)) return false;
            ctr_xor(c, p + 4, len);
            return true;
        }
        // Encrypt-and-MAC: the first block was decrypted to read the length.
        if (4 + len > 16) ctr_xor(c, p + 16, 4 + len - 16);
        hmac_over(c, seq, p, 4 + len, mac);
        return ssh_ct_equal(mac, p + 4 + len, 32);
    }
    return true;
}
