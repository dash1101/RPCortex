// The two SPI radios, host-tested where they can be.
//
// Neither a CC1101 nor an SX1276 has ever been on the bus, and a wrong register
// write on either is a silent failure — the SPI succeeds and nothing radiates or
// nothing is heard. So the parts that DON'T need the radio are pinned here: the
// frequency->register maths (checked against SmartRF/published values, not just
// round-tripped), band limits, the modem-config bit packing and its
// LowDataRateOptimize threshold, hex and timing parsing, and the exact text of
// the lines the mesh agent and the Nova D1 screens parse.
//
// The firmware pure sections are reached by defining RADIO_HOST_TEST and
// including the .cpp — the same trick btadv_test uses on bt.cpp. The screen parse
// helpers are reached with RADIO_PARSE_ONLY on novagui_radios.cpp, and asserted
// against the SAME strings the fake firmware (fakefw_d1.inc) hands the screens, so
// the round trip firmware-format -> fake -> parse is actually exercised.

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define RADIO_HOST_TEST
#include "../shell/cc1101.cpp"
#include "../shell/sx1276.cpp"

#define RADIO_PARSE_ONLY
#include "../apps/novad1/novagui_radios.cpp"

static int checks, failures;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { failures++; printf("    FAIL %s\n", what); }
}
static void eqi(long got, long want, const char *what) {
    checks++;
    if (got != want) { failures++; printf("    FAIL %s: got %ld want %ld\n", what, got, want); }
}
static void eqs(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want) != 0) { failures++; printf("    FAIL %s: got '%s' want '%s'\n", what, got, want); }
}

// --- CC1101 frequency maths -------------------------------------------------
//
// The register bytes are checked against the values SmartRF Studio and every
// CC1101 reference publish for these channels, so this catches a broken formula
// rather than merely agreeing with itself.
static void test_cc_freq(void) {
    uint8_t r[3];
    cc_freq_to_regs(433920000u, r);
    eqi(r[0], 0x10, "cc 433.92 FREQ2");
    eqi(r[1], 0xB0, "cc 433.92 FREQ1");
    eqi(r[2], 0x71, "cc 433.92 FREQ0");

    // Round-trip within one LSB (~397 Hz at 26 MHz / 2^16) for each band.
    const uint32_t fs[] = { 315000000u, 433920000u, 868350000u, 915000000u };
    for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]); i++) {
        cc_freq_to_regs(fs[i], r);
        uint32_t back = cc_regs_to_freq(r);
        long diff = (long)back - (long)fs[i];
        if (diff < 0) diff = -diff;
        ok(diff < 400, "cc freq round-trips within a step");
    }

    ok(cc_band_ok(315000000u), "cc 315 in band");
    ok(cc_band_ok(433920000u), "cc 433.92 in band");
    ok(cc_band_ok(915000000u), "cc 915 in band");
    ok(!cc_band_ok(200000000u), "cc 200 out of band");
    ok(!cc_band_ok(500000000u), "cc 500 (between bands) rejected");
    ok(!cc_band_ok(950000000u), "cc 950 out of band");
}

static void test_cc_mhz_parse(void) {
    eqi((long)cc_parse_mhz_to_hz("433.92"), 433920000, "cc parse 433.92");
    eqi((long)cc_parse_mhz_to_hz("915"), 915000000, "cc parse 915");
    eqi((long)cc_parse_mhz_to_hz("315.0"), 315000000, "cc parse 315.0");
    eqi((long)cc_parse_mhz_to_hz(""), 0, "cc parse empty");
    eqi((long)cc_parse_mhz_to_hz("abc"), 0, "cc parse junk");
    eqi((long)cc_parse_mhz_to_hz("433.92x"), 0, "cc parse trailing junk");
    char b[16];
    cc_format_mhz(b, sizeof(b), 433920000u); eqs(b, "433.92", "cc format 433.92");
    cc_format_mhz(b, sizeof(b), 915000000u); eqs(b, "915.00", "cc format 915.00");
}

static void test_cc_hex_and_timings(void) {
    uint8_t bytes[8];
    eqi((long)cc_parse_hex("8a2b", bytes, sizeof(bytes)), 2, "cc hex len");
    eqi(bytes[0], 0x8a, "cc hex[0]"); eqi(bytes[1], 0x2b, "cc hex[1]");
    eqi((long)cc_parse_hex("8a2", bytes, sizeof(bytes)), 0, "cc hex odd rejected");
    eqi((long)cc_parse_hex("zz", bytes, sizeof(bytes)), 0, "cc hex bad digit");

    eqi(cc_clamp_pulse(500), 500, "cc clamp small");
    eqi(cc_clamp_pulse(70000), 60000, "cc clamp large");

    uint16_t t[8];
    int n = cc_parse_timings("350,700,350,700", t, 8);
    eqi(n, 4, "cc timings count");
    eqi(t[0], 350, "cc timing[0]"); eqi(t[1], 700, "cc timing[1]");

    // The round trip that ties `rx` output to `tx` input: format then re-parse.
    char line[64];
    cc_format_timings(line, sizeof(line), t, n);
    eqs(line, "350,700,350,700", "cc format timings");
    uint16_t t2[8];
    int n2 = cc_parse_timings(line, t2, 8);
    eqi(n2, n, "cc timings survive a round trip");
    for (int i = 0; i < n; i++) eqi(t2[i], t[i], "cc timing survives");

    ok(!cc_arg_is_hex("350,700"), "cc comma is timings");
    ok(cc_arg_is_hex("016e02bc"), "cc no comma is hex");
    uint16_t th[4];
    int nh = cc_hex_to_timings("016e02bc", th, 4);
    eqi(nh, 2, "cc hex->timings count");
    eqi(th[0], 0x016e, "cc hex->timing[0]");
    eqi(th[1], 0x02bc, "cc hex->timing[1]");
}

// --- SX1276 frequency maths -------------------------------------------------
static void test_sx_freq(void) {
    uint8_t f[3];
    // Published SX1276 Frf values.
    sx_freq_to_frf(915000000u, f);
    eqi(f[0], 0xE4, "sx 915 FrfMsb"); eqi(f[1], 0xC0, "sx 915 FrfMid"); eqi(f[2], 0x00, "sx 915 FrfLsb");
    sx_freq_to_frf(868000000u, f);
    eqi(f[0], 0xD9, "sx 868 FrfMsb"); eqi(f[1], 0x00, "sx 868 FrfMid"); eqi(f[2], 0x00, "sx 868 FrfLsb");
    sx_freq_to_frf(434000000u, f);
    eqi(f[0], 0x6C, "sx 434 FrfMsb"); eqi(f[1], 0x80, "sx 434 FrfMid"); eqi(f[2], 0x00, "sx 434 FrfLsb");

    const uint32_t fs[] = { 434000000u, 868000000u, 915000000u };
    for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]); i++) {
        sx_freq_to_frf(fs[i], f);
        long diff = (long)sx_frf_to_freq(f) - (long)fs[i];
        if (diff < 0) diff = -diff;
        ok(diff < 62, "sx freq round-trips within a step");
    }

    ok(sx_is_lf(434000000u), "sx 434 is LF");
    ok(!sx_is_lf(915000000u), "sx 915 is HF");
    ok(!sx_is_lf(525000000u), "sx 525 boundary is HF");

    eqi((long)sx_parse_mhz_to_hz("915"), 915000000, "sx parse 915");
    eqi((long)sx_parse_mhz_to_hz("433.92"), 433920000, "sx parse 433.92");
    eqi((long)sx_parse_mhz_to_hz("junk"), 0, "sx parse junk");
    char b[16];
    sx_format_mhz(b, sizeof(b), 915000000u); eqs(b, "915.00", "sx format 915.00");
    sx_format_mhz(b, sizeof(b), 433920000u); eqs(b, "433.92", "sx format 433.92");
}

static void test_sx_modem(void) {
    uint8_t mc1, mc2, mc3;
    // SF7 BW125 CR4/5 — the values v1 hard-coded, so this pins the packing.
    ok(sx_modem_cfg(7, 125, 5, true, &mc1, &mc2, &mc3), "sx cfg SF7/125/4-5 valid");
    eqi(mc1, 0x72, "sx mc1 SF7/125/4-5");
    eqi(mc2, 0x74, "sx mc2 SF7/125/4-5 (CRC on)");
    eqi(mc3, 0x04, "sx mc3 SF7/125 (no LDRO)");

    // SF12 BW125 CR4/8 — LDRO must switch on (symbol > 16 ms).
    ok(sx_modem_cfg(12, 125, 8, true, &mc1, &mc2, &mc3), "sx cfg SF12/125/4-8 valid");
    eqi(mc1, 0x78, "sx mc1 SF12/125/4-8");
    eqi(mc2, 0xC4, "sx mc2 SF12/125/4-8");
    eqi(mc3, 0x0C, "sx mc3 SF12/125 (LDRO on)");

    // SF10 BW250 CR4/6.
    ok(sx_modem_cfg(10, 250, 6, true, &mc1, &mc2, &mc3), "sx cfg SF10/250/4-6 valid");
    eqi(mc1, 0x84, "sx mc1 SF10/250/4-6");
    eqi(mc2, 0xA4, "sx mc2 SF10/250/4-6");

    // The LDRO threshold both sides of the line at BW125: SF11 on, SF10 off.
    sx_modem_cfg(11, 125, 5, true, &mc1, &mc2, &mc3); eqi(mc3, 0x0C, "sx SF11/125 LDRO on");
    sx_modem_cfg(10, 125, 5, true, &mc1, &mc2, &mc3); eqi(mc3, 0x04, "sx SF10/125 LDRO off");
    // ...and that widening BW lifts the threshold: SF11 on 250 is fine.
    sx_modem_cfg(11, 250, 5, true, &mc1, &mc2, &mc3); eqi(mc3, 0x04, "sx SF11/250 LDRO off");

    ok(!sx_modem_cfg(6, 125, 5, true, &mc1, &mc2, &mc3), "sx SF6 rejected");
    ok(!sx_modem_cfg(13, 125, 5, true, &mc1, &mc2, &mc3), "sx SF13 rejected");
    ok(!sx_modem_cfg(7, 100, 5, true, &mc1, &mc2, &mc3), "sx BW100 rejected");
    ok(!sx_modem_cfg(7, 125, 4, true, &mc1, &mc2, &mc3), "sx CR4/4 rejected");
    ok(!sx_modem_cfg(7, 125, 9, true, &mc1, &mc2, &mc3), "sx CR4/9 rejected");
}

static void test_sx_rx_decode(void) {
    eqi(sx_snr_db(0x24), 9, "sx snr +9 dB");     // 36 * 0.25
    eqi(sx_snr_db(0xF0), -4, "sx snr -4 dB");    // (int8)0xF0 = -16, /4
    eqi(sx_rssi_dbm(115, false), -42, "sx rssi HF");
    eqi(sx_rssi_dbm(115, true), -49, "sx rssi LF");

    uint8_t data[8];
    eqi(sx_parse_hex("48656c6c6f", data, 8), 5, "sx parse hex len");
    eqi(sx_parse_hex("abc", data, 8), -1, "sx parse hex odd");
    char hex[32];
    sx_bytes_to_hex((const uint8_t *)"Hello", 5, hex, sizeof(hex));
    eqs(hex, "48656c6c6f", "sx bytes->hex");

    char body[64];
    sx_format_rx(body, sizeof(body), (const uint8_t *)"Hello", 5, -42, 9);
    eqs(body, "RX 48656c6c6f rssi -42 snr 9", "sx RX line (the mesh contract body)");
}

// --- the screen parse side --------------------------------------------------
//
// Asserts the novad1 screens parse the EXACT strings the firmware emits — the
// same strings fakefw_d1.inc reproduces for novashots. Defined in
// nova::screens::radios and declared in the parse-only build of novagui_radios.
static void test_screen_parse(void) {
    using namespace nova::screens::radios;

    // Sub-GHz status: present flag, part/ver, and the frequency string.
    RadioStatus rs;
    parse_subghz_status(
        "[:] Sub-GHz  CC1101\n"
        "  Chip    present  (part 0x00 ver 0x14)\n"
        "  Freq    433.92 MHz\n"
        "  Mode    OOK/ASK fixed-code\n", &rs);
    ok(rs.present, "subghz status present parsed");
    eqs(rs.detail, "433.92 MHz", "subghz freq parsed");

    parse_subghz_status(
        "[:] Sub-GHz  CC1101\n"
        "  Chip    absent   (part 0x00 ver 0x00)\n"
        "  Freq    433.92 MHz\n", &rs);
    ok(!rs.present, "subghz status absent parsed");

    // A capture: pull the replayable timing line out of the rx output.
    char cap[128];
    ok(parse_subghz_capture(
        "[@] Captured 6 pulses at 433.92 MHz\n"
        "350,700,350,700,350,700\n", cap, sizeof(cap)),
       "subghz capture line found");
    eqs(cap, "350,700,350,700,350,700", "subghz capture timings extracted");

    ok(!parse_subghz_capture("[:] Nothing captured.\n", cap, sizeof(cap)),
       "subghz nothing-captured is not a capture");

    // The pulse count the screen shows (and gates Replay on).
    eqi(parse_subghz_pulses(
        "[@] Captured 6 pulses at 433.92 MHz\n350,700,350,700,350,700\n"), 6,
        "subghz pulse count");
    eqi(parse_subghz_pulses("[:] Nothing captured.\n"), 0,
        "subghz nothing-captured -> 0 pulses");

    // LoRa status.
    parse_lora_status(
        "[:] LoRa  SX1276\n"
        "  Chip   present  (ver 0x12)\n"
        "  Freq   915.00 MHz\n"
        "  Modem  SF7 BW125 CR4/5\n", &rs);
    ok(rs.present, "lora status present parsed");
    eqs(rs.detail, "SF7 BW125 CR4/5", "lora modem parsed");

    // LoRa recv — the contract lines.
    LoraRx rx;
    ok(parse_lora_recv("[@] RX 48656c6c6f rssi -42 snr 9\n", &rx), "lora recv parsed");
    eqs(rx.hex, "48656c6c6f", "lora recv hex");
    eqi(rx.rssi, -42, "lora recv rssi");
    eqi(rx.snr, 9, "lora recv snr");

    ok(!parse_lora_recv("[:] Nothing received.\n", &rx), "lora nothing-received not a packet");
}

#define STAGE(f) do { fprintf(stderr, "  .. %s\n", #f); f(); } while (0)

int main(void) {
    STAGE(test_cc_freq);
    STAGE(test_cc_mhz_parse);
    STAGE(test_cc_hex_and_timings);
    STAGE(test_sx_freq);
    STAGE(test_sx_modem);
    STAGE(test_sx_rx_decode);
    STAGE(test_screen_parse);

    printf("  %d checks", checks);
    if (failures) printf(", %d FAILED", failures);
    printf("\n");
    return failures ? 1 : 0;
}
