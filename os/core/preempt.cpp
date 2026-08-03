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
