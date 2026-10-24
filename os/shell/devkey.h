// The per-device key that secrets are sealed under (see core/secret.h).
#ifndef RPC_DEVKEY_H
#define RPC_DEVKEY_H

#include <stdint.h>

#define DEVKEY_PATH "/os/device.key"

// The 32-byte key, created from the hardware random generator the first time it
// is wanted and kept in DEVKEY_PATH. Returns false if it cannot be read or made
// durable; the caller then leaves the secret as it was rather than sealing it
// under a key that would be gone after a restart.
//
// The file is deliberately not in the `backup` list. A backup is meant to leave
// the device, and a sealed password is only worth anything if its key does not
// travel with it.
bool devkey_get(uint8_t key[32]);

#endif
