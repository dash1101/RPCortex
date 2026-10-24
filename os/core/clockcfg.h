// The boot clock: what `pulse boot` stores, and what the next start does about it.
//
// Pure, so the policy host-tests. The part that cannot be tested here is the one
// line that sets the PLL; the part that decides WHETHER to is where the mistakes
// are, and a mistake in it is a board that will not start.
#ifndef RPC_CLOCKCFG_H
#define RPC_CLOCKCFG_H

#define CLOCK_MHZ_MIN 48
#define CLOCK_MHZ_MAX 400

// "250" or "250MHz" (any case) to 250. Digits only: "250x", "-5", "", " 250" and
// a number outside CLOCK_MHZ_MIN..CLOCK_MHZ_MAX are all refused, so a typo is an
// error and not a clock.
bool clock_parse_mhz(const char *s, unsigned *mhz);

enum BootClockPlan {
    BOOTCLK_NONE,          // nothing stored, or switched off
    BOOTCLK_SAME,          // already running at it
    BOOTCLK_APPLY,         // set it now
    BOOTCLK_BAD,           // a value is stored but it is not a frequency
    BOOTCLK_AFTER_CRASH    // would apply, but the last start did not finish
};

// What this start should do. `value` is the stored clock (may be empty or null),
// `enabled` whether it is switched on, `last_start_failed` whether the previous
// run ended in a reset nobody asked for, `current_mhz` where the chip is now.
// `*mhz` is set whenever the value parses, including for AFTER_CRASH, so the
// message can say which clock was skipped.
//
// A start after a failed one never applies it: an overclock that hangs the board
// would otherwise hang it again on every start. The setting is left in place, so
// one clean restart brings it back; `pulse boot off` is the way to stop it.
BootClockPlan clock_boot_plan(const char *value, bool enabled, bool last_start_failed,
                              unsigned current_mhz, unsigned *mhz);

#endif  // RPC_CLOCKCFG_H
