// Pulling a device name out of a Bluetooth advertisement.
//
// An advertisement is a packed list of length-prefixed records, at most 31
// bytes, arriving from whatever is nearby — which is to say, from something
// this device has no control over and no reason to trust. A malformed one is
// not a hypothetical: a length byte that runs past the end of the packet is the
// obvious way to make a scanner read memory that is not its own.
//
// So the parsing lives here, away from the radio, where it can be given a
// deliberately broken packet and checked.
#ifndef RPC_BTNAME_H
#define RPC_BTNAME_H

#include <stdint.h>

#define BT_NAME_MAX 24

// AD types, from the Bluetooth assigned numbers.
#define BT_AD_NAME_SHORT  0x08
#define BT_AD_NAME_FULL   0x09

// Find the device name in `data`. Prefers the complete name over the shortened
// one, since a device advertising both means the short one to be a fallback.
// Returns the length written, or 0 when there is no name. `out` is always
// terminated.
uint32_t bt_name_from_ad(const uint8_t *data, uint32_t len, char *out, uint32_t cap);

#endif  // RPC_BTNAME_H
