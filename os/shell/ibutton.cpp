// iButton — read a DS1990A's registration number off a bit-banged 1-Wire pin.
//
// There was no 1-Wire layer to reuse. The only other one-wire part on this
// device is the DHT, and that is NOT Dallas 1-Wire: it is a different protocol
// with a different start pulse, no addressing, no commands and no CRC, and its
// driver (apps/dht) is a package that times pulse widths. Nothing in it
// transfers here beyond the idea of timing a line, so this is written from the
// specification rather than adapted.
//
// Timings are the standard-speed column of Table 2 in the Maxim / Analog
// Devices application note AN126, "1-Wire Communication Through Software", and
// the four operations are its Table 1. Both are in core/onewire.h as named
// constants so the numbers live in one place. The command and the ROM layout
// are from the DS1990A data sheet: Read ROM is 33h, and the answer is 64 bits —
// an 8-bit family code, a 48-bit serial number, and an 8-bit CRC over the first
// 56, generated with X^8 + X^5 + X^4 + 1.
//
// AN126 lists three requirements for a software master and all three are met
// here, which is worth saying because two of them are easy to get wrong:
//
//   * the line must be open-drain with a pull-up. This never drives the pin
//     high. A low is an output driving 0 and a one is the pin turned back into
//     an input; driving a high would fight the slave and could damage both
//     ends. THE EXTERNAL PULL-UP IS NOT OPTIONAL — the internal one on RP2 is
//     around 50k, far too weak for the edges 1-Wire needs. 4.7k to 3V3.
//   * an accurate microsecond delay. busy_wait_us_32 is that.
//   * "the communication operations must not be interrupted while being
//     generated", which is why interrupts are masked inside a slot.
//
// DEVICE-UNCONFIRMED, ALL OF IT. No key and no reader have been attached, and
// the reference board has no pin assigned for one — novaboard leaves ibutton
// unset on both profiles, because the map has three pins left. What is tested
// on the host is the CRC and the family naming, in onewire_test. The bit
// timing, the presence detection and the ROM read have never run.
//
// What to try first, in this order:
//   1. `ibutton read <pin>` with NOTHING touching the reader. It must say
//      nothing answered. If it reports a ROM, the pin is floating and the CRC
//      guard is the only thing between that and a confident wrong answer.
//   2. Short the pin to ground briefly during a read: still no ROM, because a
//      held-low line reads all zeros and ow_rom_ok refuses those.
//   3. Then a real key. Read it several times — the same ROM every time is the
//      thing to confirm, since a marginal pull-up gives a different one each
//      pass and the CRC catches most but not all of that.
#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "logring.h"
#include "interrupt.h"
#include "onewire.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

// Scalars only, so it is safe to call from any task. Declared rather than
// pulling in the package ABI header from firmware.
extern "C" int fw_gpio_usable(unsigned pin);
extern "C" unsigned fw_gpio_count(void);

// --- the wire -------------------------------------------------------------------
//
// Open drain, always. `low` drives; `release` hands the line back to the
// pull-up. There is deliberately no "drive high".

static inline void ow_low(unsigned pin) {
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
}

static inline void ow_release(unsigned pin) {
    gpio_set_dir(pin, GPIO_IN);
}

static void ow_pin_init(unsigned pin) {
    gpio_init(pin);
    gpio_put(pin, 0);          // the level the pin takes whenever it is an output
    gpio_set_dir(pin, GPIO_IN);
    // The internal pull-up is far too weak to run 1-Wire on and an external one
    // is required regardless. It is enabled anyway so that a pin with nothing
    // whatsoever attached idles high and reads as "no device" instead of
    // floating and reading as whatever the last edge left behind.
    gpio_pull_up(pin);
}

// AN126 Table 1, Reset: delay G; drive low, delay H; release, delay I; sample
// (0 means a device is present); delay J.
//
// Interrupts are masked from the falling edge to the sample. The low itself
// only has a minimum, so being stretched by an interrupt would be harmless —
// but the sample after I does not: the presence pulse is over within a few
// hundred microseconds of the release, and a sample that arrives late reads an
// idle line and reports no key. That is the silent failure this masking is for.
static bool ow_reset(unsigned pin) {
    if (OW_T_G) busy_wait_us_32(OW_T_G);

    const uint32_t save = save_and_disable_interrupts();
    ow_low(pin);
    busy_wait_us_32(OW_T_H);
    ow_release(pin);
    busy_wait_us_32(OW_T_I);
    const bool present = gpio_get(pin) == 0;
    restore_interrupts(save);

    // The rest of the recovery slot, outside the mask: nothing is sampled in it
    // and the only requirement is that it happens before the next operation.
    busy_wait_us_32(OW_T_J);
    return present;
}

// AN126 Table 1, Write 1: drive low, delay A; release, delay B.
//                Write 0: drive low, delay C; release, delay D.
static void ow_write_bit(unsigned pin, int bit) {
    const uint32_t save = save_and_disable_interrupts();
    ow_low(pin);
    busy_wait_us_32(bit ? OW_T_A : OW_T_C);
    ow_release(pin);
    busy_wait_us_32(bit ? OW_T_B : OW_T_D);
    restore_interrupts(save);
}

// AN126 Table 1, Read: drive low, delay A; release, delay E; sample; delay F.
static int ow_read_bit(unsigned pin) {
    const uint32_t save = save_and_disable_interrupts();
    ow_low(pin);
    busy_wait_us_32(OW_T_A);
    ow_release(pin);
    busy_wait_us_32(OW_T_E);
    const int bit = gpio_get(pin) ? 1 : 0;
    busy_wait_us_32(OW_T_F);
    restore_interrupts(save);
    return bit;
}

// 1-Wire is little-endian on the wire: least significant bit of each byte
// first, and the family code is the first byte out. So a registration number
// read in arrival order is already family, serial, CRC.
static void ow_write_byte(unsigned pin, uint8_t v) {
    for (int i = 0; i < 8; i++) { ow_write_bit(pin, v & 1); v = (uint8_t)(v >> 1); }
}

static uint8_t ow_read_byte(unsigned pin) {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) if (ow_read_bit(pin)) v |= (uint8_t)(1u << i);
    return v;
}

// One reset, one Read ROM, eight bytes. False when nothing answered the reset.
//
// Read ROM only works with ONE device on the bus. With two, both answer at once
// and the wired-AND of their registration numbers comes back — which is very
// often still a valid-looking eight bytes and usually fails the CRC, but not
// always. An iButton reader is a single-slot contact, so that case is out of
// scope here; a bus with several parts on it wants Search ROM (F0h) instead.
static bool ow_read_rom(unsigned pin, uint8_t *rom) {
    if (!ow_reset(pin)) return false;
    ow_write_byte(pin, OW_CMD_READ_ROM);
    for (unsigned i = 0; i < OW_ROM_BYTES; i++) rom[i] = ow_read_byte(pin);
    return true;
}

// --- the command ---------------------------------------------------------------

enum KeyResult { KEY_GOT, KEY_BAD_CRC, KEY_NONE, KEY_STOPPED };

static KeyResult poll_key(unsigned pin, unsigned seconds, uint8_t *rom) {
    const uint32_t until = task_now_ms() + seconds * 1000u;
    KeyResult worst = KEY_NONE;
    while ((int32_t)(task_now_ms() - until) < 0) {
        if (intr_check()) return KEY_STOPPED;
        task_alive();
        if (ow_read_rom(pin, rom)) {
            if (ow_rom_ok(rom)) return KEY_GOT;
            // Something is there and what came back does not check out. Keep
            // trying — a key being pressed onto a contact bounces, and the
            // first read of a physical touch is very often a partial one — but
            // remember that it happened, so a whole read's worth of failures is
            // reported as a bad read rather than as an empty contact.
            worst = KEY_BAD_CRC;
        }
        // Fifty milliseconds between attempts. Fast enough that a deliberate
        // touch cannot be missed, slow enough that this is not sitting with
        // interrupts off for any meaningful fraction of the time.
        task_sleep_ms(50);
    }
    return worst;
}

static bool num(const char *s, int *out) {
    if (!s || !*s) return false;
    char *end = nullptr;
    const long v = strtol(s, &end, 10);
    if (!end || *end || v < 0 || v > 999) return false;
    *out = (int)v;
    return true;
}

// Which pin. Explicit wins, then the Nova D1's pin map. There is no default:
// novaboard assigns no pin for an iButton on either profile, and bit-banging a
// guessed GPIO means driving something else's line.
static int resolve_pin(int given) {
    if (given >= 0) return given;
    const int v = reg_get_int("Apps.NovaD1_PIN_ibutton", -1);
    if (v >= 0) return v;
    out_err("I do not know which pin the reader is on.");
    out_multi("  Pass it - 'ibutton read <gpio>' - or set it once with");
    out_multi("  'd1 pins set ibutton <gpio>'.");
    return -1;
}

static int cmd_ibutton(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "read";

    if (strcmp(sub, "read") != 0) {
        out_multi("Usage:");
        out_multi("  ibutton read [<gpio>] [<seconds>]");
        out_multi("  Waits for a DS1990A key and reports its 64-bit ROM.");
        out_multi("  The data line wants a 4.7k pull-up to 3V3; the internal");
        out_multi("  one is far too weak to run 1-Wire on.");
        return argc > 1 ? 1 : 0;
    }

    int given = -1, secs = 5;
    if (argc > 2 && !num(argv[2], &given)) {
        out_err("'%s' is not a pin number.", argv[2]);
        return 1;
    }
    if (argc > 3 && !num(argv[3], &secs)) {
        out_err("'%s' is not a number of seconds.", argv[3]);
        return 1;
    }
    if (secs < 1) secs = 1;
    if (secs > 30) secs = 30;

    const int pin = resolve_pin(given);
    if (pin < 0) return 1;
    if (!fw_gpio_usable((unsigned)pin)) {
        if ((unsigned)pin >= fw_gpio_count())
            out_err("This board has pins 0-%u.", fw_gpio_count() - 1);
        else
            out_err("GPIO %d belongs to the board, not to you.", pin);
        return 1;
    }

    ow_pin_init((unsigned)pin);

    uint8_t rom[OW_ROM_BYTES];
    memset(rom, 0, sizeof(rom));
    const KeyResult r = poll_key((unsigned)pin, (unsigned)secs, rom);

    char hex[32];
    unsigned at = 0;
    for (unsigned i = 0; i < OW_ROM_BYTES && at + 3 < sizeof(hex); i++)
        at += (unsigned)snprintf(hex + at, sizeof(hex) - at, "%s%02X", at ? " " : "", rom[i]);

    switch (r) {
        case KEY_GOT:
            out_info("iButton");
            out_multi("  ROM       %s", hex);
            out_multi("  Family    %02X  %s", rom[0], ow_family_name(rom[0]));
            out_multi("  CRC       ok");
            log_add(LOG_K_OK, "ibutton: read a key");
            return 0;

        case KEY_BAD_CRC:
            // Reported as a failure rather than as a ROM with a warning on it.
            // A registration number is the whole point of the part, and one
            // that did not check out is not a registration number — handing it
            // over with a note beside it is how a wrong key gets written down.
            out_err("The key did not check out.");
            out_multi("  ROM       %s", hex);
            out_multi("  CRC       bad - got %02X, wanted %02X",
                      rom[OW_ROM_BYTES - 1], ow_crc8(rom, OW_ROM_BYTES - 1));
            out_multi("  Hold the key still against both contacts, and check");
            out_multi("  the line has a 4.7k pull-up.");
            return 1;

        case KEY_STOPPED:
            out_warn("Stopped.");
            return 1;

        default:
            out_warn("Nothing answered.");
            out_multi("  Touch the key to the reader while it reads.");
            return 1;
    }
}

void ibutton_register(void) {
    static const Command c{"ibutton", "read a DS1990A iButton key", cmd_ibutton,
                           nullptr, LEVEL_USER};
    cmd_register(&c);
}
