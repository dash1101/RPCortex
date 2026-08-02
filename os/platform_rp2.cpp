// Per-target glue that the pure cores need. On the RP2350/RP2040 the salt RNG is
// the SDK's hardware-backed generator; the host test provides its own.
#include "pico/rand.h"
#include <stdint.h>

extern "C" uint32_t rpc_rand32(void) { return get_rand_32(); }
