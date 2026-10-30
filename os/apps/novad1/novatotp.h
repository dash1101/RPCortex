// Desc: One-time passcodes (RFC 6238 / RFC 4226), with the SHA-1 and HMAC they stand on.
// File: novatotp.h
//
// Ported from the MicroPython TOTP app, which carried its own SHA-1 so that it worked
// on any build. The package ABI has SHA-256 and no SHA-1, and SHA-1 is what every
// authenticator app a service hands out a QR code for is using, so it lives here:
// pure, allocation-free, and checked against the published test vectors.
//
// SHA-1 is broken as a collision-resistant hash and that does not matter here: HMAC-SHA1
// is not affected by it, which is why RFC 6238 still specifies it.
#ifndef NOVA_TOTP_H
#define NOVA_TOTP_H

#include <stdint.h>

namespace nova {
namespace totp {

struct Sha1 {
    uint32_t h[5];
    uint8_t  block[64];
    uint32_t len;               // bytes taken so far
};

void     sha1_begin(Sha1 *s);
void     sha1_add(Sha1 *s, const void *data, unsigned n);
void     sha1_end(Sha1 *s, uint8_t out[20]);

// HMAC-SHA1 of `msg` under `key`; a key longer than a block is hashed first.
void     hmac_sha1(const uint8_t *key, unsigned klen, const uint8_t *msg, unsigned mlen, uint8_t out[20]);

// RFC 4648 base32 as authenticator secrets are written: upper or lower case, spaces and
// dashes ignored, padding optional, and any other character skipped, as the MicroPython
// decoder did. Returns the bytes written (at most `cap`).
unsigned b32_decode(const char *text, uint8_t *out, unsigned cap);

// The code for a key at a Unix time, `digits` long (6 to 8), zero-padded, in out (which
// needs digits+1 bytes).
void     code(const uint8_t *key, unsigned klen, uint32_t unix_time, uint32_t step,
              unsigned digits, char *out);

// The same from the base32 text a secret is stored as. False when the text decodes to
// nothing at all.
bool     code_b32(const char *secret_b32, uint32_t unix_time, char *out);

// Seconds since 1970 from a calendar date and time (UTC), for the years a package can see.
uint32_t unix_from_civil(int year, int month, int day, int hour, int minute, int second);

}  // namespace totp
}  // namespace nova

#endif  // NOVA_TOTP_H
