// The boot clock policy. A wrong answer here is a board that hangs at every
// start, or a `pulse boot` that quietly does nothing, so every branch is pinned.

#include "clockcfg.h"

#include <stdio.h>

static int checks = 0, fails = 0;

static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

static bool parses(const char *s, unsigned want) {
    unsigned v = 0;
    return clock_parse_mhz(s, &v) && v == want;
}

static void t_parse(void) {
    ck(parses("150", 150), "plain digits");
    ck(parses("250MHz", 250), "MHz suffix");
    ck(parses("250mhz", 250), "suffix in lower case");
    ck(parses("250mHZ", 250), "suffix in any case");
    ck(parses("48", 48), "the lowest clock is accepted");
    ck(parses("400", 400), "the highest clock is accepted");

    unsigned v = 7;
    ck(!clock_parse_mhz("47", &v), "one under the lowest is refused");
    ck(!clock_parse_mhz("401", &v), "one over the highest is refused");
    ck(!clock_parse_mhz("0", &v), "zero is refused");
    ck(!clock_parse_mhz("", &v), "empty is refused");
    ck(!clock_parse_mhz(nullptr, &v), "null is refused");
    ck(!clock_parse_mhz("abc", &v), "letters are refused");
    ck(!clock_parse_mhz("250x", &v), "trailing junk is refused");
    ck(!clock_parse_mhz("250 MHz", &v), "a space before the suffix is refused");
    ck(!clock_parse_mhz("250MHzz", &v), "a longer suffix is refused");
    ck(!clock_parse_mhz("250M", &v), "half a suffix is refused");
    ck(!clock_parse_mhz("250Hz", &v), "the wrong suffix is refused");
    ck(!clock_parse_mhz("250MHx", &v), "a suffix wrong in its last letter is refused");
    ck(!clock_parse_mhz("250MH", &v), "a suffix cut short is refused");
    ck(!clock_parse_mhz(" 250", &v), "a leading space is refused");
    ck(!clock_parse_mhz("-150", &v), "a sign is refused");
    ck(!clock_parse_mhz("1.5", &v), "a fraction is refused");
    ck(!clock_parse_mhz("MHz", &v), "a suffix alone is refused");
    ck(!clock_parse_mhz("99999", &v), "five digits are refused");
    ck(!clock_parse_mhz("4294967296", &v), "a value that would wrap is refused");
    ck(!clock_parse_mhz("4294967446", &v), "a value that wraps to a valid clock is refused");
    ck(!clock_parse_mhz("0000000250", &v), "a long run of leading zeros is refused, not parsed");
    ck(v == 7, "a refused value leaves the output alone");
    ck(clock_parse_mhz("150", nullptr), "the output may be null");
}

static void t_plan(void) {
    unsigned m = 0;

    ck(clock_boot_plan(nullptr, true, false, 150, &m) == BOOTCLK_NONE, "null value: nothing to do");
    ck(clock_boot_plan("", true, false, 150, &m) == BOOTCLK_NONE, "empty value: nothing to do");
    ck(clock_boot_plan("250", false, false, 150, &m) == BOOTCLK_NONE, "switched off: nothing to do");
    // Off wins over everything, including a bad value and a crash: nobody asked.
    ck(clock_boot_plan("junk", false, true, 150, &m) == BOOTCLK_NONE, "off is silent even for a bad value after a crash");

    m = 0;
    ck(clock_boot_plan("250", true, false, 150, &m) == BOOTCLK_APPLY, "a good clock is applied");
    ck(m == 250, "and the plan says which");
    ck(clock_boot_plan("250MHz", true, false, 150, &m) == BOOTCLK_APPLY, "with the suffix too");
    ck(clock_boot_plan("150", true, false, 150, &m) == BOOTCLK_SAME, "already there: not set again");
    ck(clock_boot_plan("150", true, false, 250, &m) == BOOTCLK_APPLY, "lowering is applied as well");

    ck(clock_boot_plan("junk", true, false, 150, &m) == BOOTCLK_BAD, "a stored value that is not a clock is reported");
    ck(clock_boot_plan("9999", true, false, 150, &m) == BOOTCLK_BAD, "an out-of-range clock is reported");
    ck(clock_boot_plan("junk", true, true, 150, &m) == BOOTCLK_BAD, "a bad value is reported even after a crash");

    // The safeguard.
    m = 0;
    ck(clock_boot_plan("250", true, true, 150, &m) == BOOTCLK_AFTER_CRASH, "after a failed start it is not applied");
    ck(m == 250, "but the message can name the clock it skipped");
    ck(clock_boot_plan("150", true, true, 150, &m) == BOOTCLK_AFTER_CRASH, "the crash check comes before the already-there check");
    ck(clock_boot_plan("250", true, false, 150, nullptr) == BOOTCLK_APPLY, "the output may be null");
}

int main(void) {
    t_parse();
    t_plan();
    printf("  clockcfg: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
