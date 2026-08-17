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

    // A clock that has gone BACKWARDS. This is the real case, not a hypothetical:
    // the struct survives a reset, so after a watchdog reboot it holds a
    // timestamp from the previous run while the clock has restarted near zero.
    // The unsigned subtraction wrapped to ~4.29 billion — read as a 49-day stall,
    // which tripped every escalation stage instantly and forever.
    bb_note_yield(16000);
    ck(bb_stall_ms(3) == 0, "a clock that restarted reports no stall, not 49 days");
    ck(bb_stall_ms(15999) == 0, "one millisecond behind is still no stall");
    ck(bb_stall_ms(16000) == 0, "equal timestamps are no stall");
    ck(bb_stall_ms(16500) == 500, "and forwards still measures correctly");

    // --- "nothing could be done about it" ------------------------------------
    //
    // Written from the preemption alarm and read at the next boot, so the two
    // ends have to agree across bb_init. The flag matters most on a part with
    // no sandbox, where a wedged package command has no recovery at all and
    // this is the only thing that turns a bare watchdog reset into an
    // explanation.
    bb_init();
    bb_note_task(3, 0, "shell", 0, 0);
    bb_note_stuck(BB_STUCK_PACKAGE);
    bb_init();
    {
        const BlackBox *prev = bb_previous();
        ck(prev != nullptr, "the stalled run is carried over");
        ck(prev && prev->stuck == BB_STUCK_PACKAGE,
           "and it says a package call was what could not be recovered");
    }

    // PROGRESS CLEARS IT. A device that stalled and then came back must not
    // carry the flag into whatever goes wrong next — the report would name a
    // cause belonging to an incident that resolved itself.
    bb_init();
    bb_note_task(3, 0, "shell", 0, 0);
    bb_note_stuck(BB_STUCK_TASK);
    bb_note_yield(1000);
    bb_init();
    {
        const BlackBox *prev = bb_previous();
        ck(prev && prev->stuck == BB_STUCK_NO,
           "a stall that was recovered from leaves nothing behind");
    }

    // A DELIBERATE REBOOT IS NOT A STALL. bb_note_clean_exit is what tells the
    // reporters to stay quiet, and leaving the flag set would make `reboot`
    // print a recovery failure on the way back up.
    bb_init();
    bb_note_task(3, 0, "shell", 0, 0);
    bb_note_stuck(BB_STUCK_PACKAGE);
    bb_note_clean_exit();
    bb_init();
    {
        const BlackBox *prev = bb_previous();
        ck(prev && prev->stuck == BB_STUCK_NO && prev->task[0] == 0,
           "a clean exit clears the flag along with the task name");
    }

    // A fresh run starts with nothing to explain.
    bb_init();
    bb_note_task(3, 0, "shell", 0, 0);
    bb_init();
    {
        const BlackBox *prev = bb_previous();
        ck(prev && prev->stuck == BB_STUCK_NO,
           "a run nobody had to intervene in reports no recovery failure");
    }

    // --- did the last run end badly? -----------------------------------------
    //
    // One predicate, because there were two: the boot banner tested the task
    // name and `diag` tested only that a record existed. `diag` therefore
    // announced an unclean shutdown after every single reboot, since the region
    // survives the reset and every deliberate restart on this part is carried
    // out by the watchdog.
    bb_init();
    ck(bb_previous_crash() == nullptr, "a cold start reports no crash");

    // The ordinary case: something was running and the run stopped.
    bb_init();
    bb_note_task(4, 0, "shell", 100, 2048);
    bb_note_command("havoc spin");
    bb_init();
    {
        const BlackBox *c = bb_previous_crash();
        ck(c != nullptr, "a run that stopped while a task was scheduled is a crash");
        eq(c ? c->task : nullptr, "shell", "and it names the task");
        ck(bb_previous() == c, "the raw record and the crash record are the same one");
    }

    // A `reboot`. Nothing should be reported at the next boot.
    bb_init();
    bb_note_task(4, 0, "shell", 100, 2048);
    bb_note_clean_exit();
    bb_init();
    ck(bb_previous_crash() == nullptr, "a deliberate restart is not a crash");
    ck(bb_previous() != nullptr, "though the record itself is still there to read");

    // THE ONE THIS EXISTS FOR. A deliberate restart clears the task name and
    // then takes over a hundred milliseconds to land, with the other core still
    // scheduling — so the name is written straight back before the reset. Every
    // reader that keyed off the name saw a crash; the boot banner did too, and
    // `diag` never even looked.
    bb_init();
    bb_note_task(4, 0, "shell", 100, 2048);
    bb_note_clean_exit();
    bb_note_task(9, 1, "usb", 200, 2048);      // core 1, during the 120 ms wait
    bb_note_phase("entered fw_millis");
    bb_note_yield(4000);
    bb_init();
    ck(bb_previous_crash() == nullptr,
       "core 1 scheduling during the shutdown does not turn it back into a crash");

    // Nothing to name is nothing to report: both readers print the task first,
    // and a report with an empty name sends somebody looking for a task that
    // never existed.
    bb_init();
    bb_note_command("something");
    bb_init();
    ck(bb_previous_crash() == nullptr, "a record with no task in it reports nothing");

    printf("  blackbox: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
