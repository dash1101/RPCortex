// NFC — a PN532 over I2C, as a shell command.
//
// The pattern is bt.cpp's: the driver lives in the firmware and the Nova D1
// package reaches it through fw_shell_run and reads the text back. There is no
// NFC call in the package ABI and there is not going to be one, because the
// chip needs a bus, a timing-sensitive handshake and a frame parser, and none
// of that belongs on the far side of a sandbox boundary.
//
// Everything about the FRAMES is in core/nfcframe.cpp, where it can be given a
// deliberately broken response and checked on a host. What is left here is the
// part that needs a chip on the end of the wire.
//
// Grounded in the NXP PN532 User Manual UM0701-02 Rev. 02 (5 November 2007) and
// cross-checked against NXP AN10609_3 Rev. 1.2 (14 January 2010). Section
// numbers are quoted at each step. The v1 MicroPython driver (novamods.py,
// test_pn532 / pn532_read_card) is the other reference: it shipped and it read
// real cards, so where this differs from it the difference is deliberate and
// said so.
//
// DEVICE-UNCONFIRMED, ALL OF IT. No PN532 has been attached. What is tested is
// the framing and the card naming, on the host, in nfcframe_test. The bus
// transactions, the ready-poll handshake and the wake sequence have never run.
// What to try first, in this order:
//
//   1. `nfc status 0 1` WITH THE PANEL ALREADY RUNNING, and read the Shared
//      line before anything else. It probes 0x24 twice — once at the rate the
//      panel left the bus at, once at the 400 kHz this command uses — because
//      the Hardware screen decides whether the NFC app is reachable at all and
//      it asks at the panel's rate. If those two disagree the app is struck
//      through and told to check its wiring while this command says the reader
//      is right there, and nothing else on the device would ever say why.
//      The fix, if they do disagree, is novamodtab's i2c_present dropping the
//      rate the same way this does. That is a package change and is
//      deliberately not made here on a guess.
//   2. The Address and Firmware lines from the same run. They are separate on
//      purpose so this step can fail halfway and say so: a module with its DIP
//      switches set to SPI or HSU still answers on 0x24 and never replies to
//      GetFirmwareVersion. That was v1's most common support question.
//   3. `nfc read 5 0 1` with a card held on the antenna.
//   4. Then the same while the panel is drawing — see the bus-sharing note
//      below, which is the one thing here that could go wrong quietly.
#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "logring.h"
#include "interrupt.h"
#include "nfcframe.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hardware/i2c.h"

// Declared rather than included: rpc_app.h is the package ABI's header and this
// is firmware. The one thing wanted from it is bringing a bus up, which takes
// only scalars and so is safe to call from anywhere.
extern "C" int fw_i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned baud);

// --- the bus ------------------------------------------------------------------
//
// THE PANEL SHARES THIS BUS. On the Nova D1 the display, the RTC and the reader
// are all on I2C0, and display.cpp runs it at 1 MHz — deliberately, because a
// full frame at 400 kHz costs 23 ms and caps the device at 43 frames a second.
//
// 1 MHz is above what the PN532 is specified for. UM0701-02 § 6.2.4: "able to
// communicate with a host controller in fast mode (up to 400 KHz CLK)". So this
// command drops the bus to 400 kHz for its exchange and puts it back exactly as
// it found it afterwards.
//
// EXACTLY, and not to a number written down here. The rate is restored from the
// four registers i2c_set_baudrate writes — the two SCL counts, the spike length
// and the SDA hold — saved before the change. Restoring "1 MHz" instead would
// be display.cpp's constant copied into a second file, and the two would drift
// the first time somebody tuned the panel.
//
// The rate is changed with i2c_set_baudrate rather than by re-initialising,
// which matters: i2c_init resets the block, and resetting it underneath a
// display flush already in progress on the other core would strand that
// transfer. Re-initialising only happens when the block is not enabled at all,
// which means nothing else is using it.
//
// DEVICE-UNCONFIRMED and the first thing to watch for: two tasks on one I2C
// block. The panel is flushed by the package on the GUI task and this runs on a
// worker, so a read and a frame can genuinely overlap. Neither is destructive —
// a collision costs a garbled frame and a failed read, both of which the next
// pass repairs — but if reading a tag makes the screen tear, that is why, and
// the fix is a bus lock that both sides honour rather than anything in here.

struct NfcBus {
    unsigned bus;
    unsigned sda, scl;
    // What the bus was running at before, so it can be put back. Only the
    // registers i2c_set_baudrate touches; `saved` says whether there was
    // anything to save, which there is not when this command brought the bus
    // up itself.
    bool     saved;
    uint32_t hcnt, lcnt, spklen, sda_hold;
};

static i2c_inst_t *i2c_of(unsigned bus) {
    if (bus == 0) return i2c0;
    if (bus == 1) return i2c1;
    return nullptr;
}

// Which block a pin pair belongs to. Same arithmetic novamodtab uses, so the
// two cannot disagree about where the reader is: on RP2 the I2C instance
// alternates every two GPIOs, so SDA 0 and SDA 4 are both I2C0 and SDA 2 is
// I2C1.
static unsigned bus_of_sda(unsigned sda) { return ((sda / 2) % 2) ? 1u : 0u; }

// Whether the block is already enabled, which is the same as asking whether
// somebody else brought this bus up.
//
// Safe to read before anything has touched the block: the SDK's
// runtime_init_post_clock_resets un-resets every peripheral before main, so
// this is a register on a live block whatever else has happened. Reading a
// block still held in reset is a fault on RP2 rather than a zero, which is why
// that is worth having checked rather than assumed.
static bool bus_live(unsigned bus) {
    i2c_inst_t *i = i2c_of(bus);
    return i && (i2c_get_hw(i)->enable & I2C_IC_ENABLE_ENABLE_BITS) != 0;
}

// Take the bus to the PN532's rate. Returns false if it could not be had.
static bool bus_claim(NfcBus *b) {
    i2c_inst_t *i = i2c_of(b->bus);
    if (!i) return false;
    if (!bus_live(b->bus))
        return fw_i2c_init(b->bus, b->sda, b->scl, PN532_I2C_MAX_HZ) == 0;

    i2c_hw_t *hw = i2c_get_hw(i);
    b->hcnt     = hw->fs_scl_hcnt;
    b->lcnt     = hw->fs_scl_lcnt;
    b->spklen   = hw->fs_spklen;
    b->sda_hold = hw->sda_hold;
    b->saved    = true;
    i2c_set_baudrate(i, PN532_I2C_MAX_HZ);
    return true;
}

static void bus_release(const NfcBus &b) {
    if (!b.saved) return;
    i2c_inst_t *i = i2c_of(b.bus);
    if (!i) return;
    // The same order i2c_set_baudrate uses: disable, write the counts, enable.
    // The block will not take these while it is running.
    i2c_hw_t *hw = i2c_get_hw(i);
    hw->enable = 0;
    hw->fs_scl_hcnt = b.hcnt;
    hw->fs_scl_lcnt = b.lcnt;
    hw->fs_spklen   = b.spklen;
    hw->sda_hold    = b.sda_hold;
    hw->enable = 1;
}

// One transaction's worth of patience. Generous — the chip stretches SCL while
// it thinks — and bounded, which is the point: a blocking SDK call with no
// deadline on a bus whose slave has gone away never returns, and this runs on a
// worker task the UI is waiting on.
#define NFC_XFER_US 50000

static int bus_write(const NfcBus &b, const uint8_t *d, unsigned n) {
    i2c_inst_t *i = i2c_of(b.bus);
    if (!i) return -1;
    task_alive();
    return i2c_write_timeout_us(i, PN532_I2C_ADDR, d, n, false, NFC_XFER_US);
}

static int bus_read(const NfcBus &b, uint8_t *d, unsigned n) {
    i2c_inst_t *i = i2c_of(b.bus);
    if (!i) return -1;
    task_alive();
    return i2c_read_timeout_us(i, PN532_I2C_ADDR, d, n, false, NFC_XFER_US);
}

// Does anything answer at 0x24 at all.
//
// A one-byte write, which is what novamodtab's presence probe does — the two
// have to agree or the Hardware screen and this command will contradict each
// other about the same chip. The byte itself is ignored by the PN532: it is not
// a start code, so it never begins a frame.
static bool addr_acks(const NfcBus &b) {
    uint8_t probe = 0;
    return bus_write(b, &probe, 1) >= 0;
}

// --- the handshake --------------------------------------------------------------
//
// UM0701-02 § 6.2.4: over I2C the chip prepends a STATUS BYTE to everything the
// host reads, and bit 0 of it is RDY — "When bit RDY = 0, the PN532 has no
// frame available to be transferred to the host controller". So a read is
// always two steps: poll one byte until RDY is set, then read the frame with
// the status byte still in front of it. The parsers in nfcframe.cpp scan for
// the start code rather than assuming offset zero, which is what lets that
// leading byte be ignored instead of stripped.

static bool wait_ready(const NfcBus &b, unsigned tries) {
    for (unsigned i = 0; i < tries; i++) {
        uint8_t st = 0;
        if (bus_read(b, &st, 1) == 1 && (st & 0x01)) return true;
        if (intr_check()) return false;
        task_sleep_ms(5);
    }
    return false;
}

// Send a command and collect its answer.
//
// `body` starts with the host frame identifier. `cmd` is the command code, used
// to recognise the reply. Returns the payload length, or -1.
static int pn532_call(const NfcBus &b, const uint8_t *body, unsigned nbody,
                      uint8_t cmd, uint8_t *rx, unsigned rxcap,
                      const uint8_t **payload, unsigned ready_tries) {
    uint8_t frame[PN532_FRAME_MAX];
    const uint32_t n = pn532_frame(body, nbody, frame, sizeof(frame));
    if (!n) return -1;
    if (bus_write(b, frame, n) < 0) return -1;

    // The ACK first. § 6.2.1.3: the chip sends one as soon as the command is
    // correctly received, and it arrives before any answer does. Reading it is
    // not optional — leave it in the chip's buffer and the next read returns
    // the ACK where the response should have been.
    if (!wait_ready(b, ready_tries)) return -1;
    uint8_t ack[8];
    if (bus_read(b, ack, sizeof(ack)) < 0) return -1;
    if (!pn532_is_ack(ack, sizeof(ack))) return -1;

    if (!wait_ready(b, ready_tries)) return -1;
    const int got = bus_read(b, rx, rxcap);
    if (got < 0) return -1;
    return pn532_response(rx, (uint32_t)got, cmd, payload);
}

// Tell the chip to stop what it is doing. § 6.2.1.3: the ACK frame "is used for
// the synchronization of the packets and also for the abort mechanism".
//
// Sent when a poll runs out of time. InListPassiveTarget's retry count defaults
// to infinite (§ 7.3.4, MxRtyPassiveActivation 0xFF), so a poll that gave up
// leaves the chip still hunting with its RF field on — and the answer it
// eventually produces would be sitting in the buffer waiting to be mistaken for
// the next read's.
static void pn532_abort(const NfcBus &b) {
    static const uint8_t kAck[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
    bus_write(b, kAck, sizeof(kAck));
}

// GetFirmwareVersion, § 7.2.2 p: 73. Input D4 02, output D5 03 IC Ver Rev
// Support, with IC 0x32 for a PN532.
static bool pn532_firmware(const NfcBus &b, uint8_t *ic, uint8_t *ver, uint8_t *rev) {
    const uint8_t body[] = { PN532_TFI_HOST, PN532_CMD_FIRMWARE };
    uint8_t rx[24];
    const uint8_t *p = nullptr;
    const int n = pn532_call(b, body, sizeof(body), PN532_CMD_FIRMWARE,
                             rx, sizeof(rx), &p, 20);
    return n >= 0 && pn532_version(p, (uint32_t)n, ic, ver, rev);
}

// SAMConfiguration in normal mode, § 7.2.10 p: 89.
//
// § 3.1.3.3: LowVbat is the mode the chip starts in after reset, and the host
// "must send a SAMConfiguration command with Normal mode parameter in order to
// access other modes". Mode 0x01 is normal; the timeout only means anything in
// virtual-card mode and 0x14 is the customary 1 s; the trailing 0x01 asks for
// the IRQ pin, which nothing here uses but which is what every field driver
// sends. AN10609_3 § 2.5.2 notes the shorter "14 01" is also accepted.
//
// A FAILURE HERE IS NOT FATAL and the caller is expected to carry on. The v1
// driver never sent this command at all and read cards for a year, which is the
// evidence that the breakout modules come up out of LowVbat on their own. Given
// that, refusing to read because the chip did not answer a configuration
// command it may not need would be inventing a failure.
static bool pn532_sam_normal(const NfcBus &b) {
    const uint8_t body[] = { PN532_TFI_HOST, PN532_CMD_SAMCONFIG, 0x01, 0x14, 0x01 };
    uint8_t rx[16];
    const uint8_t *p = nullptr;
    return pn532_call(b, body, sizeof(body), PN532_CMD_SAMCONFIG,
                      rx, sizeof(rx), &p, 20) >= 0;
}

// --- reading a tag -----------------------------------------------------------------

enum ReadResult { NFC_GOT, NFC_NONE, NFC_NOCHIP, NFC_STOPPED };

static ReadResult poll_tag(const NfcBus &b, unsigned seconds, NfcTarget *out) {
    // InListPassiveTarget, § 7.3.5 p: 115. MaxTg 1 — one card is what a reader
    // screen shows and asking for two only makes the answer longer. BrTy 0x00
    // is "106 kbps type A (ISO/IEC14443 Type A)", which is every card this is
    // for. No InitiatorData: that field is only for targeting a known UID.
    const uint8_t body[] = { PN532_TFI_HOST, PN532_CMD_LISTTARGET, 0x01, 0x00 };
    uint8_t frame[PN532_FRAME_MAX];
    const uint32_t n = pn532_frame(body, sizeof(body), frame, sizeof(frame));
    if (!n || bus_write(b, frame, n) < 0) return NFC_NOCHIP;

    // The ACK. One second of patience: it comes back almost immediately when
    // the chip is listening at all, and waiting longer only delays the report
    // that it is not.
    //
    // EVERY WAY OUT OF HERE ABORTS FIRST, including the ones that look like the
    // chip is not listening. It may be listening perfectly and merely late, and
    // a poll left armed answers into the buffer after this call has given up —
    // so the NEXT read finds RDY already set, takes that stale frame as its ACK,
    // fails to recognise it and gives up too. One fault, reported twice, and it
    // presents as a reader that works every other time.
    if (!wait_ready(b, 200)) { pn532_abort(b); return NFC_NOCHIP; }
    uint8_t ack[8];
    if (bus_read(b, ack, sizeof(ack)) < 0) { pn532_abort(b); return NFC_NOCHIP; }
    if (!pn532_is_ack(ack, sizeof(ack))) { pn532_abort(b); return NFC_NOCHIP; }

    // Then wait for a card. The chip stays armed on its own — the default
    // MxRtyPassiveActivation is infinite — so this is one command and a wait,
    // not a loop of commands. v1 re-sent the poll on every pass because it had
    // no better way to keep the panel alive; here the wait is on a worker task
    // and the screen is already free.
    const uint32_t until = task_now_ms() + seconds * 1000u;
    while ((int32_t)(task_now_ms() - until) < 0) {
        if (intr_check()) { pn532_abort(b); return NFC_STOPPED; }
        uint8_t st = 0;
        if (bus_read(b, &st, 1) == 1 && (st & 0x01)) {
            // Room for NbTg, a target header and a ten-byte UID, plus framing
            // and the status byte, with slack for an ATS this does not read.
            uint8_t rx[40];
            const int got = bus_read(b, rx, sizeof(rx));
            const uint8_t *p = nullptr;
            const int payload = got > 0
                ? pn532_response(rx, (uint32_t)got, PN532_CMD_LISTTARGET, &p) : -1;
            if (payload >= 0 && pn532_target(p, (uint32_t)payload, out) > 0)
                return NFC_GOT;
            // A well-formed answer with no target in it, or a frame that did
            // not check out. Either way the chip is idle now, so re-arm — and
            // abort on the way out of any step of it, for the reason above.
            if (bus_write(b, frame, n) < 0) { pn532_abort(b); return NFC_NOCHIP; }
            if (!wait_ready(b, 200)) { pn532_abort(b); return NFC_NOCHIP; }
            if (bus_read(b, ack, sizeof(ack)) < 0) { pn532_abort(b); return NFC_NOCHIP; }
        }
        task_sleep_ms(50);
    }
    pn532_abort(b);
    return NFC_NONE;
}

// --- the command ---------------------------------------------------------------

// Where the reader is. Explicit pins win; failing that, the Nova D1's own pin
// map in the registry, which is the only place on the device that knows.
//
// There is no built-in default and there should not be: guessing a pin pair
// means driving two GPIOs that belong to something else, and on a board with
// nothing on them it produces a confident "no reader" for a reader that is
// wired somewhere this never looked.
static bool resolve(NfcBus *b, int sda, int scl) {
    if (sda < 0) sda = reg_get_int("Apps.NovaD1_PIN_sda", -1);
    if (scl < 0) scl = reg_get_int("Apps.NovaD1_PIN_scl", -1);
    if (sda < 0 || scl < 0) {
        out_err("I do not know which pins the reader is on.");
        out_multi("  Pass them - 'nfc status <sda> <scl>' - or set them once with");
        out_multi("  'd1 pins set sda <gpio>' and 'd1 pins set scl <gpio>'.");
        return false;
    }
    b->sda = (unsigned)sda;
    b->scl = (unsigned)scl;
    b->bus = bus_of_sda(b->sda);
    return true;
}

static bool num(const char *s, int *out) {
    if (!s || !*s) return false;
    char *end = nullptr;
    const long v = strtol(s, &end, 10);
    if (!end || *end || v < 0 || v > 4000000) return false;
    *out = (int)v;
    return true;
}

static int cmd_nfc(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    // Every argument after the subcommand is a number, and which is which
    // follows from how many there are. Same shape as i2cscan's positional list.
    int n[3] = { -1, -1, -1 };
    int count = 0;
    for (int i = 2; i < argc && count < 3; i++) {
        if (!num(argv[i], &n[count])) { count = -1; break; }
        count++;
    }

    if (!strcmp(sub, "status")) {
        NfcBus b;
        memset(&b, 0, sizeof(b));
        if (count < 0 || (count != 0 && count != 2)) {
            out_err("Usage: nfc status [<sda> <scl>]");
            return 1;
        }
        if (!resolve(&b, count == 2 ? n[0] : -1, count == 2 ? n[1] : -1)) return 1;

        // ASK AT THE RATE THE REST OF THE DEVICE USES, FIRST.
        //
        // The Hardware screen decides whether this app is even reachable, and
        // it decides by probing 0x24 from the package — on the live bus, at
        // whatever rate the panel left it, which is 1 MHz. This command probes
        // at 400 kHz because that is what the chip is specified for. If the
        // module answers at one rate and not the other, the two disagree: the
        // app is struck through on the home screen and told to check its
        // wiring, while `nfc status` says the reader is right there.
        //
        // That cannot be settled without a chip, so it is MEASURED and printed
        // rather than assumed away. Both answers, on their own lines.
        const bool was_live = bus_live(b.bus);
        const bool acked_shared = was_live && addr_acks(b);

        if (!bus_claim(&b)) { out_err("I2C%u would not come up.", b.bus); return 1; }

        const bool acked = addr_acks(b);
        uint8_t ic = 0, ver = 0, rev = 0;
        const bool answered = acked && pn532_firmware(b, &ic, &ver, &rev);
        bus_release(b);

        // Two separate lines, deliberately. A module whose DIP switches are set
        // for SPI or HSU still acknowledges its I2C address and never replies
        // to a command — v1 reported exactly that case as "no ACK, wrong mode?"
        // — and reporting one number for both would make the Hardware screen,
        // which only probes the address, look like it was lying.
        out_info("NFC");
        out_multi("  Chip      PN532 on I2C%u, GPIO %u/%u at %u kHz",
                  b.bus, b.sda, b.scl, (unsigned)(PN532_I2C_MAX_HZ / 1000));
        out_multi("  Address   0x%02X %s", PN532_I2C_ADDR,
                  acked ? "answered" : "did not answer");
        if (!was_live)
            out_multi("  Shared    the bus was not up - nothing else is using it");
        else if (acked_shared == acked)
            out_multi("  Shared    the same at the rate the panel uses");
        else if (acked)
            // The one that makes the app unreachable. Said in the words
            // somebody would need to act on it.
            out_multi("  Shared    silent at the panel's rate - Hardware will "
                      "call it absent");
        else
            out_multi("  Shared    answered at the panel's rate but not at "
                      "%u kHz", (unsigned)(PN532_I2C_MAX_HZ / 1000));
        if (answered && ic == 0x32)
            out_multi("  Firmware  PN532 v%u.%u", ver, rev);
        else if (answered)
            // § 7.2.2: IC is 0x32 for a PN532. Anything else answered the
            // command but is a different part, and saying its version as
            // though it were a PN532 would be the wrong kind of confident.
            out_multi("  Firmware  v%u.%u, but IC byte %02X is not a PN532", ver, rev, ic);
        else if (acked)
            out_multi("  Firmware  no reply - check the DIP switches are on I2C");
        else
            out_multi("  Firmware  not asked");
        return answered ? 0 : 1;
    }

    if (!strcmp(sub, "read")) {
        // read | read <seconds> | read <seconds> <sda> <scl>
        if (count < 0 || count == 2) {
            out_err("Usage: nfc read [<seconds>] [<sda> <scl>]");
            return 1;
        }
        unsigned secs = 5;
        if (count >= 1 && n[0] > 0) secs = (unsigned)(n[0] > 30 ? 30 : n[0]);

        NfcBus b;
        memset(&b, 0, sizeof(b));
        if (!resolve(&b, count >= 3 ? n[1] : -1, count >= 3 ? n[2] : -1)) return 1;
        if (!bus_claim(&b)) { out_err("I2C%u would not come up.", b.bus); return 1; }

        if (!addr_acks(b)) {
            bus_release(b);
            out_err("No PN532 answered at 0x%02X on I2C%u.", PN532_I2C_ADDR, b.bus);
            out_multi("  Check the wiring and that the module's DIP switches select I2C.");
            return 1;
        }
        // Asked for and not insisted on — see the note on pn532_sam_normal.
        pn532_sam_normal(b);

        NfcTarget t;
        memset(&t, 0, sizeof(t));
        const ReadResult r = poll_tag(b, secs, &t);
        bus_release(b);

        switch (r) {
            case NFC_GOT: break;
            case NFC_STOPPED:
                out_warn("Stopped.");
                return 1;
            case NFC_NOCHIP:
                out_err("The PN532 stopped answering.");
                out_multi("  It acknowledged its address and then did not reply.");
                return 1;
            default:
                out_warn("No tag.");
                out_multi("  Hold a card flat against the antenna while it reads.");
                return 1;
        }

        char uid[40], atqa[8];
        nfc_hex(t.uid, t.uid_len, uid, sizeof(uid));
        nfc_hex(t.atqa, 2, atqa, sizeof(atqa));
        out_info("Tag");
        out_multi("  UID       %s", uid);
        out_multi("  Type      %s", nfc_type_name(t.sak, t.atqa));
        out_multi("  ATQA      %s", atqa);
        out_multi("  SAK       %02X", t.sak);
        log_add(LOG_K_OK, "nfc: read a tag");
        return 0;
    }

    out_multi("Usage:");
    out_multi("  nfc status [<sda> <scl>]     is a reader there, and what firmware");
    out_multi("  nfc read [<seconds>] [<sda> <scl>]");
    out_multi("  Polls for an ISO14443-A card and reports its UID and kind.");
    out_multi("  The bus is taken to %u kHz for the exchange - the PN532's",
              (unsigned)(PN532_I2C_MAX_HZ / 1000));
    out_multi("  maximum - and put back exactly as it was afterwards.");
    return argc > 1 ? 1 : 0;
}

void nfc_register(void) {
    static const Command c{"nfc", "read an NFC tag with a PN532", cmd_nfc,
                           nullptr, LEVEL_USER};
    cmd_register(&c);
}
