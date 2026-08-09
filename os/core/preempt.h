// When to take the core away from a task, decided without touching a CPU.
//
// The policy and the mechanism are separated deliberately. The mechanism —
// PendSV, exception frames, the process stack — is ARM and cannot be exercised
// on a host. The policy is arithmetic and a few rules, and it is the part where
// a mistake is quiet: preempting something holding a lock, or never preempting
// the one task that needed it, both look like "it works" right up until they
// do not.
//
// The rule this implements: **cooperative by default, preemption as a safety
// net.** A task that yields is never interrupted, so all the existing reasoning
// about shared state stays true. Only a task that has held the core past its
// slice without yielding gets taken off it — which is to say, only a task that
// is already misbehaving.
//
// That is the whole point. A runaway package becomes a killed task instead of a
// dead device, without turning every other task into one that can be interrupted
// at any instruction.
#ifndef RPC_PREEMPT_H
#define RPC_PREEMPT_H

#include <stdint.h>
#include <stdbool.h>

// How long a task may hold the core without yielding before the timer takes it
// away. Comfortably longer than any legitimate uninterrupted stretch — a flash
// erase-and-write is under 200 ms — and far below the 8 s watchdog, so
// preemption always gets its chance first.
#define PREEMPT_SLICE_MS   250

// Why the core is being taken, which decides what happens next.
enum PreemptAction {
    PREEMPT_NONE = 0,      // leave it alone
    PREEMPT_SWITCH,        // overran its slice: reschedule, keep it runnable
    PREEMPT_DEFER,         // overran, but cannot be interrupted safely just now
};

// Everything the decision depends on. Passed in rather than read from globals so
// the awkward combinations can be constructed directly in a test.
struct PreemptState {
    uint32_t now_ms;
    uint32_t last_yield_ms;   // when the running task last reached the scheduler
    uint32_t slice_ms;        // 0 selects PREEMPT_SLICE_MS
    bool     enabled;         // master switch, so it can be turned off entirely
    bool     in_critical;     // holding a lock, or inside a flash operation
    bool     runnable_peer;   // is there anything else that could run?
    bool     is_idle_task;    // the idle task is never the problem
};

PreemptAction preempt_decide(const PreemptState &s);

// --- what can be done about a task that has stopped yielding entirely --------
//
// preempt_decide answers one question — may this task be taken off the core —
// and there are two answers above it that it does not cover. A package command
// runs on the SHELL task, so ending the task ends the session; the better
// outcome is to end the CALL and leave the task standing. And on a part with no
// sandbox there is no call to end, because a package there branches straight
// into the firmware rather than through a supervisor call that could be
// returned from differently.
//
// That last case is the one worth naming. It has no recovery at all: the shell
// cannot be ended (nothing respawns it, so the device would come back with no
// console), and the call cannot be taken back. The watchdog reboot is the only
// way out — and a reboot that had no alternative should say so rather than look
// like one nobody tried to prevent.
enum StallAction {
    STALL_LEAVE = 0,        // not yet, or it may still come back on its own
    STALL_ABANDON_CALL,     // take the package call back; the task survives
    STALL_END_TASK,         // end the task where it stands
    STALL_NO_RECOURSE,      // neither is possible; the watchdog is the way out
};

// Passed in rather than read from globals, for the same reason as PreemptState:
// the combinations that matter are the awkward ones, and a test has to be able
// to build them directly.
struct StallFacts {
    bool past_kill;          // asking the task nicely has already failed
    bool in_package;         // a package's code is what is running
    bool call_reclaimable;   // and there is a way to take that call back
    bool may_end_task;       // preempt_decide says this task may be ended
};

StallAction stall_decide(const StallFacts &f);

// How long the task has held the core. Signed subtraction, because the
// timestamp can predate a reset and an unsigned wrap reads as 49 days — the bug
// that made the graded watchdog fire constantly.
uint32_t preempt_held_ms(uint32_t now_ms, uint32_t last_yield_ms);

#endif  // RPC_PREEMPT_H
