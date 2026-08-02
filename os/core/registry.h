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

#endif  // RPC_REGISTRY_H
