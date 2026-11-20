// Standard base64 (RFC 4648 section 4: A-Z a-z 0-9 + /, '=' padding).
//
// For link mode, where file contents and command output cross a console that
// is line-based and treats some bytes (0x03, 0x1E, newline) as meaning
// something. Pure, so it is tested on the host.
#ifndef RPC_B64_H
#define RPC_B64_H

#include <stdint.h>

// Characters needed to encode `n` bytes, without a terminator.
static inline uint32_t b64_encoded_len(uint32_t n) { return ((n + 2) / 3) * 4; }

// Encode `n` bytes into `out`, which must hold b64_encoded_len(n) + 1. Always
// NUL-terminates. Returns the length written.
uint32_t b64_encode(const uint8_t *in, uint32_t n, char *out);

// Decode `len` characters. Returns the number of bytes written, or -1 if the
// text is not valid base64 (a bad character, a length that is not a multiple
// of four, padding anywhere but the end) or would not fit in `cap`.
int32_t b64_decode(const char *in, uint32_t len, uint8_t *out, uint32_t cap);

#endif
