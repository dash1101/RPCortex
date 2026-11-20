// The OpenSSH text formats SSH needs on the device: authorized_keys,
// known_hosts and a public key line. Pure parsing, so the rules are tested on
// the host against lines ssh-keygen itself wrote.
#ifndef RPC_SSHKEYS_H
#define RPC_SSHKEYS_H

#include <stdint.h>

// Is the key blob listed in this authorized_keys text?
//
// A line is "<type> <base64> [comment]". A line that starts with OPTIONS
// (from="...", command="...", no-pty and the rest) is skipped rather than
// honoured without its options: each of those narrows what a key may do, and
// a key that was meant to run one command must not get a shell because the
// option was not understood. Types other than ecdsa-sha2-nistp256 and ssh-rsa
// are skipped too, and so is a line whose base64 names a different type than
// its first word.
bool ssh_authkeys_match(const char *text, uint32_t len, const uint8_t *blob, uint32_t n);

// "<type> <base64> <comment>". Returns the length, or 0 if it does not fit.
uint32_t ssh_pubkey_line(const uint8_t *blob, uint32_t n, const char *comment,
                         char *out, uint32_t cap);

enum SshKnown { SSH_KH_UNKNOWN = 0, SSH_KH_MATCH = 1, SSH_KH_CHANGED = 2 };

// What known_hosts says about this host and key. The host is written as
// OpenSSH writes it: the bare name for port 22 and "[name]:port" otherwise;
// a line may list several, comma-separated. SSH_KH_CHANGED means the host is
// listed with a key of the same type that is not this one, which is the case
// that has to stop a connection. Hashed (|1|) lines and wildcards are not
// matched; this reads the file the device itself writes.
SshKnown ssh_known_hosts_check(const char *text, uint32_t len, const char *host, uint16_t port,
                               const uint8_t *blob, uint32_t n);

// A line to add for this host and key, with its newline.
uint32_t ssh_known_hosts_line(const char *host, uint16_t port, const uint8_t *blob, uint32_t n,
                              char *out, uint32_t cap);

#endif  // RPC_SSHKEYS_H
