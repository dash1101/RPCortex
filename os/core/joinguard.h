// Whether the boot-time join should run again, after the last one stopped the
// device.
//
// The safety here is worth stating exactly, because the version before it was
// wider than its reason. A join that hangs must not turn into a device that
// hangs every time it is switched on — that is the whole of it. It is NOT "a
// join that once went wrong means no more wireless", which is what latching the
// radio off after a single stall amounted to. From the other side of the screen
// that reads as the WiFi having randomly stopped working, fixed only by knowing
// to type a command nobody has been told about, and it happened after ONE
// transient stall on ONE boot.
//
// So: once is a transient. Try again, say plainly that it is a retry, and in
// almost every case join and be done. Twice running is a pattern, and that is
// the case the guard was built for — stand back then, and only then.
//
// The counting is separated from the mechanism for the same reason preempt.h is:
// the arithmetic is where the mistake would be quiet. An off-by-one here either
// disables the safety completely or brings back the behaviour it replaced, and
// neither announces itself.
#ifndef RPC_JOINGUARD_H
#define RPC_JOINGUARD_H

#include <stdint.h>
#include <stdbool.h>

// Two in a row. One is weather; two is a device that cannot do this.
#define JOIN_STRIKES_MAX 2

enum JoinGuardAction {
    JOIN_GO = 0,       // nothing wrong last time: join, and say nothing about it
    JOIN_RETRY,        // last boot stopped mid-join: try once more, and say so
    JOIN_STAND_BACK,   // it has now done that twice running: leave the radio alone
};

// What the strike count becomes at the start of this boot.
//
// It only counts CONSECUTIVE failures, so it is carried in memory a reset does
// not clear and reset by a join that finishes. A run that did not attempt a
// join at all leaves it exactly as it was: standing back is not evidence that
// the problem went away, and treating it as such is what would turn the guard
// into a two-boots-on, one-boot-off oscillation.
uint32_t join_guard_strikes(uint32_t stored, bool prev_stopped_in_join);

JoinGuardAction join_guard_decide(uint32_t strikes);

#endif  // RPC_JOINGUARD_H
