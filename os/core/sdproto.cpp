// The SD SPI protocol. Section numbers are the SD Physical Layer Simplified
// Specification, Part 1, Version 3.01 — see sdproto.h for the full list.
#include "sdproto.h"

#include <string.h>

// --- commands, from §7.3.1.3 Detailed Command Description --------------------

#define CMD0   0    // GO_IDLE_STATE          R1
#define CMD8   8    // SEND_IF_COND           R7
#define CMD9   9    // SEND_CSD               R1 + a 16-byte data block
#define CMD12  12   // STOP_TRANSMISSION      R1b
#define CMD13  13   // SEND_STATUS            R2
#define CMD16  16   // SET_BLOCKLEN           R1
#define CMD17  17   // READ_SINGLE_BLOCK      R1 + data
#define CMD24  24   // WRITE_BLOCK            R1 + data
#define CMD55  55   // APP_CMD                R1
#define CMD58  58   // READ_OCR               R3
#define CMD59  59   // CRC_ON_OFF             R1
#define ACMD41 41   // SD_SEND_OP_COND        R1

// R1, §7.3.2.1. Bit 7 is always 0, which is how a response is told from the
// 0xFF the card idles at.
#define R1_IDLE            0x01
#define R1_ILLEGAL_COMMAND 0x04

// §7.3.3.2 Start Block Tokens and Stop Tran Token.
#define TOKEN_START_BLOCK  0xFE
#define TOKEN_STOP_TRAN    0xFD

// §7.3.3.1 Data Response Token: xxx0sss1, '010' accepted.
#define DATA_RESP_MASK     0x1F
#define DATA_RESP_ACCEPTED 0x05

// How long each phase is given.
//
// The CMD0 window is short ON PURPOSE: an empty slot answers nothing, and this
// is the whole cost of finding that out. A card that has answered CMD0 gets the
// full second the specification allows for ACMD41.
#define PROBE_MS       40      // is anything in the slot at all
#define INIT_MS      1200      // §7.2.1: cards take up to a second
#define READ_MS       250      // waiting for a data token
#define BUSY_MS       500      // waiting out a write

// §4.2 Card Identification Mode caps the clock at 400 kHz until the card is
// initialised. Going faster here works on the card in front of you and not on
// somebody else's.
#define INIT_HZ     400000u
#define RUN_HZ    12000000u

// --- CRC ---------------------------------------------------------------------

// CRC7, x^7 + x^3 + 1. The command's seventh byte is (crc << 1) | 1.
uint8_t sd_crc7(const uint8_t *d, uint32_t n) {
    uint8_t crc = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        for (int j = 0; j < 8; j++) {
            crc = (uint8_t)(crc << 1);
            if ((b ^ crc) & 0x80) crc ^= 0x09;
            b = (uint8_t)(b << 1);
        }
    }
    return (uint8_t)(crc & 0x7F);
}

// CRC16-CCITT, x^16 + x^12 + x^5 + 1, initial value 0 — what a data block
// carries in its last two bytes (§7.3.3.2).
uint16_t sd_crc16(const uint8_t *d, uint32_t n) {
    uint16_t crc = 0;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// --- the wire ----------------------------------------------------------------

static inline void xf(SdCard *c, const uint8_t *tx, uint8_t *rx, uint32_t n) {
    c->x.xfer(c->x.ctx, tx, rx, n);
}

static uint8_t rx1(SdCard *c) {
    uint8_t b = 0xFF;
    xf(c, nullptr, &b, 1);
    return b;
}

static void clocks(SdCard *c, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) (void)rx1(c);
}

// The card holds MISO low while it is busy. Waiting for 0xFF is how a host asks
// "have you finished the last thing".
static bool wait_idle(SdCard *c, uint32_t ms) {
    uint32_t start = c->x.now_ms(c->x.ctx);
    while (true) {
        if (rx1(c) == 0xFF) return true;
        if (c->x.now_ms(c->x.ctx) - start >= ms) return false;
        c->x.yield_ms(c->x.ctx, 1);
    }
}

// Send a command and collect R1. `extra` trailing bytes are read into `tail`
// for the responses that carry them: 4 for R3 (CMD58) and R7 (CMD8), 1 for R2
// (CMD13). Returns 0xFF when the card never answered.
static uint8_t send_cmd(SdCard *c, uint8_t cmd, uint32_t arg,
                        uint8_t *tail, int extra) {
    uint8_t f[6];
    f[0] = (uint8_t)(0x40 | cmd);          // §7.3.1.1: '01' then the index
    f[1] = (uint8_t)(arg >> 24);
    f[2] = (uint8_t)(arg >> 16);
    f[3] = (uint8_t)(arg >> 8);
    f[4] = (uint8_t)arg;
    f[5] = (uint8_t)((sd_crc7(f, 5) << 1) | 1);
    xf(c, f, nullptr, 6);

    // NCR: the response arrives within 0 to 8 bytes (§7.3.2). Ten is the
    // conventional margin and costs ten clock bytes on a card that is not going
    // to answer at all.
    uint8_t r = 0xFF;
    for (int i = 0; i < 10; i++) {
        r = rx1(c);
        if (!(r & 0x80)) break;
    }
    if (tail && extra > 0) {
        for (int i = 0; i < extra; i++) tail[i] = rx1(c);
    }
    return r;
}

// A command sent inside a transaction of its own, which is what every caller
// outside the initialisation wants.
static uint8_t cmd_txn(SdCard *c, uint8_t cmd, uint32_t arg,
                       uint8_t *tail, int extra) {
    c->x.cs(c->x.ctx, true);
    if (!wait_idle(c, BUSY_MS)) { c->x.cs(c->x.ctx, false); clocks(c, 1); return 0xFF; }
    uint8_t r = send_cmd(c, cmd, arg, tail, extra);
    c->x.cs(c->x.ctx, false);
    // Eight clocks with CS high, so the card can finish whatever the last
    // command started. Skipping this is one of the classic intermittent faults.
    clocks(c, 1);
    return r;
}

// ACMD is CMD55 followed by the command itself (§7.2.11).
static uint8_t send_acmd(SdCard *c, uint8_t cmd, uint32_t arg) {
    uint8_t r = send_cmd(c, CMD55, 0, nullptr, 0);
    if (r & 0x80) return r;                    // no answer at all
    return send_cmd(c, cmd, arg, nullptr, 0);
}

// Wait for a start-block token and read `len` bytes plus the two CRC bytes.
static bool read_data(SdCard *c, uint8_t *buf, uint32_t len, bool check_crc) {
    uint32_t start = c->x.now_ms(c->x.ctx);
    uint8_t t;
    while (true) {
        t = rx1(c);
        if (t == TOKEN_START_BLOCK) break;
        // §7.3.3.3 Data Error Token: the top four bits are zero. Anything else
        // that is not 0xFF is a protocol violation, and either way there is no
        // data coming.
        if (t != 0xFF) return false;
        if (c->x.now_ms(c->x.ctx) - start >= READ_MS) return false;
    }
    xf(c, nullptr, buf, len);
    uint8_t crc[2];
    xf(c, nullptr, crc, 2);
    if (!check_crc) return true;
    uint16_t want = (uint16_t)((crc[0] << 8) | crc[1]);
    if (sd_crc16(buf, len) != want) { c->crc_errors++; return false; }
    return true;
}

// --- initialisation, §7.2.1 --------------------------------------------------

bool sd_init(SdCard *c, const SdXfer *x) {
    SdXfer keep = *x;
    memset(c, 0, sizeof(*c));
    c->x = keep;
    c->last_error = SD_ERR_NO_CARD;

    c->x.baud(c->x.ctx, INIT_HZ);

    // §6.4.1: at least 74 clocks with CS HIGH before the first command. Cards
    // need this to finish their own power-up, and skipping it works on the card
    // you have and not on somebody else's.
    c->x.cs(c->x.ctx, false);
    clocks(c, 10);                            // 80 clocks

    // CMD0 with CS asserted is what selects SPI mode at all (§7.2.1). Its CRC
    // is a constant, 0x95, because the card is still in SD mode and checking.
    c->x.cs(c->x.ctx, true);
    bool idle = false;
    uint32_t start = c->x.now_ms(c->x.ctx);
    while (c->x.now_ms(c->x.ctx) - start < PROBE_MS) {
        if (send_cmd(c, CMD0, 0, nullptr, 0) == R1_IDLE) { idle = true; break; }
        clocks(c, 1);
        c->x.yield_ms(c->x.ctx, 1);
    }
    if (!idle) {
        c->x.cs(c->x.ctx, false);
        clocks(c, 1);
        c->last_error = SD_ERR_NO_CARD;
        return false;
    }

    // CMD8 decides version 1 from version 2, and it is MANDATORY before the
    // first ACMD41 on anything that supports it (§7.2.1). VHS = 0001b for
    // 2.7-3.6 V (§4.3.13, Table 4-16) and 0xAA as the check pattern, which the
    // same section recommends.
    bool v2 = false;
    uint8_t r7[4];
    uint8_t r = send_cmd(c, CMD8, 0x000001AAu, r7, 4);
    if (r & 0x80) {
        c->x.cs(c->x.ctx, false); clocks(c, 1);
        c->last_error = SD_ERR_CMD8;
        return false;
    }
    if (r & R1_ILLEGAL_COMMAND) {
        v2 = false;                            // legacy card: no CMD8, no HCS
    } else {
        // The card echoes the voltage range and the check pattern back. A
        // mismatch means the link is not carrying what was sent, and going on
        // from there produces nonsense rather than an error.
        if ((r7[2] & 0x0F) != 0x01 || r7[3] != 0xAA) {
            c->x.cs(c->x.ctx, false); clocks(c, 1);
            c->last_error = SD_ERR_CMD8;
            return false;
        }
        v2 = true;
    }

    // §7.2.2: "Host should enable CRC verification before issuing ACMD41."
    // Cards that predate it answer illegal-command and are no worse off.
    (void)send_cmd(c, CMD59, 1, nullptr, 0);

    // ACMD41 until the card stops saying idle. HCS (bit 30) tells a v2 card the
    // host can address blocks; a v1 card ignores it, and a standard-capacity
    // card ignores it too.
    uint32_t arg = v2 ? 0x40000000u : 0;
    start = c->x.now_ms(c->x.ctx);
    bool ready = false;
    while (c->x.now_ms(c->x.ctx) - start < INIT_MS) {
        uint8_t a = send_acmd(c, ACMD41, arg);
        if (a == 0) { ready = true; break; }
        if (a & 0x80) break;                   // stopped answering entirely
        // §7.2.1: while repeating ACMD41 the host shall issue no other command
        // except CMD0. So this loop does nothing else — but it does LET THE BUS
        // GO while it waits. The Nova D1 profile puts two radios on the same
        // SPI0 as the card, and holding CS low across a second of yielding
        // would turn one card's initialisation into a second in which nothing
        // else on the bus can be spoken to. Deasserting between commands is
        // what every command here already does; this loop was the exception.
        c->x.cs(c->x.ctx, false);
        clocks(c, 1);
        c->x.yield_ms(c->x.ctx, 10);
        c->x.cs(c->x.ctx, true);
    }
    if (!ready) {
        c->x.cs(c->x.ctx, false); clocks(c, 1);
        c->last_error = SD_ERR_INIT_TIMEOUT;
        return false;
    }

    // CMD58, and the one bit everything downstream depends on.
    //
    // OCR bit 30 is CCS (§5.1). CCS=1 means the card is SDHC/SDXC and CMD17
    // takes a BLOCK number; CCS=0 means it takes a BYTE offset. Reading this
    // backwards does not fail — it reads the right data from a place 512 times
    // too far away, and looks exactly like a corrupt filesystem.
    c->block_addressed = false;
    if (v2) {
        uint8_t ocr[4];
        if (send_cmd(c, CMD58, 0, ocr, 4) != 0) {
            c->x.cs(c->x.ctx, false); clocks(c, 1);
            c->last_error = SD_ERR_OCR;
            return false;
        }
        // Bit 31 is the power-up status bit and CCS is only valid once it is
        // set (§5.1). ACMD41 returning 0 above says initialisation finished, so
        // this is a belt to that brace.
        if (!(ocr[0] & 0x80)) {
            c->x.cs(c->x.ctx, false); clocks(c, 1);
            c->last_error = SD_ERR_OCR;
            return false;
        }
        c->block_addressed = (ocr[0] & 0x40) != 0;
        c->type = c->block_addressed ? SD_TYPE_V2_HC : SD_TYPE_V2_SC;
    } else {
        c->type = SD_TYPE_V1;
    }

    // A byte-addressed card can have any block length; fix it at 512 so the
    // rest of this only ever deals in sectors. SDHC ignores CMD16 — its block
    // length is fixed at 512 whatever anybody sets (§7.2.3).
    if (!c->block_addressed) {
        if (send_cmd(c, CMD16, SD_BLOCK, nullptr, 0) != 0) {
            c->x.cs(c->x.ctx, false); clocks(c, 1);
            c->last_error = SD_ERR_BLOCKLEN;
            return false;
        }
    }

    // The CSD arrives as a 16-byte data block, not as a response (§7.2.6).
    if (send_cmd(c, CMD9, 0, nullptr, 0) != 0 || !read_data(c, c->csd, 16, false)) {
        c->x.cs(c->x.ctx, false); clocks(c, 1);
        c->last_error = SD_ERR_CSD;
        return false;
    }

    // Capacity. The two CSD versions compute it completely differently and the
    // structure field says which (§5.3.1).
    uint8_t ver = (uint8_t)(c->csd[0] >> 6);
    if (ver == 1) {
        // §5.3.3, C_SIZE at [69:48]: capacity = (C_SIZE + 1) * 512 KB, which is
        // (C_SIZE + 1) * 1024 blocks of 512 bytes.
        uint32_t csize = (((uint32_t)c->csd[7] & 0x3F) << 16) |
                         ((uint32_t)c->csd[8] << 8) | c->csd[9];
        c->blocks = (csize + 1) * 1024;
    } else {
        // §5.3.2, C_SIZE at [73:62], C_SIZE_MULT at [49:47], READ_BL_LEN at
        // [83:80]: BLOCKNR = (C_SIZE + 1) * 2^(C_SIZE_MULT + 2), each of
        // 2^READ_BL_LEN bytes.
        uint32_t csize = (((uint32_t)c->csd[6] & 0x03) << 10) |
                         ((uint32_t)c->csd[7] << 2) | (uint32_t)(c->csd[8] >> 6);
        uint32_t mult = (uint32_t)(((c->csd[9] & 0x03) << 1) | (c->csd[10] >> 7));
        uint32_t read_bl_len = (uint32_t)(c->csd[5] & 0x0F);
        uint64_t bytes = (uint64_t)(csize + 1) * ((uint32_t)1 << (mult + 2)) *
                         ((uint32_t)1 << read_bl_len);
        c->blocks = (uint32_t)(bytes / SD_BLOCK);
    }

    c->x.cs(c->x.ctx, false);
    clocks(c, 1);

    // Only NOW is it safe to go fast (§7.2.1 puts the clock change after
    // initialisation, and §4.2 caps it at 400 kHz before).
    c->x.baud(c->x.ctx, RUN_HZ);
    c->ready = true;
    c->last_error = SD_OK;
    return true;
}

void sd_deinit(SdCard *c) {
    SdXfer keep = c->x;
    memset(c, 0, sizeof(*c));
    c->x = keep;
}

// --- blocks ------------------------------------------------------------------

static uint32_t addr_of(const SdCard *c, uint32_t lba) {
    return c->block_addressed ? lba : lba * SD_BLOCK;
}

static bool read_once(SdCard *c, uint32_t lba, uint8_t *buf) {
    c->x.cs(c->x.ctx, true);
    if (!wait_idle(c, BUSY_MS)) { c->x.cs(c->x.ctx, false); clocks(c, 1); return false; }
    bool ok = false;
    if (send_cmd(c, CMD17, addr_of(c, lba), nullptr, 0) == 0)
        ok = read_data(c, buf, SD_BLOCK, true);
    c->x.cs(c->x.ctx, false);
    clocks(c, 1);
    return ok;
}

bool sd_read_block(SdCard *c, uint32_t lba, void *buf) {
    if (!c->ready || !buf) return false;
    if (c->blocks && lba >= c->blocks) return false;
    if (read_once(c, lba, (uint8_t *)buf)) return true;
    // One retry. A single bad block on a bus shared with two radios is a glitch;
    // two in a row is the card, and pretending otherwise turns a removal into a
    // loop.
    c->retries++;
    return read_once(c, lba, (uint8_t *)buf);
}

bool sd_write_block(SdCard *c, uint32_t lba, const void *buf) {
    if (!c->ready || !buf) return false;
    if (c->blocks && lba >= c->blocks) return false;

    c->x.cs(c->x.ctx, true);
    bool ok = false;
    if (wait_idle(c, BUSY_MS) && send_cmd(c, CMD24, addr_of(c, lba), nullptr, 0) == 0) {
        // §7.2.4: one byte of gap, then the start token, the data, and the CRC.
        uint8_t pre[2] = { 0xFF, TOKEN_START_BLOCK };
        xf(c, pre, nullptr, 2);
        xf(c, (const uint8_t *)buf, nullptr, SD_BLOCK);
        uint16_t crc = sd_crc16((const uint8_t *)buf, SD_BLOCK);
        uint8_t cb[2] = { (uint8_t)(crc >> 8), (uint8_t)crc };
        xf(c, cb, nullptr, 2);
        // §7.3.3.1 Data Response Token: xxx0sss1, with '010' in the status bits
        // meaning accepted. It follows the CRC directly, but a card is allowed
        // to idle first and some do — so this looks for the shape (bit 4 clear,
        // bit 0 set) rather than assuming the very next byte is it.
        uint8_t resp = 0xFF;
        for (int i = 0; i < 16; i++) {
            resp = rx1(c);
            if ((resp & 0x11) == 0x01) break;
        }
        if ((resp & DATA_RESP_MASK) == DATA_RESP_ACCEPTED)
            ok = wait_idle(c, BUSY_MS);        // the card programs while busy
    }
    c->x.cs(c->x.ctx, false);
    clocks(c, 1);
    return ok;
}

bool sd_alive(SdCard *c) {
    if (!c->ready) return false;
    uint8_t r2;
    uint8_t r = cmd_txn(c, CMD13, 0, &r2, 1);
    return r == 0 && r2 == 0;
}

const char *sd_type_name(const SdCard *c) {
    switch (c->type) {
        case SD_TYPE_V1:    return "SD (v1)";
        case SD_TYPE_V2_SC: return "SDSC (v2)";
        case SD_TYPE_V2_HC: return "SDHC/SDXC";
        default:            return "none";
    }
}

const char *sd_error_text(uint8_t err) {
    switch (err) {
        case SD_OK:               return "ok";
        case SD_ERR_NO_CARD:      return "no card answered";
        case SD_ERR_CMD8:         return "the card garbled CMD8";
        case SD_ERR_INIT_TIMEOUT: return "the card never finished initialising";
        case SD_ERR_OCR:          return "could not read the OCR";
        case SD_ERR_CSD:          return "could not read the CSD";
        case SD_ERR_BLOCKLEN:     return "the card refused a 512-byte block length";
        default:                  return "unknown";
    }
}

uint64_t sd_capacity_bytes(const SdCard *c) {
    return (uint64_t)c->blocks * SD_BLOCK;
}
