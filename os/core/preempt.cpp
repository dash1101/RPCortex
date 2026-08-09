#include "preempt.h"

uint32_t preempt_held_ms(uint32_t now_ms, uint32_t last_yield_ms) {
    // now_ms counts from boot and only goes up, so a timestamp AHEAD of it did
    // not come from this run — it survived a reset. Such a value is not
    // comparable at all, and arithmetic on it produces a confident wrong
    // answer: the signed-difference form used elsewhere still reported three
    // days here, which is the same shape as the bug that made the graded
    // watchdog fire constantly on a perfectly healthy device.
    //
    // Zero means "has not overrun", so the failure is toward leaving a task
    // alone rather than interrupting one wrongly, and it corrects itself the
    // moment that task next yields.
    //
    // A genuine 49-day wrap lands here too, and costs one skipped tick.
    if (last_yield_ms > now_ms) return 0;
    return now_ms - last_yield_ms;
}

PreemptAction preempt_decide(const PreemptState &s) {
    if (!s.enabled) return PREEMPT_NONE;

    // The idle task holds the core by definition and is not what preemption is
    // for. Taking it off would be pure overhead, forever.
    if (s.is_idle_task) return PREEMPT_NONE;

    uint32_t slice = s.slice_ms ? s.slice_ms : PREEMPT_SLICE_MS;
    if (preempt_held_ms(s.now_ms, s.last_yield_ms) < slice) return PREEMPT_NONE;

    // Nothing else wants the core, so taking it away accomplishes nothing and
    // costs a switch. A runaway task with no peers is the watchdog's problem,
    // not this one's.
    if (!s.runnable_peer) return PREEMPT_NONE;

    // Holding a lock, or mid-flash-operation. Interrupting here is how a
    // half-erased sector and a corrupted filesystem happen — the failure that
    // cost boards already. Defer: the timer will ask again, and the critical
    // section is bounded.
    if (s.in_critical) return PREEMPT_DEFER;

    return PREEMPT_SWITCH;
}

StallAction stall_decide(const StallFacts &f) {
    if (!f.past_kill) return STALL_LEAVE;

    // LOSING THE CALL BEATS LOSING THE TASK, so it is tried first even for a
    // task that could perfectly well be ended. The task most likely to be
    // holding a wedged package command is the shell, and the difference between
    // the two answers there is one lost command against a lost session.
    if (f.in_package && f.call_reclaimable) return STALL_ABANDON_CALL;

    if (f.may_end_task) return STALL_END_TASK;

    // Neither. Note that this is reached for an ordinary wedged shell built-in
    // as well, not only for a package — the shell is exempt from being ended
    // whatever it is running. What differs is what can be said about it, and
    // in_package is carried through the answer so the report can say which.
    return STALL_NO_RECOURSE;
}
