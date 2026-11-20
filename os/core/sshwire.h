// The SSH wire encodings (RFC 4251 section 5), bounded both ways.
//
// Every number in an SSH packet came from the peer, before or after
// authentication, so the reader never trusts one: a string length is checked
// against what is left of the packet before anything is touched, and the first
// failure sets a sticky error that every later read also reports. Callers read
// a whole message and test `err` once at the end, which keeps the parsers short
// enough to read and leaves no path that uses a field after a bad length.
//
// The writer is the same shape: a fixed buffer, a sticky overflow flag, and no
// allocation. A message that does not fit is a bug in the caller's sizing, and
// the flag is how the caller finds out rather than a write past the end.
#ifndef RPC_SSHWIRE_H
#define RPC_SSHWIRE_H

#include <stdint.h>

struct SshW {
    uint8_t *p;
    uint32_t cap;
    uint32_t len;
    bool     err;
};

void sshw_init(SshW *w, uint8_t *buf, uint32_t cap);
void sshw_u8(SshW *w, uint8_t v);
void sshw_bool(SshW *w, bool v);
void sshw_u32(SshW *w, uint32_t v);
void sshw_raw(SshW *w, const void *data, uint32_t n);
void sshw_string(SshW *w, const void *data, uint32_t n);
void sshw_cstr(SshW *w, const char *s);
// An mpint from an unsigned big-endian magnitude. Leading zero bytes are
// dropped and a 0x00 is put in front when the top bit is set, exactly as RFC
// 4251 section 5 requires: two encodings of one number would hash differently,
// and the exchange hash is over this encoding.
void sshw_mpint(SshW *w, const uint8_t *be, uint32_t n);

struct SshR {
    const uint8_t *p;
    uint32_t len;
    uint32_t pos;
    bool     err;
};

void     sshr_init(SshR *r, const uint8_t *buf, uint32_t len);
uint8_t  sshr_u8(SshR *r);
bool     sshr_bool(SshR *r);
uint32_t sshr_u32(SshR *r);
// A string, as a pointer into the packet and a length. Nothing is copied, so
// the pointer is good only while the packet is.
bool     sshr_string(SshR *r, const uint8_t **data, uint32_t *n);
// The same, copied out as a C string. Refuses one that does not fit or that
// carries a NUL, because a name with a NUL in it compares equal to a
// different, shorter name.
bool     sshr_cstr(SshR *r, char *out, uint32_t cap);
// An mpint that must be non-negative; the magnitude without its sign byte.
bool     sshr_mpint(SshR *r, const uint8_t **mag, uint32_t *n);
uint32_t sshr_left(const SshR *r);

// --- name-lists ---------------------------------------------------------------

// Is `name` one of the comma-separated entries? Whole names only: "aes128-ctr"
// is not in "aes128-ctr-foo".
bool ssh_namelist_has(const uint8_t *list, uint32_t n, const char *name);

// RFC 4253 section 7.1: the first name on the CLIENT's list that is also on the
// server's. Writes it to `out` and returns true, or false when there is none.
bool ssh_negotiate(const uint8_t *client, uint32_t cn, const uint8_t *server, uint32_t sn,
                   char *out, uint32_t cap);

// The first entry of a list, for the "did the guess match" test.
bool ssh_namelist_first(const uint8_t *list, uint32_t n, char *out, uint32_t cap);

#endif  // RPC_SSHWIRE_H
