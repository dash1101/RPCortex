// Desc: Flipper Zero .nfc (version 4) files - written, read, and a card classified.
// File: novanfc.h
//
// Ported from novanfc.py, the oracle the host test compares against. The files are
// the ones a Flipper writes, so a card moves between the two devices.
//
// The MicroPython version parsed a file into a list of records and wrote the list
// back out, which is a copy of the whole file in RAM. A Mifare 4K dump is a
// couple of hundred lines and a package has 12 KB, so this works on the text where
// it sits and writes through a Sink, a line at a time.
//
// WHAT THIS DOES NOT DO is get a card's memory: the firmware's `nfc` command
// returns a UID and a type and keeps everything else. A dump needs InDataExchange
// there first (PAR-70); until then only a card's identity can be written.
#ifndef NOVA_NFC_H
#define NOVA_NFC_H

#include "novacodec.h"

namespace nova {
namespace nfc {

#define NFC_DT_ISO3A      "ISO14443-3A"
#define NFC_DT_ULTRALIGHT "NTAG/Ultralight"
#define NFC_DT_CLASSIC    "Mifare Classic"

// --- reading ---------------------------------------------------------------------------------

// "AA BB CC" or "AABBCC" or "AA:BB" to bytes; a "??" byte is 0. Returns how many.
unsigned unhex(const char *s, uint8_t *out, unsigned cap);

// The value of the first "key: value" line whose key is `key`, trimmed. False when
// there is none. Blank lines and comments are not keys.
bool get(const char *text, const char *key, char *out, unsigned cap);

struct Info {
    char     device_type[24];
    uint8_t  uid[10];
    unsigned uid_len;
    uint8_t  atqa[2];
    unsigned atqa_len;
    uint8_t  sak;
    unsigned memory_lines;      // how many "Page N" / "Block N" lines the file carries
};

// The identity of a card from its file: type, UID, ATQA, SAK, and how much memory
// is in it. True when it names a UID.
bool read_info(const char *text, Info *out);

// --- writing ---------------------------------------------------------------------------------

// Whatever supplies the memory: fill `out` with page/block `index`. `*known` arrives as 0xFFFF
// (every byte real) and the getter clears the bit of any byte it does not have - bit 0 is the
// first byte - so a block whose first six bytes could not be read is written as "?? ?? ?? ?? ?? ??"
// followed by the rest. Return false when none of it is known, and "??" is written for all of it.
//
// Partly-known blocks are what a Mifare Classic sector trailer comes to: the card never gives up
// Key A, so the file says so rather than carrying six zeros that look like a key.
typedef bool (*Getter)(void *ctx, unsigned index, uint8_t *out, uint16_t *known);

// A card that is only identified: UID, ATQA, SAK.
bool write_iso14443a(const codec::Sink &k, const uint8_t *uid, unsigned uid_len,
                     const uint8_t *atqa, uint8_t sak);

struct UltralightExtras {
    const uint8_t *signature;       // 32 bytes, or null for zeros
    const uint8_t *mifare_version;  // 8 bytes, or null for the default
    uint32_t       counters[3];
    uint8_t        tearing[3];
    int            pages_read;      // -1: all of them
};

// NTAG / Ultralight, `pages` of 4 bytes, in the field order the Flipper spec gives.
bool write_ultralight(const codec::Sink &k, const uint8_t *uid, unsigned uid_len,
                      const uint8_t *atqa, uint8_t sak, const char *ul_type,
                      unsigned pages, Getter page, void *ctx,
                      const UltralightExtras *extras);

// Mifare Classic, `blocks` of 16 bytes. The type comes before the data format
// version, which is where the spec has it.
bool write_classic(const codec::Sink &k, const uint8_t *uid, unsigned uid_len,
                   const uint8_t *atqa, uint8_t sak, const char *mc_type,
                   unsigned blocks, Getter block, void *ctx);

// --- naming -----------------------------------------------------------------------------------

struct Kind {
    const char *device_type;    // one of the NFC_DT_ strings
    const char *sub_type;       // "1K", "NTAG215", ... or null
};

// A best guess from the anticollision answer. Conservative: anything not recognised
// is ISO14443-3A, which still saves the UID. The ATQA check ignores byte order on
// purpose - the PN532's order is not confirmed on a card yet, and a swap must not
// quietly turn an NTAG into a UID-only card.
Kind identify(uint8_t sak, const uint8_t *atqa, unsigned atqa_len, unsigned mem_pages);

// 64 blocks for a 1K, 256 for a 4K, 20 for a Mini.
unsigned classic_block_count(const char *mc_type);

}  // namespace nfc
}  // namespace nova

#endif  // NOVA_NFC_H
