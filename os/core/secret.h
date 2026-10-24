// Sealing a short secret so it can sit in a text file.
//
// Saved Wi-Fi passwords were stored in the clear, so a copy of the registry, a
// backup, or a support dump handed them over. This seals one under a key that
// lives somewhere else, and ties it to a context (the network name) so a sealed
// value cannot be moved to another entry.
//
// WHAT IT PROTECTS AGAINST, stated plainly because it is easy to overclaim: a
// copy of the file the value is stored in. It does not protect against anyone who
// also has the key, and on a device with no secure element the key sits in the
// same flash, so someone holding the whole flash can recover the password.
//
// THE CONSTRUCTION. Everything is HMAC-SHA-256, which the tree already has:
//
//   ek  = HMAC(key, "rpc.secret.enc.v1")      separate keys for each job
//   mk  = HMAC(key, "rpc.secret.mac.v1")
//   T   = HMAC(mk, len(context) || context || plain)[0..6)      the tag
//   ks  = HMAC(ek, T || 0) || HMAC(ek, T || 1) || ...            the keystream
//   out = "~1" || base64url(T || (plain XOR ks))                 no padding
//
// The tag is derived from the plaintext and then used as the keystream's nonce,
// which is the "synthetic IV" arrangement: no random nonce to generate or store,
// and the same input always seals to the same text, so re-saving an unchanged
// password does not rewrite the registry. Opening decrypts and then checks the
// tag, so a wrong key, a wrong context or a changed byte is rejected rather than
// returning garbage.
//
// SIZE. The registry holds 95 characters per value. A 63-character passphrase,
// the WPA2 maximum, seals to 94, so it fits without raising that limit.
#ifndef RPC_SECRET_H
#define RPC_SECRET_H

#include <stddef.h>
#include <stdint.h>

#define SECRET_KEY_LEN   32
#define SECRET_MAX_PLAIN 63          // the longest WPA2 passphrase
#define SECRET_TAG_LEN   6
#define SECRET_SEAL_MAX  96          // room for any sealed value and its terminator

enum SecretResult {
    SECRET_OK = 0,           // sealed, verified, and decrypted
    SECRET_NOT_SEALED,       // no "~1" marker: an old plaintext value, or empty
    SECRET_BAD,              // marked, but it did not verify: wrong key or context, or damaged
};

// Seal `plain` (1 to SECRET_MAX_PLAIN bytes, no NUL) for `context` (up to 255).
// An empty `plain` produces an empty string: there is nothing to hide, and an open
// network's empty password should stay empty. Returns false if it does not fit
// `cap`, is too long, or an argument is missing.
bool secret_seal(const uint8_t key[SECRET_KEY_LEN], const char *context,
                 const char *plain, char *out, size_t cap);

// The reverse. On SECRET_OK `plain` holds the secret. On the others it holds an
// empty string, so a caller that ignores the result cannot use half a password.
SecretResult secret_open(const uint8_t key[SECRET_KEY_LEN], const char *context,
                         const char *sealed, char *plain, size_t cap);

// Does this string carry the marker, whether or not it would verify?
bool secret_is_sealed(const char *s);

#endif
