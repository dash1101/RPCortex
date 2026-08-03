// DHT — read a DHT11 or DHT22 temperature and humidity sensor.
//
// v1's package was a thin wrapper: MicroPython ships a `dht` module that does
// the bit-banging in C, so the package only had to call it. There is no such
// module here, so the protocol below IS the conversion — and it is the first
// package whose correctness depends on microsecond timing rather than on logic.
//
// The wire protocol, which is the same for both parts apart from the start
// pulse and how the five bytes are interpreted:
//
//   host    pulls the line low     >=18 ms (DHT11) or >=1 ms (DHT22)
//   host    releases; the pull-up takes it high for 20-40 us
//   sensor  answers: 80 us low, then 80 us high
//   sensor  sends 40 bits, each one 50 us low then high for
//           26-28 us (a zero) or 70 us (a one)
//
// So a bit is read by timing the HIGH, not by sampling it. Anything around
// 40 us splits the two cleanly, and that is the whole decoder.
#include "rpc_app.h"

RPC_APP_VER("dht", "2.0");

#define DHT_TIMEOUT_US 200      // no edge in this long means the sensor is gone

enum DhtErr {
    DHT_OK = 0,
    DHT_NO_RESPONSE,            // nothing pulled the line down
    DHT_TIMEOUT,                // it started talking and stopped
    DHT_CHECKSUM,               // all 40 bits arrived and did not add up
};

// Wait for the line to reach `level`, returning how long that took. -1 if it
// never got there. Busy, deliberately: yielding here loses the reading.
static int wait_level(unsigned pin, int level) {
    uint32_t start = fw_micros();
    while (fw_gpio_get(pin) != level) {
        if (fw_micros() - start > DHT_TIMEOUT_US) return -1;
    }
    return (int)(fw_micros() - start);
}

// One exchange. `bytes` receives the five the sensor sends.
static DhtErr dht_read_raw(unsigned pin, bool is_dht22, unsigned char *bytes) {
    // The start pulse. Held by an output, so the only requirement is "at least
    // this long" — which means it can be a proper yielding sleep rather than
    // 18 ms of held core. Everything after this point cannot.
    fw_gpio_init(pin, FW_PIN_OUT);
    fw_gpio_put(pin, 0);
    fw_task_sleep_ms(is_dht22 ? 2 : 20);

    // Release and hand the line to the pull-up. From here to the last bit is
    // about 5 ms with no yield in it.
    fw_gpio_init(pin, FW_PIN_IN);
    fw_gpio_pull(pin, FW_PULL_UP);

    if (wait_level(pin, 0) < 0) return DHT_NO_RESPONSE;   // sensor's 80 us low
    if (wait_level(pin, 1) < 0) return DHT_TIMEOUT;       // sensor's 80 us high
    if (wait_level(pin, 0) < 0) return DHT_TIMEOUT;       // first bit's low

    for (int i = 0; i < 40; i++) {
        if (wait_level(pin, 1) < 0) return DHT_TIMEOUT;   // the 50 us low ended
        int high = wait_level(pin, 0);                    // time the high
        if (high < 0) return DHT_TIMEOUT;

        // 26-28 us is a zero, 70 us is a one. 40 splits them with room either
        // side, which matters because the timer is only as good as the loop.
        bytes[i / 8] <<= 1;
        if (high > 40) bytes[i / 8] |= 1;
    }

    unsigned sum = (unsigned)bytes[0] + bytes[1] + bytes[2] + bytes[3];
    if ((sum & 0xff) != bytes[4]) return DHT_CHECKSUM;
    return DHT_OK;
}

static bool streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int parse_pin(const char *s) {
    if (!s || !*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 999) return -1;
    }
    return v;
}

static const char *err_text(DhtErr e) {
    switch (e) {
        case DHT_NO_RESPONSE: return "nothing answered on that pin";
        case DHT_TIMEOUT:     return "the sensor started replying and stopped";
        case DHT_CHECKSUM:    return "the reading did not check out";
        default:              return "unknown";
    }
}

static int dht_cmd(int argc, char **argv) {
    if (argc < 2 || streq(argv[1], "help") || streq(argv[1], "-h") ||
        streq(argv[1], "--help")) {
        fw_printf("Usage: dht <pin> [11|22]\n");
        fw_printf("  Reads a DHT11 (default) or DHT22 on that GPIO.\n");
        fw_printf("  The data line wants a pull-up to 3V3; many modules have one.\n");
        return argc < 2 ? 1 : 0;
    }

    int pin = parse_pin(argv[1]);
    if (pin < 0) { fw_printf("'%s' is not a pin number.\n", argv[1]); return 1; }
    if (!fw_gpio_usable((unsigned)pin)) {
        if ((unsigned)pin >= fw_gpio_count())
            fw_printf("This board has pins 0-%u.\n", fw_gpio_count() - 1);
        else
            fw_printf("GPIO %d belongs to the board, not to you.\n", pin);
        return 1;
    }

    bool is_dht22 = false;
    if (argc > 2) {
        if      (streq(argv[2], "22") || streq(argv[2], "dht22")) is_dht22 = true;
        else if (streq(argv[2], "11") || streq(argv[2], "dht11")) is_dht22 = false;
        else { fw_printf("Sensor must be 11 or 22.\n"); return 1; }
    }

    // Up to three attempts. A single missed reading is normal on these parts —
    // they are slow, they will not be polled faster than every couple of
    // seconds, and one retry is the difference between "works" and "flaky".
    unsigned char b[5];
    DhtErr e = DHT_NO_RESPONSE;
    for (int attempt = 0; attempt < 3; attempt++) {
        for (int i = 0; i < 5; i++) b[i] = 0;
        e = dht_read_raw((unsigned)pin, is_dht22, b);
        if (e == DHT_OK) break;
        if (fw_task_should_stop()) break;
        fw_task_sleep_ms(2000);        // the parts need this long between reads
    }

    if (e != DHT_OK) {
        fw_printf("Could not read the sensor: %s.\n", err_text(e));
        fw_printf("  Check the wiring, that it is a DHT%s, and that the data\n",
                  is_dht22 ? "22" : "11");
        fw_printf("  line has a pull-up. Give it two seconds between readings.\n");
        return 1;
    }

    // Tenths throughout, so the value prints exactly as measured with no
    // floating point anywhere in the package.
    int temp_tenths, hum_tenths;
    if (is_dht22) {
        hum_tenths  = (b[0] << 8) | b[1];
        temp_tenths = ((b[2] & 0x7f) << 8) | b[3];
        if (b[2] & 0x80) temp_tenths = -temp_tenths;     // sign is the top bit
    } else {
        // DHT11 sends whole numbers, with the "decimal" byte usually zero.
        hum_tenths  = b[0] * 10 + (b[1] < 10 ? b[1] : 0);
        temp_tenths = b[2] * 10 + (b[3] < 10 ? b[3] : 0);
    }

    int t_whole = temp_tenths / 10, t_frac = temp_tenths % 10;
    int h_whole = hum_tenths  / 10, h_frac = hum_tenths  % 10;
    if (t_frac < 0) t_frac = -t_frac;

    fw_printf("DHT%s on GPIO %d\n", is_dht22 ? "22" : "11", pin);
    fw_printf("  Temperature  %d.%d C\n", t_whole, t_frac);
    fw_printf("  Humidity     %d.%d %%\n", h_whole, h_frac);
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("dht", "read a DHT11 or DHT22 sensor", dht_cmd);
    return 0;
}
