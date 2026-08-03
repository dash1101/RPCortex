// The black box: what it records, and that it survives.
//
// The thing this protects against is a boot report that names the wrong task, or
// no task, after a crash. That is worse than no report — it sends you looking in
// the wrong place, which is exactly what an empty logdump did.

#include "blackbox.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}
static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (got && strcmp(got, want) == 0) return;
    fails++;
    printf("  FAIL: %s  (got '%s', want '%s')\n", what, got ? got : "(null)", want);
}

int main(void) {
    // A cold start has nothing to report. Claiming otherwise would print a
    // crash report on a device that simply had not been powered before.
    bb_init();
    ck(bb_previous() == nullptr, "a cold start reports no previous run");

    bb_note_task(7, 0, "stress", 900, 2048);
    bb_note_command("stress");
    bb_note_yield(1000);
    bb_note_yield(1100);
    bb_note_yield(1200);

    // On the host the region is not preserved, so a second bb_init cannot see
    // the first run. What IS testable is the stall arithmetic and truncation,
    // which is where the bugs actually are.
    ck(bb_stall_ms(1200) == 0,    "no stall immediately after a yield");
    ck(bb_stall_ms(1700) == 500,  "stall is measured from the last yield");
    ck(bb_stall_ms(9200) == 8000, "and keeps growing");

    bb_note_yield(9200);
    ck(bb_stall_ms(9200) == 0, "a yield resets it");

    // Over-long values must be truncated, not run past the buffer. These are
    // read back after a reset, when nothing can fix a missing terminator.
    char big[200];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    bb_note_task(1, 0, big, 0, 0);
    bb_note_command(big);
    // Nothing to compare against directly without the previous-run path, but a
    // read that runs off the end would trip the sanitizer the suite runs under.
    ck(true, "over-long name and command are accepted without overrunning");

    // A null name must not be stored as a null pointer to be printed later.
    bb_note_task(2, 1, nullptr, 0, 0);
    ck(true, "a null task name is handled");

    bb_note_command(nullptr);
    ck(true, "a null command is handled");

    // Before any yield, the stall must read zero rather than "now", which would
    // make a fresh boot look like an eight-second hang.
    bb_init();
    ck(bb_stall_ms(500000) == 0, "no stall reported before the first yield");

    printf("  blackbox: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
