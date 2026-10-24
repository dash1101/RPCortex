// Idle auto-logout arithmetic: the setting parser and the expiry test.
//
// The two ways this goes wrong are a setting that is misread into logging
// people out when it should be off, and the 32-bit millisecond counter wrapping
// so that "a long time ago" reads as "just now" (or the reverse).

#include "idle.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

int main(void) {
    // --- the setting ---------------------------------------------------------
    ck(idle_timeout_ms("5") == 300000u, "5 means five minutes");
    ck(idle_timeout_ms("1") == 60000u, "1 means one minute");
    ck(idle_timeout_ms("30") == 1800000u, "multi-digit values parse");
    ck(idle_timeout_ms("0") == 0, "0 is off");
    ck(idle_timeout_ms("") == 0 && idle_timeout_ms(nullptr) == 0, "empty and missing are off");
    ck(idle_timeout_ms("-5") == 0, "a negative value is off, not a huge unsigned one");
    ck(idle_timeout_ms("5m") == 0, "trailing letters are off");
    ck(idle_timeout_ms(" 5") == 0 && idle_timeout_ms("5 ") == 0, "stray spaces are off");
    ck(idle_timeout_ms("abc") == 0, "words are off");
    ck(idle_timeout_ms("1.5") == 0, "fractions are off");
    ck(idle_timeout_ms("1440") == 1440u * 60000u, "the maximum is accepted as written");
    ck(idle_timeout_ms("100000") == 1440u * 60000u, "above the maximum is clamped to it");
    ck(idle_timeout_ms("99999999999999999999") == 1440u * 60000u, "a number far past 32 bits cannot overflow");

    // --- expiry --------------------------------------------------------------
    const uint32_t lim = 300000;   // five minutes
    ck(!idle_expired(1000, 1000, lim), "no time has passed");
    ck(!idle_expired(1000, 1000 + lim - 1, lim), "one millisecond early is not expired");
    ck(idle_expired(1000, 1000 + lim, lim), "exactly at the limit is expired");
    ck(idle_expired(1000, 1000 + lim + 5000, lim), "past the limit is expired");
    ck(!idle_expired(0, 999999999u, 0), "a limit of 0 never expires");

    // --- the counter wrapping ------------------------------------------------
    // Last input 10 s before the counter wraps; "now" is 4 minutes 50 s after the
    // wrap, so 5 minutes of real time have passed.
    uint32_t last = 0xFFFFFFFFu - 10000u + 1u;
    ck(idle_expired(last, 290000u, lim), "expiry is still detected across the wrap");
    ck(!idle_expired(last, 289999u, lim), "and one millisecond short is still not");
    ck(!idle_expired(last, 5000u, lim), "a short idle that straddles the wrap is not expired");
    ck(!idle_expired(0xFFFFFFFFu, 0, lim), "one tick across the wrap is not expired");
    // The case a naive "now >= last + limit" gets wrong: last + limit has wrapped
    // to a small number while now has not, so it reads as expired the instant
    // the user stops typing, 240 ms before the counter wraps.
    ck(!idle_expired(0xFFFFFF00u, 0xFFFFFFF0u, lim), "a deadline past the wrap does not fire early");

    printf("  idle: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
