// SHA-256 (FIPS 180-4). Public-domain-style compact implementation, vendored
// rather than pulled from mbedTLS: the accounts system needs one hash and
// linking a TLS library for it would be a large dependency for a small job.
// Verified against the standard test vectors in the host test.
#ifndef RPC_SHA256_H
#define RPC_SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_LEN 32

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    uint32_t buflen;
};

void sha256_init(Sha256Ctx *c);
void sha256_update(Sha256Ctx *c, const void *data, size_t len);
void sha256_final(Sha256Ctx *c, uint8_t out[SHA256_DIGEST_LEN]);

// One-shot convenience.
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

// Hex-encode a digest into `out` (must hold 2*len+1 bytes).
void hex_encode(const uint8_t *in, size_t len, char *out);

#endif  // RPC_SHA256_H
