// The rules about what a sleep request is allowed to be.
//
// The sleep itself cannot be tested here — it stops a machine this is not
// running on. What CAN be tested is the decision made before the clocks stop,
// and that is the part where a mistake means a device that does not come back.
//
// So power_check is deliberately pure: it takes the numbers rather than reading
// the hardware, and everything about whether a request is sane lives in it.
#include <stdio.h>

#include "../core/powerpolicy.h"

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

// The two boards, whose minimums differ by two orders of magnitude — which is
// exactly why a package has to ask rather than assume.
#define RP2040_MIN 2000
#define RP2350_MIN 10
#define PINS 30

int main(void) {
    printf("power_test - what a sleep request is allowed to be\n");

    // --- the one that matters -----------------------------------------------
    //
    // No duration and no wake pin is not a sleep, it is a device that never
    // comes back. Nothing else in this file is as important as this line.
    ck(power_check(0, -1, PINS, RP2350_MIN) == POWER_NO_WAKE,
       "a sleep with no duration and no wake pin is refused");
    ck(power_check(0, -1, PINS, RP2040_MIN) == POWER_NO_WAKE,
       "on either board");

    // --- duration -----------------------------------------------------------
    ck(power_check(5000, -1, PINS, RP2350_MIN) == POWER_OK,
       "a plain timed sleep is fine");
    ck(power_check(10, -1, PINS, RP2350_MIN) == POWER_OK,
       "right on the RP2350 minimum");
    ck(power_check(9, -1, PINS, RP2350_MIN) == POWER_TOO_SHORT,
       "one below it is refused");
    ck(power_check(1999, -1, PINS, RP2040_MIN) == POWER_TOO_SHORT,
       "and the RP2040 minimum is two seconds, not ten milliseconds");
    ck(power_check(2000, -1, PINS, RP2040_MIN) == POWER_OK,
       "which the same request satisfies exactly");

    // A request that is fine on one board and refused on the other. This is the
    // whole reason fw_power_min_sleep_ms exists.
    ck(power_check(100, -1, PINS, RP2350_MIN) == POWER_OK &&
       power_check(100, -1, PINS, RP2040_MIN) == POWER_TOO_SHORT,
       "100 ms is fine on RP2350 and refused on RP2040");

    // --- wake pins ----------------------------------------------------------
    ck(power_check(0, 14, PINS, RP2350_MIN) == POWER_OK,
       "no duration is fine when a pin can wake it");
    ck(power_check(5000, 14, PINS, RP2350_MIN) == POWER_OK,
       "a duration and a pin together is fine");
    ck(power_check(0, 29, PINS, RP2350_MIN) == POWER_OK,
       "the last pin on the board is a pin");
    ck(power_check(0, 30, PINS, RP2350_MIN) == POWER_BAD_PIN,
       "one past the end is not");
    ck(power_check(0, 99, PINS, RP2350_MIN) == POWER_BAD_PIN,
       "and nor is a number nowhere near it");

    // A too-short duration is still too short even with a pin, because the
    // timer is what enforces it and the pin does not change that.
    ck(power_check(5, 14, PINS, RP2350_MIN) == POWER_TOO_SHORT,
       "a pin does not excuse a duration below the minimum");

    // Zero duration with a pin must NOT be read as "too short" — zero means
    // "no deadline", which is a different thing from "a very small one".
    ck(power_check(0, 14, PINS, RP2040_MIN) == POWER_OK,
       "zero means no deadline rather than a deadline of zero");

    // --- the messages -------------------------------------------------------
    //
    // Each reason has to say something different, or the refusal tells the user
    // nothing they can act on.
    ck(power_check_str(POWER_NO_WAKE) != power_check_str(POWER_TOO_SHORT),
       "each refusal explains itself differently");
    ck(power_check_str(POWER_BAD_PIN) != power_check_str(POWER_TOO_SHORT),
       "including the pin case");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
