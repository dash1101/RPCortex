// The cryptography SSH needs, as a thin layer over mbedTLS.
//
// Nothing here is new cryptography: every primitive is mbedTLS's own, apart
// from Ed25519 verification, which mbedTLS does not have and which comes from
// TweetNaCl (ed25519.cpp). What this layer adds is the SSH-specific glue around them, which is where the
// interoperability bugs live — byte order of a Curve25519 secret, which bytes a
// GCM tag authenticates, how a key longer than one hash output is extended — so
// each of those is one function with one comment saying which document it
// follows, and the host tests hold each one to a published vector or to a real
// OpenSSH on the other end of a socket.
//
// Pure: no SDK, no lwIP, no tasks. The randomness comes from mbedtls_hardware_poll,
// which the SDK supplies on the device and a test supplies on the host.
#ifndef RPC_SSHCRYPTO_H
#define RPC_SSHCRYPTO_H

#include <stdint.h>
#include <stddef.h>

#include "mbedtls/gcm.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

// --- randomness ----------------------------------------------------------------

// The f_rng every mbedTLS call here is given. A CTR_DRBG seeded from the
// hardware source, shared by every connection and serialised by a lock: two
// sessions on two cores may ask at the same instant, and a DRBG is a single
// state machine. Returns 0, or an mbedTLS error if it could not be seeded.
int  ssh_rng(void *unused, unsigned char *out, size_t n);
// The same, for callers that cannot do anything useful with a failure except
// stop. False if the generator could not produce bytes.
bool ssh_random(void *out, size_t n);

// Overwrite a secret in a way the compiler may not remove.
void ssh_wipe(void *p, size_t n);

// Constant-time comparison for MACs and keys.
bool ssh_ct_equal(const void *a, const void *b, size_t n);

// --- hashing ---------------------------------------------------------------------

// The exchange hash and the key derivation, both SHA-256 for the two key
// exchanges offered. The string/mpint helpers hash an SSH encoding of a value
// rather than the value, which is what RFC 4253 section 8 hashes.
struct SshHash { mbedtls_sha256_context ctx; };
void ssh_hash_init(SshHash *h);
void ssh_hash_raw(SshHash *h, const void *data, uint32_t n);
void ssh_hash_u32(SshHash *h, uint32_t v);
void ssh_hash_string(SshHash *h, const void *data, uint32_t n);
void ssh_hash_mpint(SshHash *h, const uint8_t *be, uint32_t n);
void ssh_hash_final(SshHash *h, uint8_t out[32]);

void ssh_sha256(const void *data, uint32_t n, uint8_t out[32]);

// RFC 4253 section 7.2. K is the shared secret as an unsigned big-endian
// magnitude (encoded here as the mpint the RFC calls for), H the exchange hash,
// `letter` one of 'A'..'F'. Keys longer than one hash are extended with
// K2 = HASH(K || H || K1) and so on.
void ssh_kdf(const uint8_t *k, uint32_t kn, const uint8_t h[32], char letter,
             const uint8_t session_id[32], uint8_t *out, uint32_t outn);

// --- key exchange ----------------------------------------------------------------

enum SshKexAlg { SSH_KEX_NONE = 0, SSH_KEX_C25519 = 1, SSH_KEX_P256 = 2 };

// One side's ephemeral key for one exchange. Freed as soon as the shared
// secret is computed; the group tables are the largest allocation in a login.
struct SshEcdh {
    int alg;
    bool live;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
};

// Make a key pair and write the public value as it goes on the wire: 32 bytes,
// little-endian, for Curve25519 (RFC 8731); a 65-byte uncompressed point for
// P-256 (RFC 5656). Returns the length, or 0.
uint32_t ssh_ecdh_start(SshEcdh *e, int alg, uint8_t *pub, uint32_t cap);
// The shared secret from the peer's public value, as the big-endian bytes the
// mpint K is made from. For Curve25519 those are the X25519 output bytes
// themselves, taken as a big-endian number (RFC 8731 section 3.1) — not the
// integer mbedTLS computed, which is the same bytes read the other way round.
// Refuses a public value of the wrong length, one that is not a valid point,
// and an all-zero result. Returns the length (32), or 0.
uint32_t ssh_ecdh_finish(SshEcdh *e, const uint8_t *peer, uint32_t pn, uint8_t k[32]);
void     ssh_ecdh_free(SshEcdh *e);

// X25519 itself (RFC 7748 section 5), for the published test vectors: the
// scalar is clamped here, as the function definition requires.
bool ssh_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u[32]);

// --- the host key: ECDSA on P-256 -------------------------------------------------

struct SshHostKey {
    uint8_t d[32];      // the secret scalar
    uint8_t q[65];      // 0x04 || X || Y
    bool    ok;
};

bool     ssh_hostkey_generate(SshHostKey *k);
// From a stored secret scalar. Recomputes the public point rather than trusting
// a stored copy, so a damaged file cannot pair one key's secret with another's
// public half. False for a scalar that is not a valid private key.
bool     ssh_hostkey_load(SshHostKey *k, const uint8_t d[32]);
// "ecdsa-sha2-nistp256" public key blob (RFC 5656 section 3.1).
uint32_t ssh_hostkey_blob(const SshHostKey *k, uint8_t *out, uint32_t cap);
// Signature blob over `data` (RFC 5656 section 3.1.2), deterministic k
// (RFC 6979) so a weak moment of the hardware generator cannot leak the key.
uint32_t ssh_hostkey_sign(const SshHostKey *k, const uint8_t *data, uint32_t n,
                          uint8_t *out, uint32_t cap);

// --- verifying somebody else's signature -------------------------------------------

// Is `sig` a valid signature over `data` by the key in `keyblob`? `alg` is the
// signature algorithm the protocol said would be used ("ecdsa-sha2-nistp256",
// "rsa-sha2-256", "rsa-sha2-512" or "ssh-ed25519"), and the blob must name the
// same one, so a peer cannot ask for one algorithm and sign with another. RSA
// keys below 2048 bits are refused, and so is plain "ssh-rsa" (SHA-1).
bool ssh_sig_verify(const uint8_t *keyblob, uint32_t kn, const uint8_t *sig, uint32_t sn,
                    const uint8_t *data, uint32_t dn, const char *alg);

// Which public key algorithms a key blob may be used with. Writes the key type
// ("ssh-rsa", "ecdsa-sha2-nistp256", "ssh-ed25519") and returns false for
// anything this does not support.
bool ssh_key_type(const uint8_t *keyblob, uint32_t kn, char *out, uint32_t cap);

// "SHA256:" and the unpadded base64 of the SHA-256 of the blob — byte for byte
// what `ssh-keygen -l` and the OpenSSH client print.
void ssh_fingerprint(const uint8_t *blob, uint32_t n, char *out, uint32_t cap);

// Base64, for authorized_keys and known_hosts lines. Return the output length,
// or 0 when it does not fit or (decoding) the input is not base64.
uint32_t ssh_b64_encode(const uint8_t *in, uint32_t n, char *out, uint32_t cap);
uint32_t ssh_b64_decode(const char *in, uint32_t n, uint8_t *out, uint32_t cap);

// --- the packet ciphers ------------------------------------------------------------

enum SshCipherAlg {
    SSH_CIPHER_NONE = 0,
    SSH_AES128_GCM,     // aes128-gcm@openssh.com: RFC 5647, OpenSSH negotiation
    SSH_AES256_GCM,
    SSH_AES128_CTR,     // RFC 4344, with a separate MAC
    SSH_AES256_CTR,
};
enum SshMacAlg {
    SSH_MAC_NONE = 0,
    SSH_MAC_HMAC256,        // hmac-sha2-256: encrypt-and-MAC (RFC 6668)
    SSH_MAC_HMAC256_ETM,    // hmac-sha2-256-etm@openssh.com: MAC over the ciphertext
};

struct SshCipher {
    uint8_t alg;
    uint8_t mac;
    union {
        mbedtls_gcm_context gcm;
        mbedtls_aes_context aes;
    } u;
    uint8_t iv[16];          // GCM: the 12-byte nonce. CTR: the counter block.
    uint8_t ks[16];          // CTR keystream not yet used
    uint8_t ks_off;          // 16 = none left
    bool    hmac_live;
    mbedtls_md_context_t hmac;
};

uint32_t ssh_cipher_keylen(int alg);
uint32_t ssh_cipher_ivlen(int alg);
uint32_t ssh_cipher_block(int alg);   // 8 for none, 16 for AES
uint32_t ssh_cipher_taglen(int alg, int mac);
bool     ssh_cipher_len_plain(int alg, int mac);   // packet_length sent unencrypted

void ssh_cipher_init(SshCipher *c);
// Take keys into use. mac_key may be null for GCM and none.
bool ssh_cipher_set(SshCipher *c, int alg, int mac, const uint8_t *key, const uint8_t *iv,
                    const uint8_t *mac_key);
void ssh_cipher_free(SshCipher *c);

// Encrypt the packet at `p`: p[0..4) the length, p[4..4+len) the body. The
// tag or MAC is written at p + 4 + len; returns its length.
uint32_t ssh_cipher_seal(SshCipher *c, uint32_t seq, uint8_t *p, uint32_t len);
// For encrypt-and-MAC CTR only: decrypt the first block in place, so the
// length can be read. Called once per packet, before ssh_cipher_open.
void ssh_cipher_open_first(SshCipher *c, uint8_t *p);
// Authenticate and decrypt in place. False on a bad tag or MAC, which ends the
// connection: there is no recovering the stream position after one.
bool ssh_cipher_open(SshCipher *c, uint32_t seq, uint8_t *p, uint32_t len);

#endif  // RPC_SSHCRYPTO_H
