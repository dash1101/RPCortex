#ifndef RPC_API_H
#define RPC_API_H

#include <stdint.h>

// Address of an exported firmware symbol, or 0 if the firmware does not export
// it. This is the entire surface an app can reach: anything not in here is an
// unresolved symbol and the app is refused at load time rather than crashing
// later on a null call.
uint32_t api_lookup(const char *name);

// For the report: how wide the ABI is, and what it costs.
uint32_t api_symbol_count(void);

// A fingerprint of the ABI dispatch table over its first `count` exported
// symbols, in table order, by NAME. A package in a flash slot reaches firmware
// by INDEX, so the meaning of every baked index is exactly "which name sits at
// that position"; this hashes that. A slot records it at install for the count
// the table then had, and at open the running firmware recomputes it over the
// same prefix — a match means every index the slot baked still names the same
// function. Append-only additions go past the recorded count and leave the
// prefix untouched, which is why a slot survives them; a removal or a reorder
// changes the prefix, which is what the slot guard catches. `count` above the
// table size is clamped to it. See pkgslot_open.
uint32_t api_abi_prefix_crc(uint32_t count);

// The same table addressed by position, for sandboxed packages. One cannot
// branch into the firmware directly — flash is unreachable from unprivileged
// code — so it names a function by index and the supervisor call performs the
// jump. api_index_of returns -1 for a name that is not exported; api_addr_at
// returns 0 for an index that is not in the table, and that bounds check is
// what stops a package naming something arbitrary.
int      api_index_of(const char *name);
uint32_t api_addr_at(uint32_t index);

#endif  // RPC_API_H
