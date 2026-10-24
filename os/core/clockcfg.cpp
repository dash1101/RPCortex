#include "clockcfg.h"

#include <stddef.h>

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool clock_parse_mhz(const char *s, unsigned *mhz) {
    if (!s || !*s) return false;
    unsigned v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        if (++digits > 4) return false;          // far past the maximum; stops overflow
        v = v * 10 + (unsigned)(*s - '0');
        s++;
    }
    if (*s) {                                    // only "MHz" may follow the digits
        if (lower(s[0]) != 'm' || lower(s[1]) != 'h' || lower(s[2]) != 'z' || s[3]) return false;
    }
    if (v < CLOCK_MHZ_MIN || v > CLOCK_MHZ_MAX) return false;
    if (mhz) *mhz = v;
    return true;
}

BootClockPlan clock_boot_plan(const char *value, bool enabled, bool last_start_failed,
                              unsigned current_mhz, unsigned *mhz) {
    if (!value || !*value || !enabled) return BOOTCLK_NONE;
    unsigned want = 0;
    if (!clock_parse_mhz(value, &want)) return BOOTCLK_BAD;
    if (mhz) *mhz = want;
    if (last_start_failed) return BOOTCLK_AFTER_CRASH;
    if (want == current_mhz) return BOOTCLK_SAME;
    return BOOTCLK_APPLY;
}
