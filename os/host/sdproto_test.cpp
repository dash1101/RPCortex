// The SD initialisation dance, against a card that answers the way the
// specification says one does.
//
// There is no card attached and there may never be one while this is being
// written, so the parts that can be checked here are checked here: the command
// framing, the two CRCs, the order of the sequence, and — the one that matters
// most — whether an SDHC card ends up ADDRESSED BY BLOCK and a standard
// capacity card BY BYTE. Getting that backwards does not produce an error. It
// reads the right data from a place 512 times too far away and presents as a
// corrupt filesystem, which is a fortnight of looking in the wrong place.
//
// The fake card below is written from the specification (Part 1 v3.01, §7.2.1
// and §7.3), not from sdproto.cpp, and it is deliberately fussy: it refuses a
// command whose CRC7 is wrong, refuses one sent with the card unselected, and
// refuses to answer at all until it has been given its 74 clocks. Each of those
// is a real card's behaviour and each is a way to be wrong that otherwise only
// shows up on a bench.
//
// What is NOT covered, and cannot be: timing, voltage, a real card's quirks,
// and whether the bus works at all. See sdcard_rp2.cpp for what to run first.
#include "../core/sdproto.h"

#include <stdio.h>
#include <string.h>
#include <deque>
#include <vector>

static int checks, fails;
static void ck(bool c, const char *w) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", w); fails++; }
}

// --- a card that behaves ------------------------------------------------------

struct FakeCard {
    // How this one is meant to behave.
    bool     supports_cmd8   = true;    // false: a pre-2.00 card
    bool     high_capacity   = true;    // CCS in the OCR
    int      acmd41_busy     = 3;       // how many times it says "still idle"
    bool     never_answers   = false;   // an empty slot
    bool     bad_cmd8_echo   = false;
    bool     never_ready     = false;   // ACMD41 that never completes
    int      corrupt_reads   = 0;       // this many reads get a wrong CRC16

    // What actually happened, for the test to look at.
    bool     seen_cmd0 = false;
    bool     crc_enabled = false;
    uint32_t last_read_addr = 0xFFFFFFFF;
    uint32_t clocks_before_first_cmd = 0;
    uint32_t init_hz = 0, run_hz = 0;
    int      baud_changes = 0;
    bool     cmd_while_deselected = false;
    bool     bus_released_during_init = false;
    bool     bad_crc7 = false;
    int      acmd41_calls = 0;
    bool     saw_cmd16 = false;
    std::vector<uint8_t> last_write;

    // State.
    bool     selected = false;
    bool     spi_mode = false;
    bool     app_cmd = false;
    uint32_t now = 0;
    uint8_t  csd[16];
    std::deque<uint8_t> out;
    std::vector<uint8_t> frame;
    // Set when CMD24 has been accepted. The data does not start until the
    // response has been read out, which is why this is a flag and not a byte
    // count: the host's own reads of R1 are bytes going IN too, and counting
    // them as data is the trap the first version of this fell into.
    bool     pending_write = false;
    std::vector<uint8_t> data_in;

    std::vector<uint8_t> disk;      // a tiny backing store for CMD17/CMD24

    FakeCard() {
        memset(csd, 0, sizeof(csd));
        set_csd_v2(32767);          // 16 GB
        disk.resize(4 * SD_BLOCK, 0);
        for (size_t i = 0; i < disk.size(); i++) disk[i] = (uint8_t)(i * 7 + 3);
    }

    // §5.3.3: CSD_STRUCTURE = 01b, C_SIZE at [69:48].
    void set_csd_v2(uint32_t c_size) {
        memset(csd, 0, sizeof(csd));
        csd[0] = 0x40;
        csd[5] = 0x09;                              // READ_BL_LEN = 9, fixed
        csd[7] = (uint8_t)((c_size >> 16) & 0x3F);
        csd[8] = (uint8_t)(c_size >> 8);
        csd[9] = (uint8_t)c_size;
    }
    // §5.3.2: CSD_STRUCTURE = 00b, C_SIZE at [73:62], C_SIZE_MULT at [49:47].
    void set_csd_v1(uint32_t c_size, uint32_t mult, uint32_t read_bl_len) {
        memset(csd, 0, sizeof(csd));
        csd[0] = 0x00;
        csd[5] = (uint8_t)(read_bl_len & 0x0F);
        csd[6] = (uint8_t)((c_size >> 10) & 0x03);
        csd[7] = (uint8_t)((c_size >> 2) & 0xFF);
        csd[8] = (uint8_t)((c_size & 0x03) << 6);
        csd[9] = (uint8_t)(((mult >> 1) & 0x03));
        csd[10] = (uint8_t)((mult & 1) << 7);
    }

    void push(uint8_t b) { out.push_back(b); }
    void push_r1(uint8_t r1) {
        push(0xFF);                 // NCR: a byte of nothing before the answer
        push(r1);
    }
    void push_block(const uint8_t *d, uint32_t n, bool corrupt) {
        push(0xFF);
        push(0xFE);                 // §7.3.3.2 start block token
        for (uint32_t i = 0; i < n; i++) push(d[i]);
        uint16_t crc = sd_crc16(d, n);
        if (corrupt) crc = (uint16_t)(crc ^ 0x5A5A);
        push((uint8_t)(crc >> 8));
        push((uint8_t)crc);
    }

    void command(const uint8_t *f) {
        if (!selected) cmd_while_deselected = true;
        uint8_t want = (uint8_t)((sd_crc7(f, 5) << 1) | 1);
        // CMD0 and CMD8 always have their CRC checked; once CMD59 has turned
        // checking on, so does everything else (§7.2.2).
        uint8_t idx = (uint8_t)(f[0] & 0x3F);
        if ((idx == 0 || idx == 8 || crc_enabled) && f[5] != want) {
            bad_crc7 = true;
            push_r1(0x08);          // R1 CRC error
            return;
        }
        uint32_t arg = ((uint32_t)f[1] << 24) | ((uint32_t)f[2] << 16) |
                       ((uint32_t)f[3] << 8) | f[4];
        bool was_app = app_cmd;
        app_cmd = false;

        if (was_app && idx == 41) {                 // ACMD41
            acmd41_calls++;
            if (never_ready) { push_r1(0x01); return; }
            if (acmd41_busy > 0) { acmd41_busy--; push_r1(0x01); return; }
            push_r1(0x00);
            return;
        }

        switch (idx) {
            case 0:                                  // GO_IDLE_STATE
                seen_cmd0 = true;
                spi_mode = true;
                push_r1(0x01);
                return;
            case 8:                                  // SEND_IF_COND
                if (!supports_cmd8) { push_r1(0x05); return; }   // idle + illegal
                push(0xFF);
                push(0x01);
                push(0x00); push(0x00);
                push(bad_cmd8_echo ? 0x02 : (uint8_t)((arg >> 8) & 0x0F));
                push(bad_cmd8_echo ? 0x55 : (uint8_t)(arg & 0xFF));
                return;
            case 55:                                 // APP_CMD
                app_cmd = true;
                push_r1(acmd41_busy > 0 || never_ready ? 0x01 : 0x00);
                return;
            case 58: {                               // READ_OCR, R3
                push(0xFF);
                push(0x00);
                uint8_t b0 = 0x80;                   // power-up complete
                if (high_capacity) b0 |= 0x40;       // CCS
                push(b0); push(0xFF); push(0x80); push(0x00);
                return;
            }
            case 59:                                 // CRC_ON_OFF
                crc_enabled = (arg & 1) != 0;
                push_r1(0x00);
                return;
            case 16:                                 // SET_BLOCKLEN
                saw_cmd16 = true;
                push_r1(arg == SD_BLOCK ? 0x00 : 0x40);
                return;
            case 9:                                  // SEND_CSD
                push_r1(0x00);
                push_block(csd, 16, false);
                return;
            case 13:                                 // SEND_STATUS, R2
                push(0xFF); push(0x00); push(0x00);
                return;
            case 17: {                               // READ_SINGLE_BLOCK
                last_read_addr = arg;
                push_r1(0x00);
                uint32_t lba = high_capacity ? arg : arg / SD_BLOCK;
                static uint8_t tmp[SD_BLOCK];
                memset(tmp, 0, sizeof(tmp));
                if ((size_t)(lba + 1) * SD_BLOCK <= disk.size())
                    memcpy(tmp, disk.data() + (size_t)lba * SD_BLOCK, SD_BLOCK);
                bool corrupt = corrupt_reads > 0;
                if (corrupt) corrupt_reads--;
                push_block(tmp, SD_BLOCK, corrupt);
                return;
            }
            case 24:                                 // WRITE_BLOCK
                last_read_addr = arg;
                push_r1(0x00);
                pending_write = true;
                data_in.clear();
                return;
            default:
                push_r1(0x04);                       // illegal command
                return;
        }
    }

    void byte_in(uint8_t b) {
        if (never_answers) return;
        if (pending_write) {
            // §7.2.4: the card waits for the START BLOCK TOKEN and ignores
            // whatever idle bytes come before it — including the ones the host
            // clocked out while reading R1. Keying on the token rather than
            // counting bytes is both what a card does and the only way to get
            // this boundary right.
            if (data_in.empty() && b != 0xFE) return;
            data_in.push_back(b);
            if (data_in.size() >= 1 + SD_BLOCK + 2) {
                last_write.assign(data_in.begin() + 1, data_in.begin() + 1 + SD_BLOCK);
                pending_write = false;
                push(0xFF);
                push(0x05);          // §7.3.3.1 data accepted
                push(0x00);          // busy for a byte
                push(0xFF);
            }
            return;
        }
        if (frame.empty()) {
            if ((b & 0xC0) != 0x40) return;          // idle filler
            frame.push_back(b);
            return;
        }
        frame.push_back(b);
        if (frame.size() == 6) {
            uint8_t f[6];
            memcpy(f, frame.data(), 6);
            frame.clear();
            command(f);
        }
    }
};

static void fx(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t len) {
    FakeCard *c = (FakeCard *)ctx;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t o = 0xFF;
        if (!c->out.empty() && !c->never_answers) { o = c->out.front(); c->out.pop_front(); }
        if (rx) rx[i] = o;
        uint8_t in = tx ? tx[i] : 0xFF;
        if (!c->selected && !c->seen_cmd0 && in == 0xFF) c->clocks_before_first_cmd += 8;
        c->byte_in(in);
    }
    c->now += (len + 63) / 64;
}
static void fcs(void *ctx, bool sel) {
    FakeCard *c = (FakeCard *)ctx;
    // Did the host let go of the bus while it was waiting out ACMD41? On the
    // reference board two radios share this SPI, so a card that holds CS low
    // for the whole second the specification allows is a second in which
    // nothing else on the bus can be reached.
    if (!sel && c->acmd41_calls > 0 && c->acmd41_busy > 0)
        c->bus_released_during_init = true;
    c->selected = sel;
}
static void fbaud(void *ctx, uint32_t hz) {
    FakeCard *c = (FakeCard *)ctx;
    if (c->baud_changes == 0) c->init_hz = hz; else c->run_hz = hz;
    c->baud_changes++;
}
static void fyield(void *ctx, uint32_t ms) { ((FakeCard *)ctx)->now += ms ? ms : 1; }
static uint32_t fnow(void *ctx) { return ((FakeCard *)ctx)->now; }

static SdXfer xfer_for(FakeCard &c) {
    SdXfer x{};
    x.ctx = &c; x.xfer = fx; x.cs = fcs; x.baud = fbaud;
    x.yield_ms = fyield; x.now_ms = fnow;
    return x;
}

// --- the checks ---------------------------------------------------------------

static void test_crc(void) {
    printf("  the two CRC constants the specification prints\n");
    // §7.2.2 gives a valid CMD0 in full: 0x40 0x00 0x00 0x00 0x00 0x95. If this
    // is wrong then nothing that follows can be right, and the failure is a
    // card that simply never answers.
    uint8_t cmd0[5] = { 0x40, 0, 0, 0, 0 };
    ck(((sd_crc7(cmd0, 5) << 1) | 1) == 0x95, "CMD0's CRC byte is 0x95");
    uint8_t cmd8[5] = { 0x48, 0x00, 0x00, 0x01, 0xAA };
    ck(((sd_crc7(cmd8, 5) << 1) | 1) == 0x87, "CMD8 with argument 0x1AA is 0x87");
    // CRC16-CCITT with a zero initial value: the classic check vector.
    ck(sd_crc16((const uint8_t *)"123456789", 9) == 0x31C3,
       "CRC16-CCITT of '123456789' is 0x31C3");
}

static void test_sdhc(void) {
    printf("  an SDHC card\n");
    FakeCard card;
    card.set_csd_v2(32767);                 // 16 GB
    SdCard sd;
    SdXfer x = xfer_for(card);
    ck(sd_init(&sd, &x), "SDHC initialises");
    ck(sd.last_error == SD_OK, "and says so");
    ck(!card.bad_crc7, "every command carried a CRC the card accepted");
    ck(!card.cmd_while_deselected, "no command was sent with the card unselected");
    ck(card.clocks_before_first_cmd >= 74,
       "at least 74 clocks with CS high before the first command (6.4.1)");
    ck(card.crc_enabled, "CRC checking was turned on before ACMD41 (7.2.2)");
    ck(card.acmd41_calls >= 4, "ACMD41 was repeated until the card stopped saying idle");
    ck(card.bus_released_during_init,
       "and the bus was let go between attempts, not held for the whole second");
    ck(sd.block_addressed, "CCS=1 means BLOCK addressing");
    ck(sd.type == SD_TYPE_V2_HC, "reported as SDHC/SDXC");
    ck(!card.saw_cmd16, "an SDHC card is not told a block length; it has no choice");
    ck(sd.blocks == 32768u * 1024u, "capacity from CSD v2: (C_SIZE+1) x 512 KB");
    ck(sd_capacity_bytes(&sd) == 16ull * 1024 * 1024 * 1024, "16 GB");
    ck(card.init_hz <= 400000u, "initialisation runs at 400 kHz or slower (4.2)");
    ck(card.run_hz > 400000u, "and the clock goes up only afterwards");

    uint8_t buf[SD_BLOCK];
    ck(sd_read_block(&sd, 2, buf), "a block reads");
    ck(card.last_read_addr == 2, "CMD17 got a BLOCK number, not a byte offset");
    ck(memcmp(buf, card.disk.data() + 2 * SD_BLOCK, SD_BLOCK) == 0, "with the right bytes");
    ck(sd_alive(&sd), "CMD13 says the card is still there");

    uint8_t w[SD_BLOCK];
    for (int i = 0; i < SD_BLOCK; i++) w[i] = (uint8_t)(255 - i);
    ck(sd_write_block(&sd, 1, w), "a block writes");
    ck(card.last_write.size() == SD_BLOCK &&
       memcmp(card.last_write.data(), w, SD_BLOCK) == 0, "with the right bytes");
}

static void test_sdsc_v1(void) {
    printf("  a pre-2.00 standard capacity card\n");
    FakeCard card;
    card.supports_cmd8 = false;
    card.high_capacity = false;
    // §5.3.2's own worked example: a 32 MB card with a 512-byte block is
    // C_SIZE = 2047, C_SIZE_MULT = 3, READ_BL_LEN = 9.
    card.set_csd_v1(2047, 3, 9);
    SdCard sd;
    SdXfer x = xfer_for(card);
    ck(sd_init(&sd, &x), "a v1 card initialises");
    ck(sd.type == SD_TYPE_V1, "recognised as v1");
    ck(!sd.block_addressed, "and addressed by BYTE");
    ck(card.saw_cmd16, "a byte-addressed card is pinned to 512-byte blocks");
    ck(sd.blocks == 65536u, "capacity from CSD v1: 32 MB");

    uint8_t buf[SD_BLOCK];
    ck(sd_read_block(&sd, 3, buf), "a block reads");
    ck(card.last_read_addr == 3u * SD_BLOCK,
       "CMD17 got a BYTE offset — the factor of 512 that looks like corruption");
    ck(memcmp(buf, card.disk.data() + 3 * SD_BLOCK, SD_BLOCK) == 0, "with the right bytes");
}

static void test_sdsc_v2(void) {
    printf("  a version 2.00 standard capacity card\n");
    FakeCard card;
    card.supports_cmd8 = true;
    card.high_capacity = false;             // answered CMD8, but CCS is 0
    card.set_csd_v1(2047, 3, 9);
    SdCard sd;
    SdXfer x = xfer_for(card);
    ck(sd_init(&sd, &x), "initialises");
    ck(sd.type == SD_TYPE_V2_SC, "recognised as v2 standard capacity");
    ck(!sd.block_addressed, "CCS=0 means BYTE addressing even on a v2 card");
}

static void test_empty_slot(void) {
    printf("  an empty slot\n");
    FakeCard card;
    card.never_answers = true;
    SdCard sd;
    SdXfer x = xfer_for(card);
    uint32_t t0 = card.now;
    ck(!sd_init(&sd, &x), "nothing initialises");
    ck(sd.last_error == SD_ERR_NO_CARD, "and it says there is no card");
    // The point of the short window. A full ACMD41 timeout here would be a
    // second of nothing on every `ls /sd` with the slot empty.
    ck(card.now - t0 < 200, "and gives up quickly rather than waiting out ACMD41");
    ck(!sd.ready, "nothing is marked ready");
    uint8_t buf[SD_BLOCK];
    ck(!sd_read_block(&sd, 0, buf), "reading an uninitialised card does nothing");
    ck(!sd_alive(&sd), "and it is not alive");
}

static void test_failures(void) {
    printf("  cards that answer and still cannot be used\n");

    FakeCard bad_echo;
    bad_echo.bad_cmd8_echo = true;
    SdCard sd;
    SdXfer x1 = xfer_for(bad_echo);
    ck(!sd_init(&sd, &x1), "a garbled CMD8 echo is refused");
    ck(sd.last_error == SD_ERR_CMD8, "and named");

    FakeCard stuck;
    stuck.never_ready = true;
    SdXfer x2 = xfer_for(stuck);
    ck(!sd_init(&sd, &x2), "a card that never finishes ACMD41 is given up on");
    ck(sd.last_error == SD_ERR_INIT_TIMEOUT, "and named");
    ck(stuck.acmd41_calls > 5, "after really trying");
}

static void test_crc_recovery(void) {
    printf("  a block that arrives wrong\n");
    FakeCard card;
    card.corrupt_reads = 1;
    SdCard sd;
    SdXfer x = xfer_for(card);
    ck(sd_init(&sd, &x), "initialises");
    uint8_t buf[SD_BLOCK];
    ck(sd_read_block(&sd, 0, buf), "one bad CRC is retried and the retry succeeds");
    ck(sd.crc_errors == 1, "the bad block was counted");
    ck(sd.retries == 1, "and the retry was counted");
    ck(memcmp(buf, card.disk.data(), SD_BLOCK) == 0, "the bytes are the right ones");

    card.corrupt_reads = 99;
    ck(!sd_read_block(&sd, 0, buf), "a card that keeps getting it wrong fails the read");
    ck(sd.crc_errors == 3, "both attempts counted");
}

int main(void) {
    printf("sdproto_test - the SD card init sequence, against a fake card\n");
    test_crc();
    test_sdhc();
    test_sdsc_v1();
    test_sdsc_v2();
    test_empty_slot();
    test_failures();
    test_crc_recovery();
    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
