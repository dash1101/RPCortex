// The registry — the C++ replacement for regedit.py.
//
// v1 stored dot-notation keys (Settings.Version, System.Owner) in an INI file
// and cached the parse. This keeps the dot-notation keys for continuity but
// drops INI sections: it is a flat key=value store, which is all v1 ever used it
// as. The keys carry their own namespace in the name.
//
// The core is PURE — load-from-text and serialize-to-text, no filesystem — so it
// host-tests without a device. Persistence (read the file, load; serialize,
// write the file) is a thin wrapper in registry_fs.cpp that the device links and
// the host test does not.
//
// The two bugs v1's registry actually hit are designed out here: there is one
// instance (a single static table, not two module copies with two caches), and
// a set touches only its own key (the whole table is in memory, so there is no
// stale-cache-clobbers-neighbour path).
#ifndef RPC_REGISTRY_H
#define RPC_REGISTRY_H

#include <stdint.h>

#define REG_MAX      64
#define REG_KEY_MAX  40
#define REG_VAL_MAX  96

// Read a value, or `def` (which may be nullptr) when the key is absent.
const char *reg_get(const char *key, const char *def);

// Write a value. Returns false only if the table is full on a new key or the
// key/value is over length. An existing key is updated in place.
bool reg_set(const char *key, const char *value);

// Convenience: integer read with a fallback, for the many numeric settings.
int32_t reg_get_int(const char *key, int32_t def);

bool reg_has(const char *key);
void reg_clear(void);
uint32_t reg_count(void);

// Iterate, for a `reg` command that lists everything.
const char *reg_key_at(uint32_t i);

// Replace the table from `key=value` lines. Blank lines and lines beginning '#'
// are skipped. Clears first, so this is a load rather than a merge.
void reg_load(const char *text, uint32_t len);

// Write the table as `key=value` lines into buf. Returns the length written
// (excluding the terminating NUL), or the length it WOULD need if it did not
// fit — so a caller can detect truncation.
uint32_t reg_serialize(char *buf, uint32_t cap);

// Whether a set has happened since the last serialize — so the shell only writes
// flash when something actually changed.
bool reg_dirty(void);
void reg_mark_clean(void);

// --- the per-user table -------------------------------------------------------
//
// A second, smaller table holding the settings that belong to a PERSON rather
// than to the device, kept in that person's home directory and swapped in at
// login.
//
// WHICH KEYS: exactly the ones beginning "User.", and nothing else. Not the
// "Apps." namespace, which was the obvious answer and is wrong — the Nova D1
// keeps `Apps.NovaD1_PIN_sda` there, and which pin the display is wired to is a
// fact about the hardware. It does not change when somebody else logs in. So a
// package that wants a preference to follow the person names it "User.<thing>"
// and gets that behaviour; everything else stays device-wide by default, which
// is the safe direction to be wrong in.
//
// READS prefer the signed-in user's value and fall back to the device table. A
// device-wide "User." key is therefore the DEFAULT everyone starts from, which
// is what makes an existing setting survive this becoming per-user at all.
//
// WRITES with a scope active go to the user's table. Writes with NO scope are
// REFUSED for a "User." key — see reg_set. A background service running before
// anyone logs in must not quietly turn its own preference into the device
// default that everybody afterwards inherits.
#define REG_USER_MAX 24

// Whose settings are loaded, or "" when nobody is signed in.
const char *reg_scope_user(void);

// Name the scope. Does no I/O — the device layer loads the file and calls this.
void reg_scope_set(const char *user);

// Replace the user table from "key=value" lines, as reg_load does for the
// device table. Clears first.
void reg_scope_load(const char *text, uint32_t len);

// Write the user table out. SEPARATE from reg_serialize, which must go on
// emitting the device table alone — otherwise the shared registry file gains
// every user's keys and the split stops meaning anything.
uint32_t reg_scope_serialize(char *buf, uint32_t cap);

void reg_scope_clear(void);
bool reg_scope_dirty(void);
void reg_scope_mark_clean(void);

uint32_t    reg_scope_count(void);
const char *reg_scope_key_at(uint32_t i);

// Is this a key that belongs to a person? The one place the rule is written.
bool reg_is_user_key(const char *key);

#endif  // RPC_REGISTRY_H
