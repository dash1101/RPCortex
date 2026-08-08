// SX1276 LoRa radio — the `lora` shell command.
//
// A firmware driver, not a package, for the same reason cc1101.cpp is: the Nova
// D1 GUI and a separate mesh agent both have to reach it, and a package cannot
// call another package. Self-registering shell command; the callers drive it
// through fw_shell_run and parse the text.
//
// THIS INTERFACE IS A CONTRACT. A separate mesh agent parses the exact lines
// below, so their wording and shape are fixed. The tags are what fw_shell_run
// captures after ANSI is stripped: out_ok -> "[@] ", out_info -> "[:] ".
//
//   lora status
//       [:] LoRa  SX1276
//         Chip   present  (ver 0x12)          (or: absent  (ver 0x00))
//         Freq   915.00 MHz
//         Modem  SF7 BW125 CR4/5
//   lora config <freq_mhz> <sf> <bw_khz> <cr>
//       [@] LoRa 915.00 MHz SF7 BW125 CR4/5     (sf 7-12, bw 125/250/500, cr 5-8)
//   lora send <hexbytes>
//       [@] Sent 5 bytes                        (or [!] ... on failure)
//   lora recv [secs]
//       [@] RX 48656c6c6f rssi -42 snr 9        (one packet), or
//       [:] Nothing received.                   (timeout)
//
// GROUNDING. Registers and sequences are from the Semtech SX1276/77/78/79
// datasheet (Rev 7, DS_SX1276-7-8-9_W_APP_V7), cited inline, cross-checked with
// RadioLib's SX127x driver and the v1 MicroPython driver (RPCortex-repo
// .../novad1/novalora.py). Where this adds to v1 it is because v1 was minimal and
// "DEVICE-PENDING": v1 never read SNR, never set the LF/HF frequency-mode bit, and
// hard-coded ModemConfig for SF7/BW125 only. This computes the modem config from
// arguments and sets LowDataRateOptimize where the datasheet requires it.
//
// DEVICE-UNCONFIRMED. No SX1276 has been on the bus, and real comms need TWO
// boards. A wrong register write is silent. Pure math and framing are host-tested
// (radio_test); the register sequences are not. First checks are at cmd_lora.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>      // snprintf, used by the pure formatters below

// ===========================================================================
// Pure logic — no SDK, no hardware. radio_test defines RADIO_HOST_TEST and
// includes this file to reach exactly this section.
// ===========================================================================

// F_STEP = F_XOSC / 2^19, F_XOSC = 32 MHz (datasheet 4.1.4 "LoRa Modem"):
// Frf = f_carrier / F_STEP = f_carrier * 2^19 / 32e6.
#define SX_XOSC_HZ 32000000u

static void sx_freq_to_frf(uint32_t hz, uint8_t frf[3]) {
    uint32_t v = (uint32_t)(((uint64_t)hz << 19) / SX_XOSC_HZ);
    frf[0] = (uint8_t)((v >> 16) & 0xFF);   // RegFrfMsb 0x06
    frf[1] = (uint8_t)((v >> 8) & 0xFF);    // RegFrfMid 0x07
    frf[2] = (uint8_t)(v & 0xFF);           // RegFrfLsb 0x08
}

// The inverse (host-tested; kept for a read-back path). [[maybe_unused]] as the
// firmware reports frequency from the stored config, not the chip.
[[maybe_unused]] static uint32_t sx_frf_to_freq(const uint8_t frf[3]) {
    uint32_t v = ((uint32_t)frf[0] << 16) | ((uint32_t)frf[1] << 8) | frf[2];
    return (uint32_t)(((uint64_t)v * SX_XOSC_HZ) >> 19);
}

// Below this the part is on its low-frequency port: RegOpMode LowFrequencyModeOn
// (bit 3) must be set and the RSSI offset is -164 rather than -157 (datasheet
// 5.5.5 "RSSI and SNR"). 525 MHz is the datasheet's LF/HF boundary.
#define SX_LF_HF_HZ 525000000u
static bool sx_is_lf(uint32_t hz) { return hz < SX_LF_HF_HZ; }

// "915" or "915.0" (MHz) -> Hz. Own parser, same reasoning as the CC1101 side.
static uint32_t sx_parse_mhz_to_hz(const char *s) {
    if (!s || !*s) return 0;
    uint32_t whole = 0; bool any = false;
    while (*s >= '0' && *s <= '9') { whole = whole * 10 + (uint32_t)(*s - '0'); s++; any = true; }
    uint32_t frac = 0, scale = 1;
    if (*s == '.') {
        s++;
        for (int i = 0; i < 6 && *s >= '0' && *s <= '9'; i++) {
            frac = frac * 10 + (uint32_t)(*s - '0'); scale *= 10; s++; any = true;
        }
    }
    while (*s == ' ') s++;
    if (!any || *s) return 0;
    return whole * 1000000u + frac * (1000000u / scale);
}

static void sx_format_mhz(char *out, unsigned cap, uint32_t hz) {
    snprintf(out, cap, "%u.%02u", hz / 1000000u, (hz % 1000000u) / 10000u);
}

// Compute RegModemConfig1/2/3 from human parameters. Returns false on any value
// the SX1276 cannot express, so `config` rejects it rather than programming
// nonsense (a wrong modem byte is a link that never forms, with no error).
//
//   ModemConfig1 (0x1D): Bw[7:4] CodingRate[3:1] ImplicitHeader[0]
//       Bw:  125k=0x7, 250k=0x8, 500k=0x9        (datasheet Table 88)
//       CR:  4/5=1 .. 4/8=4  -> value cr-4
//   ModemConfig2 (0x1E): SpreadingFactor[7:4] TxContinuous[3] RxPayloadCrcOn[2]
//   ModemConfig3 (0x26): LowDataRateOptimize[3] AgcAutoOn[2]
//
// LowDataRateOptimize is MANDATED when a symbol lasts longer than 16 ms
// (datasheet errata / 4.1.1.6). Symbol time = 2^SF / BW, so 2^SF > 16*BW_kHz.
// v1 hard-coded ModemConfig3=0x04 (LDRO off) and would have failed at SF11/12 on
// 125 kHz; this sets it correctly.
static bool sx_modem_cfg(uint8_t sf, uint16_t bw_khz, uint8_t cr, bool crc_on,
                         uint8_t *mc1, uint8_t *mc2, uint8_t *mc3) {
    if (sf < 7 || sf > 12) return false;
    if (cr < 5 || cr > 8) return false;
    uint8_t bwv;
    if      (bw_khz == 125) bwv = 0x7;
    else if (bw_khz == 250) bwv = 0x8;
    else if (bw_khz == 500) bwv = 0x9;
    else return false;
    uint8_t crv = (uint8_t)(cr - 4);        // 5->1 .. 8->4

    if (mc1) *mc1 = (uint8_t)((bwv << 4) | (crv << 1));           // explicit header
    if (mc2) *mc2 = (uint8_t)((sf << 4) | (crc_on ? 0x04 : 0));   // TxContinuous=0
    bool ldro = ((uint32_t)1 << sf) > (uint32_t)16 * bw_khz;
    if (mc3) *mc3 = (uint8_t)((ldro ? 0x08 : 0) | 0x04);          // AgcAutoOn
    return true;
}

// SNR: RegPktSnrValue (0x19) is signed, in 0.25 dB steps (datasheet 5.5.5), so
// dB = raw/4. v1 never read this.
static int sx_snr_db(uint8_t raw) { return (int)((int8_t)raw) / 4; }

// RSSI: RegPktRssiValue (0x1A) plus the port offset (datasheet 5.5.5). The full
// SNR-corrected form is a later refinement; the offset form is what v1 used and
// is within a dB or two for a decodable packet.
static int sx_rssi_dbm(uint8_t raw, bool lf) { return (lf ? -164 : -157) + (int)raw; }

// Bytes -> lowercase hex, for the RX line and for echoing what was sent.
static void sx_bytes_to_hex(const uint8_t *b, int n, char *out, unsigned cap) {
    static const char h[] = "0123456789abcdef";
    unsigned at = 0;
    for (int i = 0; i < n && at + 2 < cap; i++) {
        out[at++] = h[(b[i] >> 4) & 0xF];
        out[at++] = h[b[i] & 0xF];
    }
    if (at < cap) out[at] = 0;
}

// Hex -> bytes. Same contract as the CC1101 parser.
static int sx_parse_hex(const char *s, uint8_t *out, int cap) {
    int n = 0, hi = -1;
    for (const char *p = s; p && *p; p++) {
        char c = *p;
        if (c == ' ' || c == ':' || c == '-' || c == '_') continue;
        int v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        if (hi < 0) hi = v;
        else { if (n >= cap) return -1; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    if (hi >= 0) return -1;
    return n;
}

// The RX contract line, assembled in one place so the command and the test can
// never disagree: "RX <hex> rssi <n> snr <n>". The command prints it through
// out_ok, which prepends "[@] ".
static void sx_format_rx(char *out, unsigned cap, const uint8_t *data, int n,
                         int rssi, int snr) {
    char hex[520];
    sx_bytes_to_hex(data, n, hex, sizeof(hex));
    snprintf(out, cap, "RX %s rssi %d snr %d", hex, rssi, snr);
}

#ifndef RADIO_HOST_TEST

// ===========================================================================
// Firmware. Pico SDK SPI directly (not the fw_spi_* ABI), same underlying calls
// as os/api.cpp's fw_spi_*.
// ===========================================================================

#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "interrupt.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// --- pins (see cc1101.cpp for why firmware resolves them this way) ------------
// Pico 2 W profile defaults, which MUST track novaboard.cpp's kPico2W: SCK 18,
// MOSI 19, MISO 16 (SPI0), SX_CS 21, SX_RST 12.
static int sx_pin(const char *key, int dflt) {
    int32_t v = reg_get_int(key, -2);
    return v == -2 ? dflt : (int)v;
}

struct SxPins { unsigned sck, mosi, miso, cs; int rst; };

static SxPins sx_pins(void) {
    SxPins p;
    p.sck  = (unsigned)sx_pin("Apps.NovaD1_PIN_spi_sck", 18);
    p.mosi = (unsigned)sx_pin("Apps.NovaD1_PIN_spi_mosi", 19);
    p.miso = (unsigned)sx_pin("Apps.NovaD1_PIN_spi_miso", 16);
    p.cs   = (unsigned)sx_pin("Apps.NovaD1_PIN_sx_cs", 21);
    p.rst  = sx_pin("Apps.NovaD1_PIN_sx_rst", 12);
    return p;
}

static spi_inst_t *sx_spi_for(unsigned sck) { return ((sck / 8) % 2) ? spi1 : spi0; }

// --- the bus ----------------------------------------------------------------
//
// 2 MHz, SPI mode 0, MSB first — v1's baud and the SX1276's SPI (datasheet 4.3
// "Digital Interface"). Mode set explicitly; other selects on the shared SPI0
// (CC1101, SD) driven high before init.
static spi_inst_t *sx_bus;
static unsigned    sx_cs_gpio;

static void sx_cs(int level) { gpio_put(sx_cs_gpio, level); }

static void sx_bus_begin(const SxPins &p) {
    sx_bus = sx_spi_for(p.sck);
    sx_cs_gpio = p.cs;

    const int others[] = { (int)p.cs,
                           sx_pin("Apps.NovaD1_PIN_cc_cs", 17),
                           sx_pin("Apps.NovaD1_PIN_sd_cs", 9) };
    for (int g : others) {
        if (g < 0) continue;
        gpio_init((unsigned)g);
        gpio_set_dir((unsigned)g, GPIO_OUT);
        gpio_put((unsigned)g, 1);
    }

    spi_init(sx_bus, 2000000);
    spi_set_format(sx_bus, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(p.sck,  GPIO_FUNC_SPI);
    gpio_set_function(p.mosi, GPIO_FUNC_SPI);
    gpio_set_function(p.miso, GPIO_FUNC_SPI);

    if (p.rst >= 0) {
        // Manual reset (datasheet 7.2.2): NRST low >100 us, release, wait 5 ms.
        gpio_init((unsigned)p.rst);
        gpio_set_dir((unsigned)p.rst, GPIO_OUT);
        gpio_put((unsigned)p.rst, 0);
        sleep_ms(1);
        gpio_put((unsigned)p.rst, 1);
        sleep_ms(5);
    }
}

// Registers (datasheet 6.4 "LoRa Mode Register Map", Table 41).
#define SX_REG_FIFO        0x00
#define SX_REG_OPMODE      0x01
#define SX_REG_FRFMSB      0x06    // 0x06/0x07/0x08
#define SX_REG_PACONFIG    0x09
#define SX_REG_LNA         0x0C
#define SX_REG_FIFOADDRPTR 0x0D
#define SX_REG_FIFOTXBASE  0x0E
#define SX_REG_FIFORXBASE  0x0F
#define SX_REG_FIFORXCUR   0x10
#define SX_REG_IRQFLAGS    0x12
#define SX_REG_RXNBBYTES   0x13
#define SX_REG_PKTSNR      0x19
#define SX_REG_PKTRSSI     0x1A
#define SX_REG_MODEMCFG1   0x1D
#define SX_REG_MODEMCFG2   0x1E
#define SX_REG_PREAMBLEMSB 0x20    // 0x20/0x21
#define SX_REG_PAYLOADLEN  0x22
#define SX_REG_MODEMCFG3   0x26
#define SX_REG_VERSION     0x42
#define SX_REG_PADAC       0x4D

// OpMode bits (datasheet 6.4 / Table 42).
#define SX_LORA      0x80          // LongRangeMode; only settable in SLEEP
#define SX_LF        0x08          // LowFrequencyModeOn
#define SX_SLEEP     0x00
#define SX_STDBY     0x01
#define SX_TX        0x03
#define SX_RXCONT    0x05

// IRQ flags (datasheet Table 23 RegIrqFlags).
#define SX_IRQ_RXDONE  0x40
#define SX_IRQ_CRCERR  0x20
#define SX_IRQ_TXDONE  0x08

static void sx_write(uint8_t addr, uint8_t val) {
    uint8_t b[2] = { (uint8_t)(addr | 0x80), val };     // wnr bit set = write
    sx_cs(0);
    spi_write_blocking(sx_bus, b, 2);
    sx_cs(1);
}

static uint8_t sx_read(uint8_t addr) {
    uint8_t tx[2] = { (uint8_t)(addr & 0x7F), 0 };
    uint8_t rx[2] = { 0, 0 };
    sx_cs(0);
    spi_write_read_blocking(sx_bus, tx, rx, 2);
    sx_cs(1);
    return rx[1];
}

// --- config, persisted ------------------------------------------------------
//
// Stored in the "Apps.NovaD1_" namespace so status/send/recv all read the same
// truth across reboots. FreqHz is a distinct key from v1's MHz-string
// Apps.NovaD1_LoRa_Freq to avoid mis-reading an old value as Hz.
#define SX_KEY_FREQ "Apps.NovaD1_LoRa_FreqHz"
#define SX_KEY_SF   "Apps.NovaD1_LoRa_SF"
#define SX_KEY_BW   "Apps.NovaD1_LoRa_BW"
#define SX_KEY_CR   "Apps.NovaD1_LoRa_CR"
#define SX_DEF_HZ   915000000u
#define SX_DEF_SF   7
#define SX_DEF_BW   125
#define SX_DEF_CR   5

struct SxCfg { uint32_t hz; uint8_t sf; uint16_t bw; uint8_t cr; };

// The firmware registry has reg_get_int but only a string reg_set, so integer
// settings are written as text (reg_get_int parses them back).
static void sx_reg_set_int(const char *key, int32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "%ld", (long)v);
    reg_set(key, b);
}

static SxCfg sx_cfg(void) {
    SxCfg c;
    int32_t hz = reg_get_int(SX_KEY_FREQ, (int32_t)SX_DEF_HZ);
    c.hz = (hz > 0) ? (uint32_t)hz : SX_DEF_HZ;
    c.sf = (uint8_t)reg_get_int(SX_KEY_SF, SX_DEF_SF);
    c.bw = (uint16_t)reg_get_int(SX_KEY_BW, SX_DEF_BW);
    c.cr = (uint8_t)reg_get_int(SX_KEY_CR, SX_DEF_CR);
    return c;
}

static bool sx_present(uint8_t *ver) {
    uint8_t v = sx_read(SX_REG_VERSION);
    if (ver) *ver = v;
    return v == 0x12;                    // datasheet 6.4: RegVersion reads 0x12
}

// Put the modem into a known LoRa standby with the stored config applied. Returns
// false if the chip does not answer 0x12. Grounded in datasheet 4.1 and v1's
// begin(), with the LF/HF bit and computed modem config added.
static bool sx_begin(const SxCfg &cfg) {
    // LongRangeMode can only change in SLEEP (datasheet 6.4). Drop to FSK sleep
    // first, then LoRa sleep, before touching anything else.
    uint8_t lf = sx_is_lf(cfg.hz) ? SX_LF : 0;
    sx_write(SX_REG_OPMODE, 0x00);                 // FSK sleep
    sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_SLEEP);
    sleep_ms(10);

    if (!sx_present(nullptr)) return false;

    uint8_t frf[3];
    sx_freq_to_frf(cfg.hz, frf);
    sx_write(SX_REG_FRFMSB, frf[0]);
    sx_write(SX_REG_FRFMSB + 1, frf[1]);
    sx_write(SX_REG_FRFMSB + 2, frf[2]);

    sx_write(SX_REG_FIFOTXBASE, 0x00);
    sx_write(SX_REG_FIFORXBASE, 0x00);
    sx_write(SX_REG_LNA, (uint8_t)(sx_read(SX_REG_LNA) | 0x03));   // LNA boost on

    uint8_t mc1, mc2, mc3;
    if (!sx_modem_cfg(cfg.sf, cfg.bw, cfg.cr, /*crc*/true, &mc1, &mc2, &mc3)) return false;
    sx_write(SX_REG_MODEMCFG1, mc1);
    sx_write(SX_REG_MODEMCFG2, mc2);
    sx_write(SX_REG_MODEMCFG3, mc3);

    sx_write(SX_REG_PREAMBLEMSB, 0x00);
    sx_write(SX_REG_PREAMBLEMSB + 1, 0x08);        // preamble length 8

    // PA_BOOST, ~17 dBm (datasheet 5.4.2). PaDac 0x84 = normal (0x87 would be the
    // +20 dBm boost, which needs duty limits).
    sx_write(SX_REG_PACONFIG, 0x8F);
    sx_write(SX_REG_PADAC, 0x84);

    sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_STDBY);
    sleep_ms(5);
    return true;
}

// --- the command ------------------------------------------------------------

static int lora_status(void) {
    SxPins p = sx_pins();
    sx_bus_begin(p);
    SxCfg cfg = sx_cfg();

    uint8_t lf = sx_is_lf(cfg.hz) ? SX_LF : 0;
    sx_write(SX_REG_OPMODE, 0x00);
    sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_SLEEP);
    sleep_ms(5);

    uint8_t ver = 0;
    bool present = sx_present(&ver);
    char mhz[16];
    sx_format_mhz(mhz, sizeof(mhz), cfg.hz);

    out_info("LoRa  SX1276");
    if (present) out_multi("  Chip   present  (ver 0x%02X)", ver);
    else         out_multi("  Chip   absent   (ver 0x%02X)", ver);
    out_multi("  Freq   %s MHz", mhz);
    out_multi("  Modem  SF%u BW%u CR4/%u", cfg.sf, cfg.bw, cfg.cr);
    // A successful query with a negative answer, not a failure — same as `bt
    // status`, and it lets the mesh agent tell "no chip" from "did not run".
    return 0;
}

static int lora_config(int argc, char **argv) {
    if (argc < 6) {
        out_err("Usage: lora config <freq_mhz> <sf 7-12> <bw_khz 125|250|500> <cr 5-8>");
        return 1;
    }
    uint32_t hz = sx_parse_mhz_to_hz(argv[2]);
    int sf = atoi(argv[3]);
    int bw = atoi(argv[4]);
    int cr = atoi(argv[5]);
    if (!hz) { out_err("Bad frequency: %s", argv[2]); return 1; }

    uint8_t mc1, mc2, mc3;
    if (!sx_modem_cfg((uint8_t)sf, (uint16_t)bw, (uint8_t)cr, true, &mc1, &mc2, &mc3)) {
        out_err("Bad modem: sf 7-12, bw 125/250/500, cr 5-8.");
        return 1;
    }

    sx_reg_set_int(SX_KEY_FREQ, (int32_t)hz);
    sx_reg_set_int(SX_KEY_SF, sf);
    sx_reg_set_int(SX_KEY_BW, bw);
    sx_reg_set_int(SX_KEY_CR, cr);

    SxPins p = sx_pins();
    sx_bus_begin(p);
    SxCfg cfg = { hz, (uint8_t)sf, (uint16_t)bw, (uint8_t)cr };
    if (!sx_begin(cfg)) { out_err("No SX1276 found (RegVersion not 0x12)."); return 1; }

    char mhz[16];
    sx_format_mhz(mhz, sizeof(mhz), hz);
    out_ok("LoRa %s MHz SF%d BW%d CR4/%d", mhz, sf, bw, cr);
    return 0;
}

static int lora_send(const char *hex) {
    uint8_t data[256];
    int n = sx_parse_hex(hex, data, sizeof(data));
    if (n <= 0) { out_err("Give payload as hex, e.g. lora send 48656c6c6f"); return 1; }

    SxPins p = sx_pins();
    sx_bus_begin(p);
    SxCfg cfg = sx_cfg();
    if (!sx_begin(cfg)) { out_err("No SX1276 found (RegVersion not 0x12)."); return 1; }

    uint8_t lf = sx_is_lf(cfg.hz) ? SX_LF : 0;
    sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_STDBY);
    sx_write(SX_REG_IRQFLAGS, 0xFF);              // clear IRQs
    sx_write(SX_REG_FIFOADDRPTR, 0x00);
    // Burst write into the FIFO (datasheet 4.1.2.4 "FIFO Data Buffer").
    uint8_t hdr = SX_REG_FIFO | 0x80;
    sx_cs(0);
    spi_write_blocking(sx_bus, &hdr, 1);
    spi_write_blocking(sx_bus, data, (size_t)n);
    sx_cs(1);
    sx_write(SX_REG_PAYLOADLEN, (uint8_t)n);
    sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_TX);

    // Poll TxDone, bounded. A LoRa frame at the slowest setting is under ~2 s.
    uint32_t deadline = task_now_ms() + 3000;
    bool done = false;
    while ((int32_t)(task_now_ms() - deadline) < 0) {
        if (sx_read(SX_REG_IRQFLAGS) & SX_IRQ_TXDONE) { done = true; break; }
        if (intr_check()) break;
        task_sleep_ms(5);
    }
    sx_write(SX_REG_IRQFLAGS, 0xFF);
    if (!done) { out_err("TX timed out."); return 1; }
    out_ok("Sent %d bytes", n);
    return 0;
}

static int lora_recv(unsigned secs) {
    if (secs < 1) secs = 5;
    if (secs > 60) secs = 60;
    SxPins p = sx_pins();
    sx_bus_begin(p);
    SxCfg cfg = sx_cfg();
    if (!sx_begin(cfg)) { out_err("No SX1276 found (RegVersion not 0x12)."); return 1; }

    uint8_t lf = sx_is_lf(cfg.hz) ? SX_LF : 0;
    sx_write(SX_REG_IRQFLAGS, 0xFF);
    sx_write(SX_REG_FIFOADDRPTR, 0x00);
    sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_RXCONT);

    out_info("Listening %us...", secs);
    uint32_t deadline = task_now_ms() + secs * 1000;
    uint8_t flags = 0;
    bool got = false;
    while ((int32_t)(task_now_ms() - deadline) < 0) {
        flags = sx_read(SX_REG_IRQFLAGS);
        if (flags & SX_IRQ_RXDONE) { got = true; break; }
        if (intr_check()) break;
        task_sleep_ms(5);
    }

    if (!got) {
        sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_STDBY);
        out_info("Nothing received.");
        return 0;
    }

    sx_write(SX_REG_IRQFLAGS, 0xFF);
    if (flags & SX_IRQ_CRCERR) {
        sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_STDBY);
        out_info("Nothing received.");        // a corrupt frame is not a receive
        return 0;
    }

    int n = sx_read(SX_REG_RXNBBYTES);
    if (n > 255) n = 255;
    sx_write(SX_REG_FIFOADDRPTR, sx_read(SX_REG_FIFORXCUR));
    uint8_t data[256];
    uint8_t hdr = SX_REG_FIFO & 0x7F;
    sx_cs(0);
    spi_write_blocking(sx_bus, &hdr, 1);
    spi_read_blocking(sx_bus, 0x00, data, (size_t)n);
    sx_cs(1);

    int snr = sx_snr_db(sx_read(SX_REG_PKTSNR));
    int rssi = sx_rssi_dbm(sx_read(SX_REG_PKTRSSI), sx_is_lf(cfg.hz));
    sx_write(SX_REG_OPMODE, SX_LORA | lf | SX_STDBY);

    char body[600];
    sx_format_rx(body, sizeof(body), data, n, rssi, snr);
    out_ok("%s", body);
    return 0;
}

static int cmd_lora(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    // FIRST THINGS TO CHECK ON HARDWARE (silent on a wrong register):
    //   1. `lora status` must read ver 0x12 — else SPI/wiring is wrong (CS 21,
    //      RST 12, SCK 18, MOSI 19, MISO 16 on the Pico 2 W).
    //   2. Two boards: `lora send 48656c6c6f` on one, `lora recv 10` on the other,
    //      must yield "[@] RX 48656c6c6f rssi <n> snr <n>". If not, suspect the
    //      modem config (must match on both) or the FRF/crystal.
    //   3. `lora config 433 12 125 8` then status must report SF12 BW125 CR4/8 and
    //      still find the chip — confirms the LF-mode bit and LDRO path.

    if (!strcmp(sub, "status")) return lora_status();
    if (!strcmp(sub, "config")) return lora_config(argc, argv);
    if (!strcmp(sub, "send")) {
        if (argc < 3) { out_err("Usage: lora send <hexbytes>"); return 1; }
        return lora_send(argv[2]);
    }
    if (!strcmp(sub, "recv") || !strcmp(sub, "rx"))
        return lora_recv(argc > 2 ? (unsigned)atoi(argv[2]) : 5);

    out_multi("Usage:");
    out_multi("  lora status                              chip, frequency, modem");
    out_multi("  lora config <mhz> <sf> <bw_khz> <cr>     sf 7-12, bw 125/250/500, cr 5-8");
    out_multi("  lora send <hexbytes>                     transmit a raw payload");
    out_multi("  lora recv [secs]                         receive one packet");
    return argc > 1 ? 1 : 0;
}

void lora_register(void) {
    static const Command c{"lora", "SX1276 LoRa: status, config, send, receive",
                           cmd_lora, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}

#endif  // RADIO_HOST_TEST
