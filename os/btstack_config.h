// btstack's build configuration.
//
// btstack is compiled from source into this firmware, so this file decides how
// large it is. That matters more here than in an example: the firmware slot is
// 1 MB and the OS already fills three quarters of it, so every feature switched
// on below has to earn its space.
//
// Both radios come from the same CYW43439, which is why Bluetooth is possible
// at all — and it is the one capability that MicroPython could not reach.
// MicroPython's `bluetooth` module is BLE only, so Classic (and therefore
// anything that plays audio) needed either C or a custom firmware build. This
// is that build.
//
// What is ON and why:
//
//   BLE            scanning, advertising, connecting. The common case, and the
//                  smallest — most of what a tool like this wants.
//   Classic        inquiry, naming, and the profiles a media player needs.
//                  Considerably larger than BLE; measured, not guessed.
//   L2CAP / RFCOMM the transports the Classic profiles sit on.
//
// Deliberately OFF: mesh, BNEP/PAN networking, HID host, and the audio codecs.
// SBC in particular is added by the media package rather than the OS, so a
// device that never plays audio does not carry the decoder.
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// --- what is enabled --------------------------------------------------------

#define ENABLE_BLE
#define ENABLE_CLASSIC
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION

// Secure connections, because pairing without them is pairing that anyone
// nearby can listen to.
#define ENABLE_LE_SECURE_CONNECTIONS

// Say what went wrong. The alternative is a stack that fails silently, and
// diagnosing a radio problem without any log at all is guesswork.
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_P256

// --- sizes ------------------------------------------------------------------
//
// Every one of these is memory that exists whether or not it is used, so they
// are set to what this device plausibly needs rather than to what is generous.

#define MAX_NR_HCI_CONNECTIONS          3
#define MAX_NR_L2CAP_SERVICES           4
#define MAX_NR_L2CAP_CHANNELS           4
#define MAX_NR_RFCOMM_MULTIPLEXERS      2
#define MAX_NR_RFCOMM_SERVICES          2
#define MAX_NR_RFCOMM_CHANNELS          2
#define MAX_NR_GATT_CLIENTS             2
#define MAX_NR_SM_LOOKUP_ENTRIES        3
#define MAX_NR_SERVICE_RECORD_ITEMS     4
#define MAX_NR_WHITELIST_ENTRIES        4
#define MAX_NR_LE_DEVICE_DB_ENTRIES     4
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 4

// The largest ACL packet. 1021 is the full Classic size; anything smaller means
// fragmenting every audio frame, which costs more in CPU than it saves in RAM.
#define HCI_ACL_PAYLOAD_SIZE            1021
// Six, not four: the GATT client needs that much for a long characteristic
// read and refuses to compile below it.
#define HCI_INCOMING_PRE_BUFFER_SIZE    6

// The CYW43 transport prepends a four-byte header to everything it sends, and
// wants its chunks aligned. Both are the driver's requirements rather than
// btstack's, and it refuses to compile without them.
#define HCI_OUTGOING_PRE_BUFFER_SIZE    4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT    4

// Two of each, so a transfer in one direction does not stall the other.
#define MAX_NR_HCI_ACL_BUFFERS          4

#define NVM_NUM_DEVICE_DB_ENTRIES       4
#define NVM_NUM_LINK_KEYS               4

// btstack keeps its own pool rather than reaching for malloc, which suits this
// OS: a radio stack allocating from the shared heap during a download is how a
// fragmented heap turns into a failed TLS handshake.
#define HAVE_MALLOC

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_BTSTACK_STDIN 0

#endif  // BTSTACK_CONFIG_H
