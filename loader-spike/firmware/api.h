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

#endif  // RPC_API_H
