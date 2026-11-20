// The CYW43 radio firmware, stored deflated and inflated as it is sent.
//
// On a wireless board the cyw43 driver is built with CYW43_FW_STREAM=1 and with
// this file as its CYW43_CHIPSET_FIRMWARE_INCLUDE_FILE, in place of the header
// that carries the firmware as one 227 KB array (os/CMakeLists.txt does both).
// The driver itself is patched (os/patches/cyw43-fw-stream.patch) to take the
// firmware and the CLM from the reader below instead of from that array.
//
// Deliberately, nothing here defines fw_data. A read of the firmware that the
// patch does not cover therefore fails to compile, rather than reading from
// somewhere that no longer holds it.
//
// What the stream holds, and the checks on it, are in tools_cyw43fw.py and
// core/fwstream.h. Plain C: the driver includes this.
#ifndef RPC_CYW43_FW_H
#define RPC_CYW43_FW_H

#include <stddef.h>
#include <stdint.h>

#include "cyw43_fw_data.h"      // generated: the lengths and the stored bytes

// The stream holds the firmware the driver would pick: the Bluetooth-capable
// one when Bluetooth is linked in (every standard wireless image), the plain
// one in an image built without Bluetooth. os/CMakeLists.txt makes it from the
// right header and says which in RPC_CYW43_FW_BT; a mismatch would send the
// chip firmware the driver does not expect.
#ifndef RPC_CYW43_FW_BT
#define RPC_CYW43_FW_BT 1
#endif
#if (CYW43_ENABLE_BLUETOOTH ? 1 : 0) != RPC_CYW43_FW_BT
#error "cyw43_fw_data was made from the other firmware than the driver picks; see os/CMakeLists.txt"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CYW43_FW_WIFI 0
#define CYW43_FW_CLM  1

typedef struct FwStream cyw43_fw_stream_t;

// NULL when there is no memory for it, after saying so in the log.
cyw43_fw_stream_t *cyw43_fw_open(int which);

// The next n bytes, checked; NULL if the image is damaged. Good until the next
// read or the close.
const uint8_t *cyw43_fw_read(cyw43_fw_stream_t *z, size_t n);

// Frees it. 0 only if all of the image was read and all of it was good; a
// damaged image is reported in the log here. NULL is accepted (and is not 0).
int cyw43_fw_close(cyw43_fw_stream_t *z);

#ifdef __cplusplus
}
#endif

#endif  // RPC_CYW43_FW_H
