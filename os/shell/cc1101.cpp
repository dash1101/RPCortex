// CC1101 sub-GHz radio — the `subghz` shell command.
//
// A firmware driver, not a package: the Nova D1 GUI has to reach it, and a
// package cannot call another package. So the driver lives here as a
// self-registering shell command (like bt/btaudio), and the novad1 package
// drives it through fw_shell_run and parses the text — exactly the shape the BLE
// screens use for `bt`.
//
// WHAT IT DOES
//   subghz status            is a CC1101 there (PARTNUM 0x30 / VERSION 0x31), and
//                            the current frequency
//   subghz freq <mhz>        set the carrier in one of the CC1101's three bands
//                            (300-348 / 387-464 / 779-928 MHz)
//   subghz rx [secs]         listen for one OOK/ASK burst and print its timing —
//                            "capture a garage remote". Bounded by secs.
//   subghz tx <timings|hex>  replay a captured fixed-code OOK burst
//
// FIXED-CODE OOK ONLY. Rolling codes (KeeLoq and the like) are deliberately NOT
// implemented — replaying one does nothing useful and capturing one is a
// different job. v1 refused them too (novacc.py only ever did RAW timing); this
// keeps that line.
//
// .sub FILE FORMAT IS NOT HERE. A separate agent owns Flipper .sub interop. A
// capture is kept in the simplest internal form — a comma-separated list of
// microsecond pulse durations, mark first — and `subghz tx` replays that same
// string. `.sub` import/export slots in at cc_parse_timings()/cc_format_timings()
// (parse a .sub into a timing list, format a timing list back out), which is the
// one seam the rest of this file is built around.
//
// GROUNDING. Every register and sequence below is from the TI CC1101 datasheet
// (SWRS061) with the register/section cited inline, cross-checked against the
// ELECHOUSE_CC1101 Arduino library and RadioLib's CC1101 driver, and against the
// v1 MicroPython driver (RPCortex-repo .../novad1/novacc.py). Where this diverges
// from v1 it is because v1 was wrong: v1 is marked "DEVICE-PENDING" (desk-checked,
// never confirmed on hardware) and it wrote the OOK modulation byte to register
// 0x10, which is MDMCFG4, not MDMCFG2 (0x12) — see the note at the config block.
//
// DEVICE-UNCONFIRMED, ALL OF IT. No CC1101 has been on the bus. A wrong register
// write on this part gives NO error — the SPI just succeeds and nothing radiates
// or nothing is heard. The pure math and string handling below are host-tested
// (radio_test); the register sequences are not. First things to check on real
// hardware are listed at cmd_subghz.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>      // snprintf, used by the pure formatters below

// ===========================================================================
// Pure logic — no SDK, no hardware. Compiled on the host by radio_test, which
// defines RADIO_HOST_TEST and includes this file to reach exactly this section.
// A wrong frequency register or a mis-parsed timing list is a silent failure, so
// the parts that CAN be proven on the host live here and are proven there.
// ===========================================================================

// The CC1101 crystal. f_carrier = (F_XOSC / 2^16) * FREQ  (datasheet 21 "Frequency
// Programming", eq. for FREQ2/1/0 0x0D-0x0F). Nova D1 uses a 26 MHz part, the
// common module value; a 27 MHz module would shift every frequency ~4% and is the
// first thing to suspect if status reads a chip but rx/tx land off-channel.
#define CC_XOSC_HZ 26000000u

// carrier Hz -> the three FREQ bytes. 64-bit intermediate: f*2^16 overflows 32
// bits above ~65 kHz. FREQ is 22 bits, so 779-928 MHz all fit in three bytes.
static void cc_freq_to_regs(uint32_t hz, uint8_t regs[3]) {
    uint32_t freq = (uint32_t)(((uint64_t)hz << 16) / CC_XOSC_HZ);
    regs[0] = (uint8_t)((freq >> 16) & 0xFF);   // FREQ2 (0x0D)
    regs[1] = (uint8_t)((freq >> 8) & 0xFF);    // FREQ1 (0x0E)
    regs[2] = (uint8_t)(freq & 0xFF);           // FREQ0 (0x0F)
}

// The inverse of the above (host-tested; kept for a future read-back path in
// status). [[maybe_unused]] because the firmware currently reports the frequency
// from the registry, which is what was just programmed.
[[maybe_unused]] static uint32_t cc_regs_to_freq(const uint8_t regs[3]) {
    uint32_t freq = ((uint32_t)regs[0] << 16) | ((uint32_t)regs[1] << 8) | regs[2];
    return (uint32_t)(((uint64_t)freq * CC_XOSC_HZ) >> 16);
}

// The CC1101's three ISM sub-bands (datasheet 1 "Features", 4 "Absolute Maximum"
// / typical operating ranges). Outside these the PLL will not lock and nothing
// radiates, so a freq set is refused rather than left to fail in silence.
static bool cc_band_ok(uint32_t hz) {
    return (hz >= 300000000u && hz <= 348000000u) ||
           (hz >= 387000000u && hz <= 464000000u) ||
           (hz >= 779000000u && hz <= 928000000u);
}

// "433.92" (MHz, decimals optional) -> Hz. Own parser rather than strtod so the
// exact rounding is fixed and testable and no libm is pulled in. Up to six
// fractional digits are honoured (1 Hz resolution); the rest are ignored.
// Returns 0 on anything that is not a number, which the caller treats as an error.
static uint32_t cc_parse_mhz_to_hz(const char *s) {
    if (!s || !*s) return 0;
    uint32_t whole = 0;
    bool any = false;
    while (*s >= '0' && *s <= '9') { whole = whole * 10 + (uint32_t)(*s - '0'); s++; any = true; }
    uint32_t frac = 0, scale = 1;
    if (*s == '.') {
        s++;
        for (int i = 0; i < 6 && *s >= '0' && *s <= '9'; i++) {
            frac = frac * 10 + (uint32_t)(*s - '0');
            scale *= 10;
            s++;
            any = true;
        }
    }
    while (*s == ' ') s++;
    if (!any || *s) return 0;                   // trailing junk => not a clean number
    // Hz = whole*1e6 + frac/scale*1e6.
    return whole * 1000000u + frac * (1000000u / scale);
}

// Hz -> "433.92" with two decimals, for status and the capture summary. Two is
// enough to name a channel and never prints a spurious ".00000".
static void cc_format_mhz(char *out, unsigned cap, uint32_t hz) {
    unsigned mhz = hz / 1000000u;
    unsigned hundredths = (hz % 1000000u) / 10000u;
    snprintf(out, cap, "%u.%02u", mhz, hundredths);
}

// Parse a hex string ("8a2b", "8a 2b", "8a:2b") into bytes. Returns the count, or
// 0 on a bad digit, an odd number of digits, or overflow — the same contract as
// bt_ad_from_hex, widened for a payload rather than a 31-byte advertisement.
static size_t cc_parse_hex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    int hi = -1;
    for (const char *p = s; p && *p; p++) {
        char c = *p;
        if (c == ' ' || c == ':' || c == '-' || c == '_') continue;
        int v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return 0;
        if (hi < 0) hi = v;
        else { if (n >= cap) return 0; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    if (hi >= 0) return 0;
    return n;
}

// The largest single pulse a capture keeps, in microseconds. uint16 caps it at
// 65535 anyway; 60000 leaves headroom and any real OOK edge is far shorter.
#define CC_PULSE_MAX 60000u
// How many pulses a capture holds. A fixed-code remote burst (Princeton, CAME and
// the like are ~50-70 edges) is well under this; a rolling code or continuous
// noise would overrun it, which is one more reason this only claims fixed codes.
#define CC_CAP_MAX 128

// Clamp one measured pulse into the stored range. Its own function so the bound
// is defined in exactly one place and tested.
static uint16_t cc_clamp_pulse(uint32_t us) {
    if (us > CC_PULSE_MAX) us = CC_PULSE_MAX;
    return (uint16_t)us;
}

// Timing list -> "350,700,350,...". This is the on-the-wire capture format: what
// `rx` prints and what `tx` reads back, so the two MUST agree and a round-trip
// test pins that. Returns characters written (excluding NUL).
static unsigned cc_format_timings(char *out, unsigned cap, const uint16_t *t, int n) {
    unsigned at = 0;
    if (cap) out[0] = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + at, cap > at ? cap - at : 0, "%s%u", i ? "," : "", (unsigned)t[i]);
        if (w < 0 || at + (unsigned)w >= cap) { out[at] = 0; break; }
        at += (unsigned)w;
    }
    return at;
}

// "350,700,..." (or space-separated) -> timing list. Accepts commas, spaces and
// newlines as separators so a .sub RAW_Data line drops straight in later. Returns
// the count; stops at cap.
static int cc_parse_timings(const char *s, uint16_t *out, int cap) {
    int n = 0;
    const char *p = s;
    while (p && *p && n < cap) {
        while (*p == ',' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
        if (!*p) break;
        uint32_t v = 0;
        bool any = false;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); p++; any = true; }
        if (!any) { if (*p) p++; continue; }    // skip a stray non-digit
        out[n++] = cc_clamp_pulse(v);
    }
    return n;
}

// Is this argument the hex form or the timing form? The rule is unambiguous and
// documented in the help: a comma anywhere means a decimal timing list; anything
// else is treated as hex (16-bit big-endian microsecond pulses, the compact form
// `rx` can also emit). Space-separated decimals would collide with hex once the
// spaces are stripped, so the canonical timing list is comma-separated and that
// is what `rx` prints.
static bool cc_arg_is_hex(const char *s) {
    for (const char *p = s; p && *p; p++) if (*p == ',') return false;
    return true;
}

// Hex "016e02bc..." -> timing list (each pair of bytes is one big-endian uint16
// pulse). The symmetric partner to a future cc_timings_to_hex; the compact form a
// capture can be stored or shipped as.
static int cc_hex_to_timings(const char *s, uint16_t *out, int cap) {
    uint8_t bytes[CC_CAP_MAX * 2];
    size_t nb = cc_parse_hex(s, bytes, sizeof(bytes));
    if (!nb || (nb & 1)) return 0;              // need whole 16-bit values
    int n = 0;
    for (size_t i = 0; i + 1 < nb && n < cap; i += 2)
        out[n++] = (uint16_t)(((uint16_t)bytes[i] << 8) | bytes[i + 1]);
    return n;
}

#ifndef RADIO_HOST_TEST

// ===========================================================================
// Firmware. SPI over the Pico SDK directly (this is firmware, so it does NOT go
// through the fw_spi_* ABI — that is for packages), the same underlying calls
// os/api.cpp's fw_spi_* wrap: spi_init / spi_write_blocking /
// spi_write_read_blocking, with CS driven by hand on a GPIO.
// ===========================================================================

#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "interrupt.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// --- pins -------------------------------------------------------------------
//
// This is firmware and novaboard (the package's pin map) is not linkable from
// here, so the wiring is resolved the one way firmware can: read the registry
// override the package writes (Apps.NovaD1_PIN_*, device-wide "Apps." keys, so no
// per-user scope), and fall back to the reference Pico 2 W profile default when
// the user has set none.
//
// THESE DEFAULTS MUST TRACK novaboard.cpp's kPico2W profile. They are copied, not
// shared, because the two live in different link units; if a pin moves there it
// moves here. Current Pico 2 W map: SCK 18, MOSI 19, MISO 16 (SPI0), CC_CS 17,
// CC_GDO0 20.
// The registry override the package writes, or the default when the user set
// none. reg_get_int is the firmware registry call (the fw_reg_* variants are the
// package ABI); a -2 sentinel distinguishes "absent" from a real value.
static int cc_pin(const char *key, int dflt) {
    int32_t v = reg_get_int(key, -2);
    return v == -2 ? dflt : (int)v;
}

struct CcPins { unsigned sck, mosi, miso, cs, gdo0; };

static CcPins cc_pins(void) {
    CcPins p;
    p.sck  = (unsigned)cc_pin("Apps.NovaD1_PIN_spi_sck", 18);
    p.mosi = (unsigned)cc_pin("Apps.NovaD1_PIN_spi_mosi", 19);
    p.miso = (unsigned)cc_pin("Apps.NovaD1_PIN_spi_miso", 16);
    p.cs   = (unsigned)cc_pin("Apps.NovaD1_PIN_cc_cs", 17);
    p.gdo0 = (unsigned)cc_pin("Apps.NovaD1_PIN_cc_gdo0", 20);
    return p;
}

// SPI0 or SPI1 for a given SCK pin, the same rule novaboard checks against: the
// controller is (gpio / 8) % 2. SCK 18 -> SPI0, which is where the reference
// board wires all three SPI devices.
static spi_inst_t *cc_spi_for(unsigned sck) {
    return ((sck / 8) % 2) ? spi1 : spi0;
}

// --- the bus ----------------------------------------------------------------
//
// 1 MHz, SPI mode 0 (CPOL 0 / CPHA 0), MSB first — the CC1101's SPI (datasheet 10
// "Configuration Registers" / 11 "SPI Interface"), and v1's baud. Mode is set
// explicitly rather than left to spi_init's default, because a wrong SPI mode on
// a driver like this fails silently.
//
// Both other selects on the shared SPI0 (SX1276, SD) are driven high BEFORE the
// bus is initialised: a floating CS on another chip on the same bus will answer
// alongside the CC1101 and corrupt every read.
static spi_inst_t *cc_bus;
static unsigned    cc_cs_gpio;

static void cc_cs(int level) { gpio_put(cc_cs_gpio, level); }

static void cc_bus_begin(const CcPins &p) {
    cc_bus = cc_spi_for(p.sck);
    cc_cs_gpio = p.cs;

    // Every select on this bus high first (CC1101, SX1276, SD).
    const int others[] = { (int)p.cs,
                           cc_pin("Apps.NovaD1_PIN_sx_cs", 21),
                           cc_pin("Apps.NovaD1_PIN_sd_cs", 9) };
    for (int g : others) {
        if (g < 0) continue;
        gpio_init((unsigned)g);
        gpio_set_dir((unsigned)g, GPIO_OUT);
        gpio_put((unsigned)g, 1);
    }

    spi_init(cc_bus, 1000000);
    spi_set_format(cc_bus, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(p.sck,  GPIO_FUNC_SPI);
    gpio_set_function(p.mosi, GPIO_FUNC_SPI);
    gpio_set_function(p.miso, GPIO_FUNC_SPI);
}

// Header byte flags (datasheet 10.1 "SPI Interface"): bit7 read, bit6 burst.
#define CC_READ  0x80
#define CC_BURST 0x40

// Config registers (datasheet Table 43 "Configuration Register Overview").
#define CC_IOCFG0   0x02
#define CC_PKTCTRL0 0x08
#define CC_FSCTRL1  0x0B
#define CC_FREQ2    0x0D    // FREQ2/FREQ1/FREQ0 are 0x0D/0x0E/0x0F
#define CC_MDMCFG4  0x10
#define CC_MDMCFG3  0x11
#define CC_MDMCFG2  0x12    // NOT 0x10 — v1's novacc.py had this wrong (0x10 is MDMCFG4)
#define CC_MCSM0    0x18
#define CC_FOCCFG   0x19
#define CC_AGCCTRL2 0x1B
#define CC_AGCCTRL1 0x1C
#define CC_AGCCTRL0 0x1D
#define CC_FREND1   0x21
#define CC_FREND0   0x22
#define CC_PATABLE  0x3E
// Status registers, read burst (datasheet Table 44 "Status Register Overview").
#define CC_PARTNUM  0x30
#define CC_VERSION  0x31
// Command strobes (datasheet Table 42 "Command Strobes").
#define CC_SRES  0x30
#define CC_SRX   0x34
#define CC_STX   0x35
#define CC_SIDLE 0x36
#define CC_SFRX  0x3A
#define CC_SFTX  0x3B

static void cc_strobe(uint8_t s) {
    cc_cs(0);
    spi_write_blocking(cc_bus, &s, 1);
    cc_cs(1);
}

static void cc_write_reg(uint8_t addr, uint8_t val) {
    uint8_t b[2] = { addr, val };
    cc_cs(0);
    spi_write_blocking(cc_bus, b, 2);
    cc_cs(1);
}

// Status/config read. Status registers (0x30-0x3D) MUST use the burst bit or the
// address is decoded as a command strobe instead (datasheet 10.4 "Status
// Register Details") — reading VERSION as 0x31 would fire the SFSTXON strobe.
static uint8_t cc_read_status(uint8_t addr) {
    uint8_t tx[2] = { (uint8_t)(addr | CC_READ | CC_BURST), 0 };
    uint8_t rx[2] = { 0, 0 };
    cc_cs(0);
    spi_write_read_blocking(cc_bus, tx, rx, 2);
    cc_cs(1);
    return rx[1];
}

static void cc_reset(void) {
    // Manual reset strobe (datasheet 19.1 "Manual Reset"): SRES, then wait for the
    // part to settle. v1 used 2 ms; 5 is safe.
    cc_strobe(CC_SRES);
    sleep_ms(5);
}

// --- frequency, persisted ---------------------------------------------------
//
// `subghz freq` then `subghz rx` implies the frequency sticks, and `status` must
// report the truth after a reboot — so it lives in the registry, not a static.
// Same "Apps.NovaD1_" namespace v1 used (Apps.NovaD1_LoRa_Freq), stored as Hz.
#define CC_KEY_FREQ "Apps.NovaD1_SubGhz_Freq"
#define CC_DEFAULT_HZ 433920000u        // 433.92 MHz, the usual remote band

static uint32_t cc_current_hz(void) {
    int32_t v = reg_get_int(CC_KEY_FREQ, (int32_t)CC_DEFAULT_HZ);
    return (v > 0) ? (uint32_t)v : CC_DEFAULT_HZ;
}

// The firmware registry has reg_get_int but only a string reg_set, so an integer
// setting is written as text (reg_get_int parses it back).
static void cc_reg_set_int(const char *key, int32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%ld", (long)v);
    reg_set(key, b);
}

static void cc_program_freq(uint32_t hz) {
    uint8_t f[3];
    cc_freq_to_regs(hz, f);
    cc_write_reg(CC_FREQ2, f[0]);
    cc_write_reg(CC_FREQ2 + 1, f[1]);
    cc_write_reg(CC_FREQ2 + 2, f[2]);
}

// --- presence ---------------------------------------------------------------
//
// A CC1101 answers PARTNUM 0x00, VERSION 0x14 (datasheet 10.4; the common value
// is 0x14, some lots read 0x04/0x07). 0x00 or 0xFF on VERSION means nothing is on
// the bus — the same test v1's present() made, widened to also report PARTNUM.
static bool cc_present(uint8_t *partnum, uint8_t *version) {
    uint8_t pn = cc_read_status(CC_PARTNUM);
    uint8_t ver = cc_read_status(CC_VERSION);
    if (partnum) *partnum = pn;
    if (version) *version = ver;
    return !(ver == 0x00 || ver == 0xFF);
}

// --- OOK presets ------------------------------------------------------------
//
// Written as whole cited blocks, not a couple of registers over the power-on
// defaults, because in OOK the demodulator and PA both need setting up and a
// half-configured radio hears/says nothing with no error. Grounded in datasheet
// 27 "Configuration Registers" and TI DN022 "CC11xx OOK/ASK Register Settings",
// cross-checked with ELECHOUSE_CC1101 and RadioLib.
//
// DEVICE-UNCONFIRMED. These values have never keyed a real PA or fed a real
// demodulator. The AGC block especially is the tuning knob for OOK RX range and
// is the first thing to revisit if capture is deaf.
static void cc_config_common(uint32_t hz) {
    cc_reset();
    cc_program_freq(hz);
    // MDMCFG2 (0x12): MOD_FORMAT=011 ASK/OOK, SYNC_MODE=000 (no preamble/sync) —
    // datasheet 27.12. THIS is the byte v1 wrote to 0x10 by mistake.
    cc_write_reg(CC_MDMCFG2, 0x30);
    // MCSM0 (0x18): FS_AUTOCAL=01, calibrate on every IDLE->RX/TX (datasheet
    // 27.20 / 19.2 "Frequency Synthesizer Calibration"). Without this a freq
    // change never recalibrates the PLL and the radio sits off-channel, silently.
    cc_write_reg(CC_MCSM0, 0x18);
    // FSCTRL1 (0x0B): IF frequency ~152 kHz, TI's OOK reference value.
    cc_write_reg(CC_FSCTRL1, 0x06);
}

// TX: async serial mode, carrier keyed by the GDO0 pin (datasheet 27.1/10.5
// "Asynchronous Serial Operation"). PKTCTRL0=0x32 (PKT_FORMAT=11 async serial,
// LENGTH_CONFIG=10 infinite). IOCFG0=0x2D routes GDO0 as the serial data input
// (the ELECHOUSE/v1 value). FREND0=0x11 selects PATABLE[1] for a '1' and
// PATABLE[0] for a '0'; PATABLE {0x00 off, 0xC0 on} per DN022.
//
// PATABLE 0xC0 is a near-max setting characterised for 868/915 MHz. 315/433 MHz
// want a different PA ramp for rated output — output power at those bands is
// UNVERIFIED, though the keying still works.
static void cc_config_tx(uint32_t hz) {
    cc_config_common(hz);
    cc_write_reg(CC_PKTCTRL0, 0x32);
    cc_write_reg(CC_IOCFG0, 0x2D);
    cc_write_reg(CC_FREND0, 0x11);
    uint8_t pa[3] = { (uint8_t)(CC_PATABLE | CC_BURST), 0x00, 0xC0 };
    cc_cs(0);
    spi_write_blocking(cc_bus, pa, 3);
    cc_cs(1);
}

// RX: async serial mode, demodulated data out on GDO0 (IOCFG0=0x0D). The demod
// front-end is the least certain part of this driver.
//   MDMCFG4 0x87  CHANBW_E=2,CHANBW_M=0 -> ~203 kHz RX filter; DRATE_E=7
//   MDMCFG3 0x83  DRATE_M=131 -> ~4.8 kBaud nominal (async thresholds on
//                 amplitude, so this only sets the demod's expectation)
//   AGCCTRL2/1/0 0x07/0x00/0x91  fixed-gain-ish OOK AGC per DN022
//   FREND1 0x56   reset default, stated for completeness
static void cc_config_rx(uint32_t hz) {
    cc_config_common(hz);
    cc_write_reg(CC_PKTCTRL0, 0x32);
    cc_write_reg(CC_IOCFG0, 0x0D);
    cc_write_reg(CC_MDMCFG4, 0x87);
    cc_write_reg(CC_MDMCFG3, 0x83);
    cc_write_reg(CC_FOCCFG, 0x16);
    cc_write_reg(CC_AGCCTRL2, 0x07);
    cc_write_reg(CC_AGCCTRL1, 0x00);
    cc_write_reg(CC_AGCCTRL0, 0x91);
    cc_write_reg(CC_FREND1, 0x56);
}

// --- capture ----------------------------------------------------------------
//
// One burst, as a list of pulse durations (us), mark first. The demodulated OOK
// line idles low and a mark is the carrier present, so the first low->high edge
// starts the capture and the run ends after a long silence.
//
// This blocks its calling task for up to `secs`, tightly polling GDO0 — an edge
// timer cannot yield without missing edges. It is bounded, it pets the watchdog,
// and it honours Ctrl+C, the same shape as an IR capture. The package drives it
// on a worker so the UI never sits on this.
//
// DEVICE-UNCONFIRMED: idle polarity, the end-of-burst gap and the RX preset are
// all assumptions. First check: `subghz rx` with a known remote should return a
// pulse count in the low hundreds with the shortest pulses ~250-500 us.
#define CC_IDLE_END_US 15000u           // this much silence ends a burst
static int cc_capture(unsigned gdo0, unsigned secs, uint16_t *out) {
    gpio_init(gdo0);
    gpio_set_dir(gdo0, GPIO_IN);

    const uint32_t start = time_us_32();
    const uint32_t deadline = start + secs * 1000000u;
    int last = gpio_get(gdo0);
    uint32_t last_edge = start;
    int n = 0;
    bool capturing = false;
    uint32_t polls = 0;

    for (;;) {
        uint32_t now = time_us_32();
        if ((int32_t)(now - deadline) >= 0) break;
        if ((++polls & 0x3FFFF) == 0) { task_alive(); if (intr_check()) break; }

        int cur = gpio_get(gdo0);
        if (cur != last) {
            uint32_t dt = now - last_edge;
            last_edge = now;
            last = cur;
            if (!capturing) {
                // First edge: begin. A low->high starts on a mark; if the line
                // happened to idle high, the first stored pulse is a space and
                // replay is inverted — noted, and why idle polarity is check one.
                capturing = (cur != 0);
                n = 0;
            } else {
                if (n < CC_CAP_MAX) out[n++] = cc_clamp_pulse(dt);
                if (n >= CC_CAP_MAX) break;
            }
        } else if (capturing && (now - last_edge) > CC_IDLE_END_US) {
            break;                       // burst over
        }
    }
    return n;
}

// --- replay -----------------------------------------------------------------
//
// Key GDO0 from the timing list: even index = mark (carrier), odd = space, each
// held for its microsecond duration (datasheet async serial TX; v1's fire_timing
// in SDK form). busy_wait_us for the intervals rather than a yielding sleep.
//
// This does NOT make the waveform jitter-free: on a preemptive two-core scheduler
// the loop can still be preempted between two edges, stretching a pulse. It is
// best-effort and good enough for a ±20%-tolerant fixed-code remote most of the
// time; see the jitter-vs-bug note in cmd_subghz. Bounded by the list length and
// clamped pulses, so total air time is bounded regardless.
static void cc_replay(unsigned gdo0, const uint16_t *t, int n) {
    gpio_init(gdo0);
    gpio_set_dir(gdo0, GPIO_OUT);
    gpio_put(gdo0, 0);
    cc_strobe(CC_STX);
    sleep_ms(1);
    for (int i = 0; i < n; i++) {
        gpio_put(gdo0, (i % 2 == 0) ? 1 : 0);
        busy_wait_us(t[i]);
    }
    gpio_put(gdo0, 0);
    cc_strobe(CC_SIDLE);
}

// The last capture, file-static so it never sits on a shell stack and so it
// SURVIVES between shell calls: `subghz rx` fills it, then `subghz tx last`
// replays it without the caller having to carry a long timing list back through
// the command line (a screen slot cannot hold one, and the shell line has its own
// length limit). The firmware image is always resident, so the static persists;
// the single novad1 worker serialises access, so one rx cannot clobber another's.
static uint16_t cc_timings[CC_CAP_MAX];
static int      cc_last_n;

// --- the command ------------------------------------------------------------

static int sub_status(void) {
    CcPins p = cc_pins();
    cc_bus_begin(p);
    cc_reset();
    uint32_t hz = cc_current_hz();
    cc_program_freq(hz);

    uint8_t pn = 0, ver = 0;
    bool present = cc_present(&pn, &ver);
    char mhz[16];
    cc_format_mhz(mhz, sizeof(mhz), hz);

    out_info("Sub-GHz  CC1101");
    if (present) out_multi("  Chip    present  (part 0x%02X ver 0x%02X)", pn, ver);
    else         out_multi("  Chip    absent   (part 0x%02X ver 0x%02X)", pn, ver);
    out_multi("  Freq    %s MHz", mhz);
    out_multi("  Mode    OOK/ASK fixed-code");
    // A successful query with a negative answer, not a failure — same as `bt
    // status`. Presence is in the text; returning non-zero here would make a
    // caller unable to tell "no chip" from "the command did not run".
    return 0;
}

static int sub_freq(const char *arg) {
    uint32_t hz = cc_parse_mhz_to_hz(arg);
    if (!hz) { out_err("Usage: subghz freq <mhz>   e.g. subghz freq 433.92"); return 1; }
    if (!cc_band_ok(hz)) {
        out_err("%s MHz is outside the CC1101 bands (300-348, 387-464, 779-928).", arg);
        return 1;
    }
    cc_reg_set_int(CC_KEY_FREQ, (int32_t)hz);
    CcPins p = cc_pins();
    cc_bus_begin(p);
    cc_config_common(hz);              // reset + program + autocal-on-next-strobe
    char mhz[16];
    cc_format_mhz(mhz, sizeof(mhz), hz);
    out_ok("Frequency set to %s MHz", mhz);
    return 0;
}

static int sub_rx(unsigned secs) {
    if (secs < 1) secs = 5;
    if (secs > 10) secs = 10;          // a tight poll owns the core; keep it short
    CcPins p = cc_pins();
    cc_bus_begin(p);
    uint32_t hz = cc_current_hz();
    cc_config_rx(hz);
    cc_strobe(CC_SFRX);
    cc_strobe(CC_SRX);

    out_info("Listening %us for a burst... (Ctrl+C to stop)", secs);
    int n = cc_capture(p.gdo0, secs, cc_timings);
    cc_strobe(CC_SIDLE);

    if (n < 4) { cc_last_n = 0; out_info("Nothing captured."); return 0; }
    cc_last_n = n;                      // kept for `subghz tx last`

    char mhz[16];
    cc_format_mhz(mhz, sizeof(mhz), hz);
    out_ok("Captured %d pulses at %s MHz", n, mhz);
    // The replayable line: comma-separated microseconds, exactly what `tx` reads.
    // Bounded by CC_CAP_MAX, so the buffer is sized to it rather than guessed at.
    char line[CC_CAP_MAX * 7 + 1];
    cc_format_timings(line, sizeof(line), cc_timings, n);
    out_multi("%s", line);
    return 0;
}

static int sub_tx(const char *arg) {
    int n;
    if (!strcmp(arg, "last")) {
        // Replay the burst still held from the last `subghz rx`. This is the path
        // the Nova D1 screen uses, so it never has to carry the timing list back.
        n = cc_last_n;
        if (n < 2) { out_err("No capture to replay. Run 'subghz rx' first."); return 1; }
    } else {
        if (cc_arg_is_hex(arg)) n = cc_hex_to_timings(arg, cc_timings, CC_CAP_MAX);
        else                    n = cc_parse_timings(arg, cc_timings, CC_CAP_MAX);
        if (n < 2) { out_err("Nothing to send. Give a timing list (350,700,...) or hex."); return 1; }
        cc_last_n = n;
    }

    CcPins p = cc_pins();
    cc_bus_begin(p);
    uint32_t hz = cc_current_hz();
    if (!cc_present(nullptr, nullptr)) {
        out_err("No CC1101 found — nothing to transmit with.");
        return 1;
    }
    cc_config_tx(hz);
    cc_replay(p.gdo0, cc_timings, n);

    char mhz[16];
    cc_format_mhz(mhz, sizeof(mhz), hz);
    out_ok("Sent %d pulses at %s MHz", n, mhz);
    return 0;
}

// argv[2..argc) joined, so `subghz tx 350 700 350` (space-separated) is not
// silently read as the single pulse in argv[2]. Everything past the subcommand is
// one argument to the parser.
static void join_tail(int argc, char **argv, int from, char *out, unsigned cap) {
    unsigned at = 0;
    if (cap) out[0] = 0;
    for (int i = from; i < argc; i++) {
        int w = snprintf(out + at, cap > at ? cap - at : 0, "%s%s", at ? " " : "", argv[i]);
        if (w < 0 || at + (unsigned)w >= cap) break;
        at += (unsigned)w;
    }
}

static int cmd_subghz(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    // FIRST THINGS TO CHECK ON HARDWARE (a wrong register here is silent):
    //   1. `subghz status` must read part 0x00 ver 0x14 — if 0x00/0xFF the SPI or
    //      wiring is wrong (CS 17, SCK 18, MOSI 19, MISO 16 on the Pico 2 W).
    //   2. `subghz tx` a known code and scope GDO0 (GPIO 20) / a receiver: a
    //      carrier must key at the programmed frequency. If dead, suspect the
    //      IOCFG0/PKTCTRL0/PATABLE TX block or the 26 vs 27 MHz crystal. Confirm
    //      GDO0 is not driven from BOTH ends: IOCFG0=0x2D is the cited ELECHOUSE/v1
    //      value and unverified — if it is actually an output function the chip and
    //      the RP2350 fight over the pin.
    //   3. `subghz rx` a known remote must return ~hundreds of pulses; if deaf,
    //      the AGC/MDMCFG RX block is the tuning target.
    //   4. TELLING JITTER FROM A BUG: this is a preemptive two-core scheduler and
    //      the bit-bang can be preempted mid-burst, so timing is best-effort. A
    //      register error fails IDENTICALLY every time; scheduling jitter does not.
    //      If replay of the SAME capture is inconsistent between attempts, that is
    //      jitter, not the TX config. A lone multi-millisecond pulse in an
    //      otherwise clean capture is a preemption, not the demodulator.

    if (!strcmp(sub, "status")) return sub_status();
    if (!strcmp(sub, "freq"))   return sub_freq(argc > 2 ? argv[2] : "");
    if (!strcmp(sub, "rx"))     return sub_rx(argc > 2 ? (unsigned)atoi(argv[2]) : 5);
    if (!strcmp(sub, "tx")) {
        if (argc < 3) { out_err("Usage: subghz tx last | <timings> | <hex>"); return 1; }
        char tail[CC_CAP_MAX * 7 + 1];
        join_tail(argc, argv, 2, tail, sizeof(tail));
        return sub_tx(tail);
    }

    out_multi("Usage:");
    out_multi("  subghz status              is a CC1101 present, and the frequency");
    out_multi("  subghz freq <mhz>          300-348 / 387-464 / 779-928 MHz");
    out_multi("  subghz rx [secs]           capture one OOK burst (fixed codes)");
    out_multi("  subghz tx last             replay the last capture");
    out_multi("  subghz tx <timings|hex>    replay 350,700,... (comma-separated) or hex");
    out_multi("  Fixed-code OOK only; rolling codes are not replayable.");
    return argc > 1 ? 1 : 0;
}

void subghz_register(void) {
    static const Command c{"subghz", "CC1101 sub-GHz: status, freq, capture, replay",
                           cmd_subghz, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}

#endif  // RADIO_HOST_TEST
