// WS2812 — addressable LEDs, and the first package that could not exist
// without PIO.
//
// The protocol has no clock. A bit is the RATIO of high time to low time in a
// 1.25 us slot: about 400 ns high for a zero, 800 ns for a one, and anything
// more than roughly 150 ns off is read as the other value. A CPU loop measured
// on this device varies by several microseconds when something else is
// happening, which is ten times the entire budget — so this is not a matter of
// writing a tighter loop, it cannot be done that way at all.
//
// A state machine can. It runs on its own hardware at a fixed clock and is not
// interrupted, so the timing below is exact whether the device is idle or
// downloading a firmware update.
#include "rpc_app.h"

RPC_APP_VER("ws2812", "1.0");

// The program. Four instructions, and all of the timing is in the delays.
//
// Side-set drives the line, so each instruction says what the pin should be
// while it executes. One bit is:
//
//   T1 high (start)  ->  T2 the data bit  ->  T3 low (finish)
//
// with the total fixed at 10 cycles. Run the state machine at 8 MHz and a slot
// is 1.25 us, which is the number the part wants.
#define T1 2
#define T2 5
#define T3 3

// Encoded by hand rather than through the SET/JMP macros, because side-set
// occupies the top bits of the delay field and the general encoders do not
// know how many of those this program claimed.
//
//   bit 15-13 opcode, 12-8 side-set + delay, 7-0 operands
//
// With one side-set bit, 12 is the pin level and 11-8 are the delay.
#define SIDE(level, delay)  ((unsigned short)((((level) & 1) << 12) | (((delay) & 0xf) << 8)))

static const unsigned short kProgram[] = {
    // 0: out x, 1        side 0 [T3-1]   drive low, pull the next bit
    (unsigned short)(0x6000u | SIDE(0, T3 - 1) | (1 << 5) | 1),
    // 1: jmp !x, 3       side 1 [T1-1]   go high; branch on the bit
    (unsigned short)(0x0000u | SIDE(1, T1 - 1) | (1 << 5) | 3),
    // 2: jmp 0           side 1 [T2-1]   a one: stay high longer
    (unsigned short)(0x0000u | SIDE(1, T2 - 1) | 0),
    // 3: jmp 0           side 0 [T2-1]   a zero: drop early
    (unsigned short)(0x0000u | SIDE(0, T2 - 1) | 0),
};

static bool streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int parse_uint(const char *s, int fallback) {
    if (!s || !*s) return fallback;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return fallback;
        v = v * 10 + (*s - '0');
        if (v > 100000) return fallback;
    }
    return v;
}

// One strip, held between commands so a package can drive it repeatedly
// without re-claiming hardware every time.
static int      g_sm  = -1;
static unsigned g_pin = 0;

static int strip_open(unsigned pin) {
    if (g_sm >= 0 && g_pin == pin) return 0;      // already up on this pin
    if (g_sm >= 0) { fw_pio_stop(g_sm); fw_pio_release(g_sm); g_sm = -1; }

    int h = fw_pio_claim();
    if (h < 0) {
        fw_printf("No PIO state machine free (%u of %u in use).\n",
                  fw_pio_count() - fw_pio_free(), fw_pio_count());
        return -1;
    }
    // wrap back to 0 after the last instruction: the program is a loop with no
    // instruction spent on jumping, which is why it fits in four.
    if (fw_pio_load(h, kProgram, 4, 0, 3) < 0) {
        fw_printf("Could not load the program - PIO instruction memory is full.\n");
        fw_pio_release(h);
        return -1;
    }
    // Side-set only: the pin is driven by the side-set field, not by OUT.
    if (fw_pio_config_pins(h, 0, 0, 0, 0, pin, 1) != 0) {
        fw_printf("GPIO %u cannot be used for this.\n", pin);
        fw_pio_release(h);
        return -1;
    }
    // Shift left, autopull at 24 bits: the colour order is green, red, blue,
    // most significant bit first, which is what shifting left delivers.
    fw_pio_config_shift(h, FW_PIO_SHIFT_LEFT, 1, 24);

    // 10 cycles per bit at 1.25 us means the state machine wants 8 MHz.
    //
    // Derived from the ACTUAL clock, not a constant. The default differs
    // between RP2040 and RP2350 and the clock is adjustable at runtime — this
    // board was running at 240 MHz when the package was written, and a divider
    // worked out against that would have been wrong on every stock one.
    //
    // 24.8 fixed point, so the fraction survives: at 125 MHz the divider is
    // 15.625, and rounding it to 15 or 16 puts the bit slot far enough out to
    // be read as the wrong value.
    unsigned long div_x256 = ((unsigned long)fw_clock_hz() * 256UL) / 8000000UL;
    if (div_x256 < 256) div_x256 = 256;       // never faster than full speed
    fw_pio_config_clock(h, (unsigned)div_x256);

    if (fw_pio_start(h) != 0) {
        fw_printf("The state machine would not start.\n");
        fw_pio_release(h);
        return -1;
    }
    g_sm = h;
    g_pin = pin;
    return 0;
}

// Colours are pushed left-aligned: the state machine shifts out of the top, so
// 24 bits of data sit in the high end of the word.
static void put_pixel(unsigned r, unsigned g, unsigned b) {
    unsigned long v = ((unsigned long)(g & 0xff) << 16) |
                      ((unsigned long)(r & 0xff) << 8)  |
                       (unsigned long)(b & 0xff);
    fw_pio_put(g_sm, v << 8, 10000);
}

static int ws2812_cmd(int argc, char **argv) {
    if (argc < 2 || streq(argv[1], "help") || streq(argv[1], "-h")) {
        fw_printf("Usage:\n");
        fw_printf("  ws2812 set <pin> <count> <r> <g> <b>   every LED one colour\n");
        fw_printf("  ws2812 off <pin> <count>               all off\n");
        fw_printf("  ws2812 chase <pin> <count> [times]     one lit LED, moving\n");
        fw_printf("  ws2812 info                            PIO usage\n");
        return argc < 2 ? 1 : 0;
    }

    if (streq(argv[1], "info")) {
        fw_printf("PIO state machines: %u free of %u\n", fw_pio_free(), fw_pio_count());
        if (g_sm >= 0) fw_printf("This strip holds one, on GPIO %u.\n", g_pin);
        return 0;
    }

    if (argc < 4) { fw_printf("Needs a pin and a count.\n"); return 1; }
    int pin   = parse_uint(argv[2], -1);
    int count = parse_uint(argv[3], -1);
    if (pin < 0 || count < 1 || count > 1000) {
        fw_printf("Pin and count must be sensible numbers.\n");
        return 1;
    }
    if (!fw_gpio_usable((unsigned)pin)) {
        fw_printf("GPIO %d belongs to the board, or does not exist.\n", pin);
        return 1;
    }
    if (strip_open((unsigned)pin) != 0) return 1;

    if (streq(argv[1], "off")) {
        for (int i = 0; i < count; i++) put_pixel(0, 0, 0);
        fw_printf("%d LED%s off.\n", count, count == 1 ? "" : "s");
        return 0;
    }

    if (streq(argv[1], "set")) {
        if (argc < 7) { fw_printf("Usage: ws2812 set <pin> <count> <r> <g> <b>\n"); return 1; }
        int r = parse_uint(argv[4], 0), g = parse_uint(argv[5], 0), b = parse_uint(argv[6], 0);
        if (r > 255 || g > 255 || b > 255) { fw_printf("Colours are 0-255.\n"); return 1; }
        for (int i = 0; i < count; i++) put_pixel((unsigned)r, (unsigned)g, (unsigned)b);
        fw_printf("%d LED%s set to %d,%d,%d.\n", count, count == 1 ? "" : "s", r, g, b);
        return 0;
    }

    if (streq(argv[1], "chase")) {
        int times = argc > 4 ? parse_uint(argv[4], 3) : 3;
        for (int t = 0; t < times; t++) {
            for (int lit = 0; lit < count; lit++) {
                for (int i = 0; i < count; i++)
                    if (i == lit) put_pixel(40, 0, 60); else put_pixel(0, 0, 0);
                // Sleeps rather than busy-waits: the state machine has the
                // frame already and does not care what the CPU does next,
                // which is the entire point of driving it this way.
                fw_task_sleep_ms(40);
                if (fw_task_should_stop()) { fw_printf("Stopped.\n"); return 0; }
            }
        }
        for (int i = 0; i < count; i++) put_pixel(0, 0, 0);
        return 0;
    }

    fw_printf("Not a ws2812 subcommand: '%s'\n", argv[1]);
    return 1;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("ws2812", "drive addressable LEDs through PIO", ws2812_cmd);
    return 0;
}
