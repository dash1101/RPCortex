// Desc: Reading a contact card's memory into a .nfc dump, driven by a reader.
// File: novanfcdump.h
//
// The part that walks a card once the firmware has selected it: for a Mifare Classic, try the
// well-known keys on each sector and read what opens; for an NTAG / Ultralight, read the version,
// the signature and the pages. It is the exact walk the MicroPython suite did (novamods.py,
// pn532_dump_classic / pn532_dump_ntag), ported so it can run against a model of a card on the
// host - the firmware's two verbs (select, exchange) come in as function pointers, so a fake card
// answers them in a test and the real PN532 answers them on the board.
//
// Reading a card you hold is what every one of these tools does; the keys below are the ones
// published in every NFC library (libnfc, libfreefare, the Flipper) as the factory and transport
// defaults, and a sector that uses none of them stays "??" in the file - unreadable, and said so.
//
// DEVICE-UNCONFIRMED: the walk is proven against the fake card in novacodec_test; it has not run
// against a PN532.
#ifndef NOVA_NFC_DUMP_H
#define NOVA_NFC_DUMP_H

#include <stdint.h>

#include "novanfc.h"

namespace nova {
namespace nfcdump {

// What a selected card looks like to the engine - the same fields fw_nfc_select fills.
struct Tag {
    uint8_t uid[10];
    uint8_t uid_len;
    uint8_t atqa[2];
    uint8_t sak;
};

// The firmware's two verbs, as the engine sees them. The return contracts match fw_nfc_select and
// fw_nfc_exchange exactly (1/0/negative, and a byte count or negative). `reset` asks select to
// cycle the RF field so a halted card answers again. cancelled() lets a long dump stop, and
// progress() is for the screen's counter.
struct Reader {
    int  (*select)(void *ctx, Tag *out, bool reset);
    int  (*exchange)(void *ctx, const uint8_t *tx, unsigned tx_len, uint8_t *rx, unsigned rx_cap);
    bool (*cancelled)(void *ctx);
    void (*progress)(void *ctx, unsigned done, unsigned total);
    void  *ctx;
};

enum { DUMP_ISO3A, DUMP_ULTRALIGHT, DUMP_CLASSIC };

// The largest card this reads: a Mifare Classic 4K is 256 blocks of 16 bytes.
#define DUMP_MEM_MAX   4096
#define DUMP_UNITS_MAX 256

struct Dump {
    int      type;                      // DUMP_*
    char     kind[16];                  // "1K", "4K", "NTAG215", ...
    Tag      tag;
    unsigned units;                     // blocks (16 B) or pages (4 B)
    unsigned unit_bytes;                // 16 or 4
    unsigned read_units;                // how many were actually recovered
    uint8_t  mem[DUMP_MEM_MAX];
    uint16_t known[DUMP_UNITS_MAX];     // per unit, which bytes are real (bit i = byte i)
    // NTAG / Ultralight extras, when they were read.
    bool     has_sig;
    uint8_t  sig[32];
    bool     has_ver;
    uint8_t  mifare_version[8];
};

enum { DUMP_OK, DUMP_NOCARD, DUMP_CANCELLED, DUMP_UNSUPPORTED };

// Walk the card `first` (already selected by the caller) into `out`. Returns a DUMP_* code. An
// ISO14443-3A card that is not a Classic or an NTAG has no memory to read and comes back
// DUMP_UNSUPPORTED with its identity still in `out->tag`.
int run(const Reader &r, const Tag &first, Dump *out);

// Fill one unit (block or page) from a finished dump, for the .nfc writer's Getter. `*known` is
// cleared where a byte was not recovered.
bool unit_getter(void *dump, unsigned index, uint8_t *out, uint16_t *known);

}  // namespace nfcdump
}  // namespace nova

#endif  // NOVA_NFC_DUMP_H
