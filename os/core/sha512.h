// SHA-512 (FIPS 180-4), for Ed25519 (core/ed25519.cpp). Its own, rather than
// mbedTLS's, because images without Wi-Fi carry no mbedTLS and still have to
// check a release's signature. Small and plain; not tuned for speed, since it
// hashes a manifest of a couple of kilobytes and a 64-byte prefix.
#ifndef RPC_SHA512_H
#define RPC_SHA512_H

#include <stddef.h>
#include <stdint.h>

#define SHA512_DIGEST_LEN 64

struct Sha512Ctx {
    uint64_t h[8];
    uint8_t  buf[128];
    uint32_t used;          // bytes in buf
    uint64_t total;         // bytes hashed so far
};

void sha512_init(Sha512Ctx *c);
void sha512_update(Sha512Ctx *c, const void *data, size_t n);
void sha512_final(Sha512Ctx *c, uint8_t out[SHA512_DIGEST_LEN]);

#endif
