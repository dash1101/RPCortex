// The preemption policy.
//
// Everything here is a rule about when NOT to take the core, and each one exists
// because taking it at that moment breaks something specific. The tests name the
// something, because a rule whose reason has been forgotten is a rule that gets
// "simplified" away by the next person to read it.
#include "preempt.h"

#include <stdio.h>

static int checks = 0, fails = 0;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { printf("    FAIL %s\n", what); fails++; }
}

// A task that has been running well past its slice, with somewhere to switch to.
static PreemptState overrun(void) {
    PreemptState s{};
    s.now_ms = 10000;
    s.last_yield_ms = 10000 - (PREEMPT_SLICE_MS + 50);
    s.enabled = true;
    s.runnable_peer = true;
    return s;
}

int main(void) {
    printf("  preempt\n");

    // --- elapsed time -------------------------------------------------------
    ok(preempt_held_ms(1000, 900) == 100, "held: ordinary case");
    ok(preempt_held_ms(1000, 1000) == 0,  "held: no time at all");
    // The timestamp survives a reset; comparing against a previous boot must not
    // report 49 days, which is what made the graded watchdog fire constantly.
    ok(preempt_held_ms(100, 4000000000u) == 0, "held: a pre-reset timestamp reads as zero");
    ok(preempt_held_ms(50, 100) == 0,          "held: a future timestamp reads as zero");

    // --- the ordinary decision ---------------------------------------------
    {
        PreemptState s = overrun();
        ok(preempt_decide(s) == PREEMPT_SWITCH, "an overrunning task is preempted");
    }
    {
        PreemptState s = overrun();
        s.last_yield_ms = s.now_ms - 10;       // yielded recently
        ok(preempt_decide(s) == PREEMPT_NONE, "a cooperative task is left alone");
    }
    {
        // Exactly at the boundary counts as overrun, so a slice is a limit
        // rather than something to be exceeded before anything happens.
        PreemptState s = overrun();
        s.last_yield_ms = s.now_ms - PREEMPT_SLICE_MS;
        ok(preempt_decide(s) == PREEMPT_SWITCH, "the slice boundary itself counts");
        s.last_yield_ms = s.now_ms - (PREEMPT_SLICE_MS - 1);
        ok(preempt_decide(s) == PREEMPT_NONE, "one millisecond short does not");
    }

    // --- the rules about when not to ---------------------------------------
    {
        PreemptState s = overrun();
        s.enabled = false;
        ok(preempt_decide(s) == PREEMPT_NONE, "disabled means never");
    }
    {
        // Interrupting a flash erase leaves a half-erased sector and a corrupt
        // filesystem. That failure has already cost boards once.
        PreemptState s = overrun();
        s.in_critical = true;
        ok(preempt_decide(s) == PREEMPT_DEFER, "a critical section defers, not preempts");
    }
    {
        PreemptState s = overrun();
        s.runnable_peer = false;
        ok(preempt_decide(s) == PREEMPT_NONE,
           "with nobody to switch to, preempting costs a switch and gains nothing");
    }
    {
        PreemptState s = overrun();
        s.is_idle_task = true;
        ok(preempt_decide(s) == PREEMPT_NONE, "the idle task is never preempted");
    }
    {
        // Order matters: an idle task in a critical section must still be NONE,
        // not DEFER, or the timer would keep re-asking about it forever.
        PreemptState s = overrun();
        s.is_idle_task = true;
        s.in_critical = true;
        ok(preempt_decide(s) == PREEMPT_NONE, "idle wins over critical");
    }
    {
        // A custom slice is honoured, so a tuning pass can shorten it.
        PreemptState s = overrun();
        s.slice_ms = 50;
        s.last_yield_ms = s.now_ms - 60;
        ok(preempt_decide(s) == PREEMPT_SWITCH, "a shorter custom slice fires sooner");
        s.slice_ms = 1000;
        ok(preempt_decide(s) == PREEMPT_NONE, "a longer custom slice holds off");
    }
    {
        // Zero selects the default rather than meaning "preempt immediately",
        // which a zero-initialised struct would otherwise do.
        PreemptState s{};
        s.enabled = true;
        s.runnable_peer = true;
        s.now_ms = PREEMPT_SLICE_MS - 1;
        s.slice_ms = 0;
        ok(preempt_decide(s) == PREEMPT_NONE, "slice 0 means the default, not zero");
    }
    {
        // The slice must be usable well inside the watchdog, or a runaway task
        // reboots the device before preemption ever gets a chance at it — which
        // would make the whole mechanism decorative.
        ok(PREEMPT_SLICE_MS * 4 < 8000, "the slice fires well before the watchdog");
    }

    // --- what can be done about a task that has stopped yielding ------------
    //
    // The RP2040 row of this table is the whole reason it exists: there, a
    // package command wedged on the shell task has no answer at all, and the
    // fall-through it used to take through preempt_tick made that outcome look
    // like an oversight rather than a decision.
    {
        StallFacts f{};
        ok(stall_decide(f) == STALL_LEAVE,
           "nothing is done before the stall passes the threshold");
        f.in_package = f.call_reclaimable = f.may_end_task = true;
        ok(stall_decide(f) == STALL_LEAVE,
           "and that holds however recoverable the task is");
    }
    {
        // The sandboxed part. Losing the call is the good outcome and it is
        // taken even when the task could have been ended instead.
        StallFacts f{};
        f.past_kill = f.in_package = f.call_reclaimable = true;
        ok(stall_decide(f) == STALL_ABANDON_CALL,
           "a reclaimable package call is taken back");
        f.may_end_task = true;
        ok(stall_decide(f) == STALL_ABANDON_CALL,
           "and taking the call back beats ending the task that holds it");
    }
    {
        // A wedged task that is not a package, or is one that cannot be
        // unwound: the existing forced exit.
        StallFacts f{};
        f.past_kill = f.may_end_task = true;
        ok(stall_decide(f) == STALL_END_TASK, "an ordinary wedged task is ended");
        f.in_package = true;
        ok(stall_decide(f) == STALL_END_TASK,
           "so is a package's own task, when the call cannot be reclaimed");
    }
    {
        // ARMv6-M: a package command on the shell task. in_package is true —
        // the OS knows perfectly well whose code it is — but there is no call
        // to reclaim and the shell may not be ended, because nothing would
        // respawn it. The answer is that there is no answer, and it is named.
        StallFacts f{};
        f.past_kill = true;
        f.in_package = true;
        f.call_reclaimable = false;
        f.may_end_task = false;
        ok(stall_decide(f) == STALL_NO_RECOURSE,
           "a wedged package on a part with no sandbox has no recovery");
    }
    {
        // The same for a wedged built-in on the shell. Also no recourse, and
        // the caller tells the two apart by in_package rather than by this.
        StallFacts f{};
        f.past_kill = true;
        ok(stall_decide(f) == STALL_NO_RECOURSE,
           "so does a wedged shell command that is not a package");
    }
    {
        // NO_RECOURSE MUST NOT BE REACHABLE WHILE SOMETHING IS STILL POSSIBLE.
        // Exhaustive over the three booleans that follow past_kill, because the
        // failure this guards against is a future edit reordering the rules so
        // that a recoverable case falls through to "reboot the device".
        for (int i = 0; i < 8; i++) {
            StallFacts f{};
            f.past_kill        = true;
            f.in_package       = (i & 1) != 0;
            f.call_reclaimable = (i & 2) != 0;
            f.may_end_task     = (i & 4) != 0;
            bool recoverable = (f.in_package && f.call_reclaimable) || f.may_end_task;
            ok((stall_decide(f) == STALL_NO_RECOURSE) == !recoverable,
               "no recourse is reported only when there genuinely is none");
        }
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
