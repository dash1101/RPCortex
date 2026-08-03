// The converted packages, against a fake device.
//
// Packages only ever call fw_*, so a host implementation of that ABI makes
// their logic ordinary testable code. What this covers is every decision a
// package makes: a protocol decoded, a bus scanned, a pin refused, a clock
// divider worked out. What it does not cover is the silicon — see TESTING.md.
//
// The DHT case is the reason this file exists. Its decoder distinguishes a
// 26 us pulse from a 70 us one, and getting that threshold wrong produces
// plausible-looking wrong readings rather than an error. Finding that on
// hardware needs an oscilloscope; here it is a synthesised waveform.
// Once, at global scope: the packages include it too, but the guard makes
// those no-ops so the types and fw_* declarations stay global while each
// package's rpc_app_header lands in its own namespace.
#include "rpc_app.h"
#include "fakehw.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}
static void has(const char *needle, const char *what) {
    checks++;
    if (!strstr(fake_output(), needle)) {
        printf("  FAIL: %s\n", what);
        printf("        output was: %.200s\n", fake_output());
        fails++;
    }
}
static void hasnt(const char *needle, const char *what) {
    checks++;
    if (strstr(fake_output(), needle)) {
        printf("  FAIL: %s\n", what);
        fails++;
    }
}

// The packages, compiled in.
//
// Each goes in its own namespace: they are separate translation units on the
// device and every one of them defines rpc_app_header, plus its own streq and
// parse_uint. app_main is extern "C" and so has no namespace to hide in, which
// is why that one is renamed by hand.
namespace pkg_dht {
#define app_main dht_app_main
#include "../apps/dht/dht.cpp"
#undef app_main
}
namespace pkg_i2c {
#define app_main i2c_app_main
#include "../apps/i2cscan/i2cscan.cpp"
#undef app_main
}
namespace pkg_gpio {
#define app_main gpio_app_main
#include "../apps/gpio/gpio.cpp"
#undef app_main
}
namespace pkg_ws {
#define app_main ws_app_main
#include "../apps/ws2812/ws2812.cpp"
#undef app_main
}

static int run(int (*fn)(int, char **), const char *a0, const char *a1 = nullptr,
               const char *a2 = nullptr, const char *a3 = nullptr,
               const char *a4 = nullptr, const char *a5 = nullptr,
               const char *a6 = nullptr) {
    const char *argv[8] = { a0, a1, a2, a3, a4, a5, a6, nullptr };
    int argc = 0;
    while (argc < 7 && argv[argc]) argc++;
    fake_output_clear();
    return fn(argc, (char **)argv);
}

// --- a DHT22 on a wire ------------------------------------------------------
//
// Builds the exact waveform the part emits for a given reading: the response
// pair, then 40 bits, each a 50 us low followed by a high whose LENGTH carries
// the value. If the decoder's threshold is wrong, this is what catches it.
static void script_dht22(unsigned pin, int temp_tenths, int hum_tenths,
                         bool break_checksum) {
    unsigned char b[5];
    b[0] = (unsigned char)((hum_tenths >> 8) & 0xff);
    b[1] = (unsigned char)(hum_tenths & 0xff);
    int t = temp_tenths;
    unsigned char sign = 0;
    if (t < 0) { sign = 0x80; t = -t; }
    b[2] = (unsigned char)(((t >> 8) & 0x7f) | sign);
    b[3] = (unsigned char)(t & 0xff);
    b[4] = (unsigned char)((b[0] + b[1] + b[2] + b[3]) & 0xff);
    if (break_checksum) b[4] ^= 0xff;

    int      lv[FAKE_EDGES];
    unsigned us[FAKE_EDGES];
    unsigned n = 0;

    // The package holds the line low then releases it; the part answers with
    // 80 us low and 80 us high before any data.
    lv[n] = 1; us[n++] = 30;      // the release, before the part responds
    lv[n] = 0; us[n++] = 80;      // response low
    lv[n] = 1; us[n++] = 80;      // response high

    for (int i = 0; i < 40; i++) {
        int bit = (b[i / 8] >> (7 - (i % 8))) & 1;
        lv[n] = 0; us[n++] = 50;                  // every bit starts low
        lv[n] = 1; us[n++] = bit ? 70u : 26u;     // the high carries the value
    }
    // The part pulls low once more after the final bit before it lets go.
    // Without this the decoder times out waiting for that edge and the last
    // byte comes out shifted by one — which reads exactly like a decoder bug
    // and is not one.
    lv[n] = 0; us[n++] = 50;
    lv[n] = 1; us[n++] = 10000;                   // released, idle high

    fake_pin_script(pin, lv, us, n);
}

int main(void) {
    printf("packages_test - converted packages against a fake device\n");

    // --- dht ----------------------------------------------------------------
    {
        fake_reset();
        script_dht22(2, 234, 567, false);          // 23.4 C, 56.7 %
        run(pkg_dht::dht_cmd, "dht", "2", "22");
        has("23.4 C", "dht decodes the temperature it was sent");
        has("56.7 %", "dht decodes the humidity it was sent");

        fake_reset();
        script_dht22(2, -105, 800, false);         // -10.5 C: the sign bit
        run(pkg_dht::dht_cmd, "dht", "2", "22");
        has("-10.5 C", "dht decodes a negative temperature");

        fake_reset();
        script_dht22(2, 200, 500, true);           // deliberately corrupt
        int rc = run(pkg_dht::dht_cmd, "dht", "2", "22");
        ck(rc != 0, "dht rejects a reading whose checksum is wrong");
        has("did not check out", "and says why");
        hasnt("20.0 C", "and does not report the corrupt value anyway");

        fake_reset();                              // nothing on the pin at all
        rc = run(pkg_dht::dht_cmd, "dht", "2", "22");
        ck(rc != 0, "dht fails cleanly when nothing answers");

        fake_reset();
        rc = run(pkg_dht::dht_cmd, "dht", "25");            // a pin the board owns
        ck(rc != 0, "dht refuses a reserved pin");
        has("belongs to the board", "and says whose it is");
    }

    // --- i2cscan ------------------------------------------------------------
    {
        fake_reset();
        unsigned char r = 0;
        fake_i2c_add(0x3C, &r, 1);                 // an OLED
        fake_i2c_add(0x76, &r, 1);                 // a BME280
        int rc = run(pkg_i2c::i2cscan_cmd, "i2cscan");
        ck(rc == 0, "i2cscan succeeds when devices answer");
        has("0x3C", "finds the device at 0x3C");
        has("SSD1306", "and names it");
        has("0x76", "finds the device at 0x76");
        has("BMP280", "and names that one too");
        has("2 devices found", "counts exactly the two that exist");

        fake_reset();
        rc = run(pkg_i2c::i2cscan_cmd, "i2cscan");
        ck(rc != 0, "an empty bus is a failure, not a silent success");
        has("Nothing answered", "and says so");
        // The regression that shipped: a zero-length write returns success
        // without a transaction, so every address looked present.
        hasnt("112", "an empty bus does not report every address as a device");
    }

    // --- gpio ---------------------------------------------------------------
    {
        fake_reset();
        run(pkg_gpio::gpio_cmd, "gpio", "write", "5", "1");
        ck(fake_pin_level(5) == 1, "gpio write drives the pin high");
        run(pkg_gpio::gpio_cmd, "gpio", "write", "5", "0");
        ck(fake_pin_level(5) == 0, "gpio write drives it low again");

        int rc = run(pkg_gpio::gpio_cmd, "gpio", "write", "23", "1");
        ck(rc != 0, "gpio refuses a pin the radio owns");
        ck(fake_pin_level(23) == 0, "and does not drive it anyway");

        rc = run(pkg_gpio::gpio_cmd, "gpio", "write", "99", "1");
        ck(rc != 0, "gpio refuses a pin that does not exist");
        has("0-29", "and says what the board has");

        // The datasheet conversion: 0.706 V at 27 C, -1.721 mV per degree.
        // 851 raw is 0.6856 V, which is about 38.9 C.
        fake_reset();
        fake_adc_set(4, 851);
        run(pkg_gpio::gpio_cmd, "gpio", "temp");
        has("38", "the die temperature maths is right at a known reading");

        fake_reset();
        run(pkg_gpio::gpio_cmd, "gpio", "list");
        has("23", "gpio list names the reserved pins");
        has("29", "including the last one");
    }

    // --- ws2812 -------------------------------------------------------------
    {
        // The four program words, checked against the encoding rather than
        // against themselves. Side-set in bit 12, delay in 11-8.
        fake_reset();
        fake_set_clock_hz(125000000u);
        run(pkg_ws::ws2812_cmd, "ws2812", "set", "2", "1", "255", "0", "0");

        unsigned short prog[8];
        unsigned n = fake_pio_program(0, prog, 8);
        ck(n == 4, "ws2812 loads a four-instruction program");
        ck(prog[0] == (unsigned short)(0x6000u | (0 << 12) | (2 << 8) | (1 << 5) | 1),
           "instruction 0 is out x,1 side 0 [2]");
        ck(prog[1] == (unsigned short)(0x0000u | (1 << 12) | (1 << 8) | (1 << 5) | 3),
           "instruction 1 is jmp !x,3 side 1 [1]");

        // 125 MHz / 8 MHz = 15.625, which is 4000 in 24.8 fixed point. Rounding
        // to 15 or 16 would put the bit slot far enough out to be misread, and
        // this is the bug that shipped once already.
        ck(fake_pio_divider(0) == 4000,
           "the divider keeps its fraction at 125 MHz (15.625)");

        fake_reset();
        fake_set_clock_hz(240000000u);
        run(pkg_ws::ws2812_cmd, "ws2812", "set", "6", "1", "0", "255", "0");   // a different pin: the package holds its state machine
        ck(fake_pio_divider(0) == 7680, "and is recomputed at 240 MHz (30.0)");

        // Colour order is green, red, blue, left-aligned in the word.
        fake_reset();
        run(pkg_ws::ws2812_cmd, "ws2812", "set", "2", "1", "0x00", "0", "0");
        fake_reset();
        run(pkg_ws::ws2812_cmd, "ws2812", "set", "3", "2", "1", "2", "3");
        unsigned long puts[8];
        unsigned np = fake_pio_puts(0, puts, 8);
        ck(np == 2, "one word pushed per LED");
        ck(puts[0] == 0x02010300ul, "green, red, blue, left aligned in the word");

        fake_reset();
        int rc = run(pkg_ws::ws2812_cmd, "ws2812", "set", "24", "1", "1", "1", "1");
        ck(rc != 0, "ws2812 refuses a reserved pin");
    }

    // --- the network ABI, as a package would use it -------------------------
    //
    // Not a package yet — the network conversions come next — but the calls are
    // new and this is what says they are usable rather than merely present.
    // Nova D1 reaches for `network` 78 times, so these are the ones the whole
    // conversion rests on.
    {
        fake_reset();
        ck(fw_net_connected() == 0, "offline reports offline");

        char buf[64];
        ck(fw_net_ssid(buf, sizeof(buf)) == 0 && buf[0] == 0,
           "and has no network name to give");
        ck(fw_http_get("http://example.com", buf, sizeof(buf)) < 0,
           "a fetch while offline fails rather than hanging");
        ck(fw_net_resolve("example.com", buf, sizeof(buf)) < 0,
           "and so does a lookup");

        fake_net_up("dash_", "192.168.1.50");
        ck(fw_net_connected() == 1, "connected reports connected");
        fw_net_ssid(buf, sizeof(buf));
        ck(strcmp(buf, "dash_") == 0, "and gives the network name");
        fw_net_ip(buf, sizeof(buf));
        ck(strcmp(buf, "192.168.1.50") == 0, "and the address");

        // A short buffer must truncate rather than run over. Packages hold
        // small stack buffers and this is the call that would find out.
        char tiny[4];
        fw_net_ssid(tiny, sizeof(tiny));
        ck(strlen(tiny) < sizeof(tiny), "a short buffer is truncated, not overrun");

        fake_net_add_ap("dash_", -42, 6, 1);
        fake_net_add_ap("neighbour", -71, 11, 1);
        fake_net_add_ap("open-guest", -80, 1, 0);
        FwNetAp aps[8];
        int n = fw_net_scan(aps, 8);
        ck(n == 3, "a scan returns every access point");
        ck(strcmp(aps[0].ssid, "dash_") == 0, "strongest first");
        ck(aps[0].rssi == -42 && aps[0].channel == 6, "with its signal and channel");
        ck(aps[2].secured == 0, "and whether it wants a password");

        // Asking for fewer than exist must not write past the array.
        FwNetAp two[2];
        n = fw_net_scan(two, 2);
        ck(n == 2, "a scan into a smaller array stops at its size");

        fake_http_serve("http://example.com/x", "hello from the fake network");
        int got = fw_http_get("http://example.com/x", buf, sizeof(buf));
        ck(got == 27, "a fetch returns the body length");
        ck(memcmp(buf, "hello from", 10) == 0, "and the body");

        // A body larger than the buffer is truncated to it, not refused: the
        // caller said how much room there was.
        char small[8];
        got = fw_http_get("http://example.com/x", small, sizeof(small));
        ck(got == 8, "a fetch into a small buffer fills it and stops");
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
