// Gpio — drive and read pins from the shell. Converted from v1's Gpio package.
//
// The interesting part of the conversion is not the pin calls, it is refusing
// the wrong ones. On RP2 an out of range pin number does not fault, it aliases
// onto some other register, so a typo turns into a device that misbehaves for
// reasons nobody traces back to the typo. fw_gpio_usable answers that per
// board, including the pins the radio owns on the wireless variants.
#include "rpc_app.h"

RPC_APP_VER("gpio", "2.0");

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

static void usage(void) {
    fw_printf("Usage:\n");
    fw_printf("  gpio read <pin>            read a pin\n");
    fw_printf("  gpio write <pin> <0|1>     drive a pin\n");
    fw_printf("  gpio toggle <pin>          flip a pin\n");
    fw_printf("  gpio pull <pin> up|down|none\n");
    fw_printf("  gpio adc <0-3>             read an ADC channel\n");
    fw_printf("  gpio temp                  the on-chip temperature sensor\n");
    fw_printf("  gpio list                  which pins this board allows\n");
}

// A pin the caller named, checked and reported on properly. Returns -1 when it
// is not usable, having already said why.
static int want_pin(const char *arg) {
    int pin = parse_pin(arg);
    if (pin < 0) {
        fw_printf("'%s' is not a pin number.\n", arg ? arg : "");
        return -1;
    }
    if (!fw_gpio_usable((unsigned)pin)) {
        if ((unsigned)pin >= fw_gpio_count())
            fw_printf("This board has pins 0-%u.\n", fw_gpio_count() - 1);
        else
            fw_printf("GPIO %d belongs to the board, not to you.\n", pin);
        return -1;
    }
    return pin;
}

static int gpio_cmd(int argc, char **argv) {
    if (argc < 2 || streq(argv[1], "help") || streq(argv[1], "-h") ||
        streq(argv[1], "--help")) {
        usage();
        return argc < 2 ? 1 : 0;
    }

    const char *sub = argv[1];

    if (streq(sub, "list")) {
        fw_printf("GPIO 0-%u on this board.\n", fw_gpio_count() - 1);
        fw_printf("Reserved: ");
        int any = 0;
        for (unsigned p = 0; p < fw_gpio_count(); p++)
            if (!fw_gpio_usable(p)) { fw_printf("%s%u", any++ ? ", " : "", p); }
        fw_printf("%s\n", any ? "" : "none");
        fw_printf("ADC channels 0-3 are GPIO 26-29; 'gpio temp' reads the die sensor.\n");
        return 0;
    }

    if (streq(sub, "temp")) {
        unsigned ch = fw_adc_temp_channel();
        if (fw_adc_init(ch) != 0) { fw_printf("Could not start the ADC.\n"); return 1; }
        int raw = fw_adc_read(ch);
        if (raw < 0) { fw_printf("Could not read the sensor.\n"); return 1; }
        // The conversion from the RP2 datasheet: 3.3 V over 12 bits, and the
        // sensor reads 0.706 V at 27 C falling 1.721 mV per degree.
        double volts = raw * 3.3 / 4096.0;
        double c = 27.0 - (volts - 0.706) / 0.001721;
        int whole = (int)c;
        int tenth = (int)((c - whole) * 10);
        if (tenth < 0) tenth = -tenth;
        fw_printf("%d.%d C  (raw %d)\n", whole, tenth, raw);
        return 0;
    }

    if (streq(sub, "adc")) {
        if (argc < 3) { fw_printf("Usage: gpio adc <0-3>\n"); return 1; }
        int ch = parse_pin(argv[2]);
        if (ch < 0 || ch > 3) { fw_printf("ADC channels are 0-3.\n"); return 1; }
        if (fw_adc_init((unsigned)ch) != 0) { fw_printf("Could not start the ADC.\n"); return 1; }
        int raw = fw_adc_read((unsigned)ch);
        if (raw < 0) { fw_printf("Could not read channel %d.\n", ch); return 1; }
        // Millivolts rather than a float, so the reading is exact as printed.
        unsigned mv = (unsigned)((raw * 3300) / 4096);
        fw_printf("channel %d (GPIO %d):  %d  =  %u mV\n", ch, 26 + ch, raw, mv);
        return 0;
    }

    if (streq(sub, "read")) {
        if (argc < 3) { fw_printf("Usage: gpio read <pin>\n"); return 1; }
        int pin = want_pin(argv[2]);
        if (pin < 0) return 1;
        fw_gpio_init((unsigned)pin, FW_PIN_IN);
        int v = fw_gpio_get((unsigned)pin);
        if (v < 0) { fw_printf("Could not read GPIO %d.\n", pin); return 1; }
        fw_printf("GPIO %d = %d\n", pin, v);
        return 0;
    }

    if (streq(sub, "write")) {
        if (argc < 4) { fw_printf("Usage: gpio write <pin> <0|1>\n"); return 1; }
        int pin = want_pin(argv[2]);
        if (pin < 0) return 1;
        int val = parse_pin(argv[3]);
        if (val != 0 && val != 1) { fw_printf("Value must be 0 or 1.\n"); return 1; }
        fw_gpio_init((unsigned)pin, FW_PIN_OUT);
        fw_gpio_put((unsigned)pin, val);
        fw_printf("GPIO %d <- %d\n", pin, val);
        return 0;
    }

    if (streq(sub, "toggle")) {
        if (argc < 3) { fw_printf("Usage: gpio toggle <pin>\n"); return 1; }
        int pin = want_pin(argv[2]);
        if (pin < 0) return 1;
        // Read as an input first, then drive the opposite. Initialising as an
        // output would drive the pin low before the read, so it would always
        // report the same answer.
        fw_gpio_init((unsigned)pin, FW_PIN_IN);
        int was = fw_gpio_get((unsigned)pin);
        fw_gpio_init((unsigned)pin, FW_PIN_OUT);
        fw_gpio_put((unsigned)pin, was ? 0 : 1);
        fw_printf("GPIO %d %d -> %d\n", pin, was, was ? 0 : 1);
        return 0;
    }

    if (streq(sub, "pull")) {
        if (argc < 4) { fw_printf("Usage: gpio pull <pin> up|down|none\n"); return 1; }
        int pin = want_pin(argv[2]);
        if (pin < 0) return 1;
        int mode;
        if      (streq(argv[3], "up"))   mode = FW_PULL_UP;
        else if (streq(argv[3], "down")) mode = FW_PULL_DOWN;
        else if (streq(argv[3], "none")) mode = FW_PULL_NONE;
        else { fw_printf("Pull must be up, down or none.\n"); return 1; }
        fw_gpio_init((unsigned)pin, FW_PIN_IN);
        fw_gpio_pull((unsigned)pin, mode);
        fw_printf("GPIO %d pull %s\n", pin, argv[3]);
        return 0;
    }

    fw_printf("Not a gpio subcommand: '%s'\n", sub);
    usage();
    return 1;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("gpio", "drive and read GPIO pins", gpio_cmd);
    return 0;
}
