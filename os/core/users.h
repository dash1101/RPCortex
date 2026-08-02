// Accounts — the C++ replacement for usrmgmt.py.
//
// Matches v1's model for parity: salted SHA-256, per-user random salt, a role
// field, a NOPASS marker for the guest account, and root/guest protected from
// removal. The stored record mirrors v1's user.cfg line so the concept carries
// over one-to-one:
//
//     name,salthex$hashhex,/home/,role
//
// where role is "admin" or "user", hash = SHA-256(salt_bytes || password), and
// the NOPASS sentinel replaces salt$hash for an account that accepts any
// password (guest).
//
// Salted SHA-256 is what v1 shipped and is chosen here for parity, not because
// it is strong: with no secure element on the board an offline attacker with the
// file can brute-force it. PBKDF2/many-rounds is the intended hardening and is a
// one-function change (hash_password), noted where it goes.
//
// Core is pure and host-testable; persistence lives in users_fs.cpp.
#ifndef RPC_USERS_H
#define RPC_USERS_H

#include <stdint.h>

#define USR_MAX       16
#define USR_NAME_MAX  24
#define USR_CRED_MAX  100   // "salthex$hashhex" = 32 + 1 + 64 + NUL
#define USR_HOME_MAX  40

enum UserRole { ROLE_USER = 0, ROLE_ADMIN = 1 };

// Randomness for salts. Backed by the SDK RNG on the device and by the test's
// own source on the host — declared here, defined per target, so users.cpp
// stays pure. The same seam the loader used for its allocator.
extern "C" uint32_t rpc_rand32(void);

// Create an account. `password` is ignored when nopass is true (guest). Returns
// false if the name exists, the table is full, or a field is over length.
bool users_add(const char *name, const char *password, bool admin, bool nopass);

// True when the password matches — or the account is NOPASS, which accepts any.
bool users_verify(const char *name, const char *password);

bool users_exists(const char *name);
bool users_is_admin(const char *name);
bool users_is_nopass(const char *name);

// Change a password (clears NOPASS). Admin override path; the shell decides who
// may call it. Returns false if the user is absent or the value is over length.
bool users_set_password(const char *name, const char *password);

// Remove an account. root and guest are protected and always refused, matching
// v1 (factoryreset is the only thing that clears them, and that is a separate
// path). Returns false if refused or absent.
bool users_remove(const char *name);

uint32_t    users_count(void);
const char *users_name_at(uint32_t i);

void reg_note_home(const char *name, char *out, uint32_t cap);  // "/home/<name>/"

// Pure load/serialize, mirroring the registry, so a device reads user.cfg into
// users_load and writes users_serialize back.
void     users_clear(void);
void     users_load(const char *text, uint32_t len);
uint32_t users_serialize(char *buf, uint32_t cap);
bool     users_dirty(void);
void     users_mark_clean(void);

#endif  // RPC_USERS_H
