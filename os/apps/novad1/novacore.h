// Desc: Registry access, storage paths and the small helpers every module needs.
// File: novacore.h
//
// The leaf. Nothing here may include another Nova module — this is the bottom of
// the layer map and the one file allowed to be imported by anything, which only
// works while it depends on nothing itself.
//
// Ported from novacore.py, minus the memory toolkit. is_oom / reclaim /
// largest_block existed because MicroPython's non-compacting collector made
// "90 KB free" and "cannot open a TLS socket" simultaneously true. That problem
// class does not exist here, so the functions do not either.
#ifndef NOVA_CORE_H
#define NOVA_CORE_H

#include "rpc_app.h"

// Every setting this suite owns lives under one prefix, so `reg` groups them and
// nothing else on the device can collide. Keys are written out in full at the
// call site — "Apps.NovaD1_Home", not NOVA_KEY("Home") — because a key that can
// be grepped for is worth more than one that is assembled.
#define NOVA_KEY_PREFIX "Apps.NovaD1_"

// The longest registry value the firmware will hold (REG_VAL_MAX), plus room for
// the terminator. Every reg_get buffer in the suite is this size, so no call
// site has to guess and none of them truncates differently from the others.
#define NOVA_VAL_MAX 97

// Where the device keeps what it collects. One root, made at first run.
#define NOVA_ROOT      "/nova"
#define NOVA_CODES     "/nova/codes"
#define NOVA_SCRIPTS   "/nova/scripts"
#define NOVA_LOGS      "/nova/logs"

namespace nova {

// --- settings ---------------------------------------------------------------
//
// reg() returns the value or `def` — never null, so a caller can use the result
// straight away. The string is copied into a per-call rotating buffer, because
// the firmware's own pointer is unreadable from inside the sandbox.
const char *reg(const char *key, const char *def);
int         reg_int(const char *key, int def);
bool        reg_bool(const char *key, bool def);
bool        reg_is(const char *key, const char *value, bool def_match);

void reg_set(const char *key, const char *value);
void reg_set_int(const char *key, int value);
void reg_set_bool(const char *key, bool value);

// Write the registry to flash. Deliberately separate from reg_set: a settings
// screen changing six values should cost one flash write, not six.
void reg_save(void);

// --- text -------------------------------------------------------------------
//
// The handful of string operations the suite does over and over. All of them
// write into a caller-owned buffer and all of them terminate, because a
// truncated string that is still a string is recoverable and one that is not
// is a fault somewhere else entirely.

// Copy at most cap-1 bytes and terminate. Returns what was actually copied.
unsigned copy(char *dst, unsigned cap, const char *src);

// Truncate to `chars` characters, ending in ".." when something was cut. For
// list rows, where the alternative is drawing past the edge of the panel.
void ellipsize(char *dst, unsigned cap, const char *src, unsigned chars);

// Case-insensitive compare, because the registry stores what a user typed.
bool ieq(const char *a, const char *b);

// Split "a,b,c" — returns the number of fields found, filling `out` with
// pointers into a copy the caller owns. Used for the home list, favourites and
// category overrides, which are all comma-separated in the registry.
unsigned split_csv(char *editable, char **out, unsigned max);

// Is `needle` one of the comma-separated fields in `csv`?
bool csv_has(const char *csv, const char *needle);

// Add or remove a field, in place. Returns true when the string changed, which
// is what decides whether a flash write is worth doing.
bool csv_add(char *csv, unsigned cap, const char *field);
bool csv_remove(char *csv, const char *field);

// --- paths ------------------------------------------------------------------

// Make the storage tree if it is not there. Cheap to call at every boot.
void paths_init(void);

// Build "/nova/codes/<cat>/<name>" and friends without every call site doing its
// own snprintf and getting the separator wrong on one of them.
void path_join(char *out, unsigned cap, const char *a, const char *b);
void path_join3(char *out, unsigned cap, const char *a, const char *b, const char *c);

// --- time -------------------------------------------------------------------

// "14:32" / "2:32 PM" per Apps.NovaD1_Clock24, and "Mon 5 Aug". Both write into
// the caller's buffer and both work when the clock was never set — they render
// "--:--" rather than a confident wrong time, because a device that shows
// 00:00 as though it meant it is worse than one that admits it does not know.
bool time_hhmm(char *out, unsigned cap);
bool time_date(char *out, unsigned cap);

}  // namespace nova

#endif  // NOVA_CORE_H
