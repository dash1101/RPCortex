// Ed25519 signature verification (RFC 8032), for SSH: a user's ssh-ed25519
// key in authorized_keys, and a server's ssh-ed25519 host key.
//
// Verify only. The device's own keys stay ECDSA (mbedTLS), so nothing here
// handles a secret and nothing needs to be constant-time except the final
// comparison. The arithmetic is TweetNaCl's; ed25519.cpp says what was taken
// and what was changed.
#ifndef RPC_ED25519_H
#define RPC_ED25519_H

#include <stdint.h>
#include <stddef.h>

// Is sig (R || S, 64 bytes) a valid signature over msg by the public key pk?
//
// Strict RFC 8032 section 5.1.7: S must be below the group order L, A and R
// must each decode as section 5.1.3 says (y below p, a point on the curve,
// and no x of zero with the sign bit set), and the check is the cofactorless
// [S]B = R + [k]A. About 10,800 field multiplications; see ed25519.cpp for
// what that costs on the device.
bool ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t n, const uint8_t pk[32]);

// Does this 32-byte encoding decode as a point (RFC 8032 section 5.1.3)?
// ed25519_verify asks this of A and R; it is here so the rule can be tested
// on its own.
bool ed25519_point_ok(const uint8_t enc[32]);

#endif  // RPC_ED25519_H
