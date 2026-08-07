// Desc: The OLED panel — one interface, three controllers, a page-diff push.
// File: display.cpp
#include "display.h"
#include "novacore.h"
#include "novaboard.h"

#include <string.h>

namespace nova {

// I2C control bytes. With Co = 0 the rest of the transmission is all of one
// kind, which is what lets a run of commands go out as a single transaction.
#define CTRL_CMD  0x00
#define CTRL_DATA 0x40

// 1 MHz, not the 400 kHz these panels are specified for.
//
// A full frame is 1024 bytes and at 400 kHz that is about 23 ms — enough to cap
// the device at 43 frames a second before any drawing has happened. At 1 MHz it
// is around 9, which takes the bus out of the argument entirely. The MicroPython
// suite has run these panels at 1 MHz on hardware for a year.
#define I2C_HZ 1000000

// --- init sequences ---------------------------------------------------------
//
// Carried over byte for byte from the MicroPython suite. The SH1106 sequence is
// verified on hardware; the other two are grounded in working drivers for those
// parts. A wrong byte here produces a blank or garbled panel and no error at
// all, so none of these is worth "tidying".

// SH1106. Note there is no addressing-mode command: this controller is page
// mode only, which is what show() writes anyway.
static const uint8_t kInitSH1106[] = {
    0xae,               // display off while configuring
    0xd5, 0x80,         // clock divide / oscillator
    0xa8, 0x3f,         // multiplex ratio: 64 rows
    0xd3, 0x00,         // no vertical offset
    0x40,               // start line 0
    0xad, 0x8b,         // DC-DC on (SH1106's own charge pump command)
    0xa1,               // segment re-map: column 127 -> SEG0
    0xc8,               // COM scan direction remapped
    0xda, 0x12,         // COM pins: alternative, for 128x64
    0x81, 0x80,         // contrast
    0xd9, 0x22,         // pre-charge period
    0xdb, 0x35,         // VCOMH deselect
    0xa4,               // resume from RAM
    0xa6,               // normal, not inverted
    0xaf,               // display on
};

static const uint8_t kInitSSD1306[] = {
    0xae,
    0xd5, 0x80,
    0xa8, 0x3f,
    0xd3, 0x00,
    0x40,
    0x8d, 0x14,         // charge pump on — the SSD1306 has one, the SSD1309 does not
    0x20, 0x02,         // PAGE addressing, to match how show() writes
    0xa1,
    0xc8,
    0xda, 0x12,
    0x81, 0xcf,
    0xd9, 0xf1,
    0xdb, 0x40,
    0xa4,
    0xa6,
    0xaf,
};

// SSD1309, for the 2.42" panel the reference build carries. This is the DEFAULT.
//
// The sequence itself is the MicroPython suite's, and novad1-v1.md marks that
// one verified on hardware. THIS code path has never driven a panel, so it is
// DEVICE-UNCONFIRMED in the sense that matters here: the bytes are known good,
// the code sending them is not.
//
// Two differences from the SSD1306 that both fail silently:
//
//   0xfd 0x12   COMMAND UNLOCK, and it must come FIRST. The SSD1309 powers up
//               with its command interface locked and ignores everything until
//               this arrives.
//   no 0x8d     the charge-pump command does not exist on this part; it is
//               driven from an external boost converter. Sending the SSD1306's
//               is not a no-op.
//
// One deliberate departure from stock SSD1309 drivers: they set 0x20 0x00
// (horizontal addressing). show() writes a page at a time for the diff, so this
// sets page addressing to match. Copying the stock value renders garbage.
static const uint8_t kInitSSD1309[] = {
    0xfd, 0x12,         // unlock the command interface, first of all
    0xae,
    0xd5, 0x80,
    0xa8, 0x3f,
    0xd3, 0x00,
    0x40,
    0x20, 0x02,         // PAGE addressing (see above)
    0xa1,
    0xc8,
    0xda, 0x12,
    0x81, 0xcf,
    0xd9, 0xf1,
    0xdb, 0x30,         // VCOMH ~0.83 x VCC
    0xa4,
    0xa6,
    0x2e,               // scrolling off
    0xaf,
};

// --- the panel ---------------------------------------------------------------

PanelKind panel_from_name(const char *s) {
    if (!s || !*s)                return PANEL_AUTO;
    if (nova::ieq(s, "sh1106"))   return PANEL_SH1106;
    if (nova::ieq(s, "ssd1306"))  return PANEL_SSD1306;
    if (nova::ieq(s, "ssd1309"))  return PANEL_SSD1309;
    return PANEL_AUTO;
}

const char *Display::kind_name(void) const {
    switch (kind_) {
        case PANEL_SSD1306: return "ssd1306";
        case PANEL_SH1106:  return "sh1106";
        default:            return "ssd1309";
    }
}

bool Display::cmds(const uint8_t *seq, unsigned n) {
    // One transaction for the whole run. With the control byte's Co bit clear,
    // everything after it is a command, so a 23-byte init is one write rather
    // than 23 — and each of those would be a supervisor call as well as a bus
    // transaction.
    uint8_t buf[40];
    if (n + 1 > sizeof(buf)) return false;
    buf[0] = CTRL_CMD;
    memcpy(buf + 1, seq, n);
    return fw_i2c_write(bus_, addr_, buf, n + 1, 0) >= 0;
}

bool Display::probe(uint8_t addr) {
    // A zero-length write is the usual way to ask "is anything at this address",
    // and not every controller answers it. One command byte the panel would
    // ignore anyway is safer: 0xe3 is NOP on all three parts.
    uint8_t nop[2] = { CTRL_CMD, 0xe3 };
    return fw_i2c_write(bus_, addr, nop, 2, 0) >= 0;
}

// The configured panel, or the one named. Everything below is shared; the only
// difference is where the kind comes from.
bool Display::begin_as(PanelKind kind) {
    forced_ = kind;
    bool ok = begin();
    forced_ = PANEL_AUTO;
    return ok;
}

bool Display::begin(void) {
    ready_ = false;

    int sda = board::pin(board::PIN_SDA);
    int scl = board::pin(board::PIN_SCL);
    if (sda == board::PIN_NONE || scl == board::PIN_NONE) return false;

    // Which controller the pins belong to is fixed on RP2 — there is no GPIO
    // matrix — so the bus follows from the pin rather than being configured
    // separately and getting out of step with it.
    bus_ = ((sda / 2) % 2) ? 1 : 0;
    if (fw_i2c_init(bus_, (unsigned)sda, (unsigned)scl, I2C_HZ) != 0) return false;

    // 0x3c is what nearly every module is strapped to; 0x3d is the other option
    // and costs one probe to cover.
    if      (probe(0x3c)) addr_ = 0x3c;
    else if (probe(0x3d)) addr_ = 0x3d;
    else return false;

    kind_ = forced_ != PANEL_AUTO ? forced_
                                  : panel_from_name(nova::reg(NOVA_KEY_PREFIX "Display", ""));
    const uint8_t *seq;
    unsigned n;
    switch (kind_) {
        case PANEL_SSD1306: seq = kInitSSD1306; n = sizeof(kInitSSD1306); col_offset_ = 0; break;
        case PANEL_SSD1309: seq = kInitSSD1309; n = sizeof(kInitSSD1309); col_offset_ = 0; break;
        case PANEL_SH1106:
            // The SH1106 is a 132-column part showing 128, so everything is two
            // columns in. This is the ONLY place that offset appears — a screen
            // that knew about it would be wrong on the other two panels.
            seq = kInitSH1106; n = sizeof(kInitSH1106); col_offset_ = 2;
            break;
        default:
            // NOTHING CONFIGURED MEANS SH1106, and this went the other way once
            // on the strength of a document rather than a working device.
            //
            // The bill of materials specifies a 2.42" SSD1309, so the default
            // was changed to match it — and the panel on the bench went dark and
            // stayed dark for four versions. The mechanism is the charge pump:
            // an SH1106 turns its own on with 0xad 0x8b, the SSD1309 sequence has
            // no pump command at all because that part is driven externally, and
            // an SH1106 sent the SSD1309 init therefore has no supply to its
            // panel. Every I2C write is acknowledged. Nothing reports an error.
            // The screen is simply black.
            //
            // So the default is the one that has actually lit up, and switching
            // is one command. `novad1 display test` cycles all three with a
            // pattern on each, which is the only honest way to tell them apart:
            // they share an address and answer nothing that identifies them.
            kind_ = PANEL_SH1106;
            seq = kInitSH1106; n = sizeof(kInitSH1106); col_offset_ = 2;
            break;
    }
    if (!cmds(seq, n)) return false;

    have_last_ = false;
    ready_ = true;
    return true;
}

void Display::show(const Canvas &c) {
    last_pages_ = 0;
    if (!ready_ || !c.buffer()) return;

    const uint8_t *buf = c.buffer();
    const int w = c.width();
    const int pages = (c.height() + 7) / 8;
    if (w > 128 || pages > 8) return;         // not a panel this driver knows

    // The control byte and the page, in one buffer, so a page is one write.
    uint8_t out[1 + 128];
    out[0] = CTRL_DATA;

    for (int page = 0; page < pages; page++) {
        const uint8_t *src = buf + page * w;
        if (have_last_ && memcmp(last_ + page * w, src, (unsigned)w) == 0) continue;

        uint8_t set[4] = {
            CTRL_CMD,
            (uint8_t)(0xb0 | page),                       // page address
            (uint8_t)(0x00 | (col_offset_ & 0x0f)),       // column, low nibble
            (uint8_t)(0x10 | (col_offset_ >> 4)),         // column, high nibble
        };
        // A FAILED WRITE IS NOT THE END OF THE PANEL.
        //
        // This used to set ready_ = false and give up, so one NAK — a bus
        // shared with the NFC reader and the clock, a marginal pull-up, a
        // moment of contention — turned the screen off permanently with nothing
        // said anywhere. The frame is abandoned and the next one tries again;
        // the panel is only given up on after enough consecutive failures that
        // it is clearly not there any more.
        if (fw_i2c_write(bus_, addr_, set, sizeof(set), 0) < 0) { note_fail(); return; }

        memcpy(out + 1, src, (unsigned)w);
        if (fw_i2c_write(bus_, addr_, out, (unsigned)w + 1, 0) < 0) { note_fail(); return; }
        fails_ = 0;
        last_pages_++;
    }

    // Snapshot AFTER the writes succeeded. Recording it first would mean a page
    // that failed to send was remembered as sent, and the diff would never try
    // it again — a panel stuck showing a stale row with nothing wrong anywhere.
    memcpy(last_, buf, (unsigned)(w * pages));
    have_last_ = true;
}

void Display::contrast(uint8_t value) {
    if (!ready_) return;
    uint8_t seq[2] = { 0x81, value };
    cmds(seq, sizeof(seq));
}

void Display::power(bool on) {
    if (!ready_) return;
    uint8_t seq[1] = { (uint8_t)(on ? 0xaf : 0xae) };
    cmds(seq, sizeof(seq));
    // The panel's own state changed, so what it is holding is no longer what the
    // diff thinks it sent.
    invalidate();
}

void Display::invert(bool on) {
    if (!ready_) return;
    uint8_t seq[1] = { (uint8_t)(on ? 0xa7 : 0xa6) };
    cmds(seq, sizeof(seq));
}

// Namespace scope, not a function-local static. A local static of a class type
// needs a guard variable and __cxa_guard_acquire to make its one-time
// construction safe — a symbol the firmware does not export and should not have
// to. At namespace scope with a trivial default constructor there is nothing to
// construct: it is bss, and bss is zero.
static Display g_display;

// Thirty consecutive failed frames is about a second at the idle rate, which is
// far longer than any transient and short enough that a genuinely unplugged
// panel is noticed. Giving up also drops the diff, so a panel that comes back
// gets a full repaint rather than half of one.
void Display::note_fail(void) {
    have_last_ = false;
    if (++fails_ < 30) return;
    ready_ = false;
    fw_log(1, "novad1: the panel stopped answering");
}

Display &display(void) { return g_display; }

}  // namespace nova
