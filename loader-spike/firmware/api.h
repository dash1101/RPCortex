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

// The same table addressed by position, for sandboxed packages. One cannot
// branch into the firmware directly — flash is unreachable from unprivileged
// code — so it names a function by index and the supervisor call performs the
// jump. api_index_of returns -1 for a name that is not exported; api_addr_at
// returns 0 for an index that is not in the table, and that bounds check is
// what stops a package naming something arbitrary.
int      api_index_of(const char *name);
uint32_t api_addr_at(uint32_t index);

#endif  // RPC_API_H
