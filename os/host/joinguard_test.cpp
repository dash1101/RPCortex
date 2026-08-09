// The boot-join guard.
//
// Two failure modes bracket this, and the tests are written as the story of a
// device rather than as a table, because that is the form the mistakes take.
// Too eager and a device that cannot join spends every boot hanging; too shy
// and one transient stall leaves somebody with no network and a log line they
// were never told to read.
#include "joinguard.h"

#include <stdio.h>

static int checks = 0, fails = 0;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { printf("    FAIL %s\n", what); fails++; }
}

int main(void) {
    printf("  joinguard\n");

    // --- the ordinary boot --------------------------------------------------
    ok(join_guard_strikes(0, false) == 0, "a clean boot after a clean boot: nothing counted");
    ok(join_guard_decide(0) == JOIN_GO,   "and it just joins");

    // --- one transient stall, which is the whole point ----------------------
    //
    // This is the case the old behaviour got wrong: it latched the radio off for
    // the entire next boot, and the device sat there offline until a person
    // typed a command. One stall has to end with the device back on the network
    // by itself.
    {
        uint32_t s = join_guard_strikes(0, true);
        ok(s == 1, "one stop in the join counts one strike");
        ok(join_guard_decide(s) == JOIN_RETRY, "one strike retries rather than giving up");
    }

    // --- twice running, which is what the guard is actually for -------------
    {
        uint32_t s = join_guard_strikes(join_guard_strikes(0, true), true);
        ok(s == 2, "two boots in a row count two");
        ok(join_guard_decide(s) == JOIN_STAND_BACK, "and the second one stands back");
    }

    // --- the retry worked ---------------------------------------------------
    //
    // A join that finishes clears the count at the device, so the boot after it
    // sees nothing. Expressed here as the count it is asked about being zero.
    ok(join_guard_decide(join_guard_strikes(0, false)) == JOIN_GO,
       "a join that finished leaves the next boot ordinary");

    // --- standing back is not evidence of recovery --------------------------
    //
    // The boot that stands back does not attempt a join, so it cannot have
    // stopped in one. If that cleared the count, the device would oscillate:
    // hang, hang, stand back, hang, hang, stand back — two watchdog resets out
    // of every three boots, which is the boot loop this exists to prevent
    // wearing a different hat.
    {
        uint32_t s = 2;
        ok(join_guard_strikes(s, false) == 2, "a boot that did not try leaves the count alone");
        ok(join_guard_decide(join_guard_strikes(s, false)) == JOIN_STAND_BACK,
           "so it keeps standing back rather than trying again next boot");
    }

    // --- the counter does not run away --------------------------------------
    ok(join_guard_strikes(JOIN_STRIKES_MAX, true) == JOIN_STRIKES_MAX,
       "the count stops at the limit instead of wrapping");
    ok(join_guard_decide(9999) == JOIN_STAND_BACK, "anything above the limit still stands back");

    // --- the limit is a retry, not a hair trigger ---------------------------
    ok(JOIN_STRIKES_MAX >= 2, "there is at least one retry before giving up");

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
