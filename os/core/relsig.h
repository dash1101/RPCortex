// Signed releases: is this release list (releases/latest.json, exactly the bytes
// fetched) signed by one of the RPCortex release keys built into the firmware?
//
// tools/release.py signs latest.json with Ed25519 and publishes the signature
// beside it as latest.json.sig, in the same commit, as lines of
//
//   ed25519 <key id> <signature, base64>
//
// where the key id is the first 8 hex digits of SHA-256 of the 32-byte public
// key. A file may carry more than one line (a key being replaced signs beside
// the new one); one good line from a known key is enough. Lines that are not
// understood are ignored, so a later format can be added beside this one.
//
// The keys: a primary that signs every release, and a backup kept offline that
// can sign if the primary is ever lost, so a lost key never strands a board.
#ifndef RPC_RELSIG_H
#define RPC_RELSIG_H

#include <stddef.h>
#include <stdint.h>

enum RelSig {
    RELSIG_OK = 0,          // signed by a key this firmware knows
    RELSIG_MISSING,         // no signature line at all
    RELSIG_UNKNOWN_KEY,     // signed, but by no key this firmware carries
    RELSIG_BAD,             // a known key's signature that does not verify: altered
};

// `which` (optional, 9 bytes) gets the id of the key that verified.
RelSig relsig_check(const uint8_t *manifest, size_t n, const char *sig_text, char which[9]);

const char *relsig_str(RelSig r);

#endif
