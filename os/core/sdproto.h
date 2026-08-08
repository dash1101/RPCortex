// SD/SDHC cards over SPI: the PROTOCOL, with no hardware in it.
//
// Everything here is command framing, CRC, the initialisation dance and the
// block transfer. Nothing here knows what an RP2350 is. The bytes go out
// through an SdXfer the caller supplies, which on the device is the SPI
// peripheral (sdcard_rp2.cpp) and in a host test is a fake card that answers
// the way the specification says a card does.
//
// That split exists because of where the risk is. The initialisation sequence
// is the part that goes wrong, and it goes wrong QUIETLY — a card that
// half-initialises returns plausible rubbish rather than an error, and the
// single worst way to get it wrong is to read CCS backwards and address every
// block off by a factor of 512, which presents as corruption rather than as a
// bug. None of that needs a card in front of it to test. The timing, the
// electrical behaviour and the ways real cards deviate do, and they are what
// remains DEVICE-UNCONFIRMED.
//
// SPI mode rather than the four-bit SDIO mode because RP2 has no SD host
// controller: SPI is what the part can do, and it is what every microcontroller
// SD driver uses.
//
// GROUNDING. Every step below cites the SD Physical Layer Simplified
// Specification, Part 1, Version 3.01 (18 May 2010) — the section numbers are
// from that document and were read, not recalled:
//
//   §6.4.1    Power Up — "at least 74 SD clocks ... In case of SPI mode, CS
//             shall be held to high during 74 clock cycles"
//   §7.2.1    Mode Selection and Initialization — CMD0 with CS asserted enters
//             SPI mode; CMD8 before the first ACMD41; HCS in the ACMD41
//             argument and CCS in the CMD58 response; ACMD41 repeated until
//             in_idle_state is 0, with no other command in between except CMD0
//   §7.2.2    Bus Transfer Protection — CRC is off by default in SPI mode, CMD0
//             still needs a valid one (0x95), CMD8's is always checked, and
//             CMD59 turns checking on
//   §7.2.3/4  Data Read / Data Write
//   §7.3.1.3  Detailed Command Description — the SPI command table
//   §7.3.2.1  Format R1     §7.3.2.3 Format R2
//   §7.3.2.4  Format R3     §7.3.2.6 Format R7
//   §7.3.3.1  Data Response Token
//   §7.3.3.2  Start Block Tokens and Stop Tran Token
//   §7.3.3.3  Data Error Token
//   §5.1      OCR register — bit 31 power-up status, bit 30 CCS
//   §5.3.2    CSD Version 1.0 — C_SIZE [73:62], C_SIZE_MULT [49:47]
//   §5.3.3    CSD Version 2.0 — C_SIZE [69:48], capacity = (C_SIZE+1) x 512 KB
//   §4.3.13   Send Interface Condition Command (CMD8)
#ifndef RPC_SDPROTO_H
#define RPC_SDPROTO_H

#include <stdint.h>

#define SD_BLOCK 512

// The transport. Batch-oriented rather than a byte at a time, because a 512-byte
// block through a per-byte callback is 512 indirect calls for no reason.
struct SdXfer {
    void *ctx;
    // Full duplex. A null `tx` means "send 0xFF" — the idle level, which is what
    // a host sends while it is only listening. A null `rx` discards.
    void (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t len);
    // Chip select. True selects the card (drives CS low).
    void (*cs)(void *ctx, bool select);
    // Change the clock. Initialisation runs slowly and everything after it does
    // not; see sd_init.
    void (*baud)(void *ctx, uint32_t hz);
    // Give the core up. A card can take a second to initialise and the OS has
    // other things to do in that second.
    void (*yield_ms)(void *ctx, uint32_t ms);
    uint32_t (*now_ms)(void *ctx);
};

enum {
    SD_TYPE_NONE = 0,
    SD_TYPE_V1,        // pre-2.00, byte addressed, no CMD8
    SD_TYPE_V2_SC,     // 2.00 standard capacity, byte addressed
    SD_TYPE_V2_HC,     // SDHC/SDXC, BLOCK addressed
};

// Why an initialisation gave up. Reported by `sd mount` verbatim, because
// "failed" on a bus this fiddly is not a useful thing to tell somebody.
enum {
    SD_OK = 0,
    SD_ERR_NO_CARD,      // nothing answered CMD0 — an empty slot looks like this
    SD_ERR_CMD8,         // answered CMD0, then refused or garbled CMD8
    SD_ERR_INIT_TIMEOUT, // ACMD41 never finished
    SD_ERR_OCR,          // CMD58 would not read the OCR
    SD_ERR_CSD,          // CMD9 would not read the CSD
    SD_ERR_BLOCKLEN,     // a byte-addressed card refused CMD16
};

struct SdCard {
    SdXfer   x;
    uint8_t  type;
    bool     block_addressed;   // CCS from the OCR. Getting this wrong is a
                                // factor-of-512 error that reads as corruption.
    uint32_t blocks;            // capacity in 512-byte blocks
    uint8_t  csd[16];
    uint32_t crc_errors;        // read blocks whose CRC did not match
    uint32_t retries;           // reads that had to be repeated
    uint8_t  last_error;
    bool     ready;
};

// Bring a card up. False when there is no card, or there is one and it will not
// initialise; `last_error` says which.
//
// FAST when the slot is empty. A full ACMD41 loop is up to a second, and paying
// that on every `ls /sd` with no card in would make the whole thing feel broken
// — so a card that does not answer CMD0 within a short window is declared
// absent, and the long timeout is only spent on a card that has spoken.
bool sd_init(SdCard *c, const SdXfer *x);
void sd_deinit(SdCard *c);

// Read one 512-byte block. Retries once on a CRC mismatch.
bool sd_read_block(SdCard *c, uint32_t lba, void *buf);

// Write one. Separate from the read path and not used by the mount, which is
// read-only — this is here so a future writer has the block layer already
// proven, and so `sd` can be told to prove the card writes.
bool sd_write_block(SdCard *c, uint32_t lba, const void *buf);

// Is the card still there? CMD13 (SEND_STATUS, §7.3.1.3), which is one command
// and no data transfer — cheap enough to ask before an operation and on a
// timer, unlike a re-initialisation.
bool sd_alive(SdCard *c);

const char *sd_type_name(const SdCard *c);
const char *sd_error_text(uint8_t err);
uint64_t    sd_capacity_bytes(const SdCard *c);

// Exposed for the host test, which checks them against the two constants the
// specification prints: CMD0's CRC is 0x95 and CMD8's, for argument 0x1AA, is
// 0x87 (§7.2.2). If these are wrong nothing else can be right.
uint8_t  sd_crc7(const uint8_t *d, uint32_t n);
uint16_t sd_crc16(const uint8_t *d, uint32_t n);

#endif  // RPC_SDPROTO_H
