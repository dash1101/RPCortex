// Who is allowed inside cyw43 and lwIP, and from where.
//
// One rule, written down once, because it has been broken three times in three
// different files and each break looked like something else entirely.
//
//   EVERY entry into cyw43 or lwIP happens while holding the network operation
//   lock, from the core that brought the radio up.
//
// Both halves are load-bearing, and the reasons are different.
//
// --- the core half ------------------------------------------------------------
//
// cyw43_arch_lwip_begin() is async_context_acquire_lock_blocking(), which for
// the threadsafe_background context is recursive_mutex_enter_blocking(). The
// SDK keys that mutex on lock_get_caller_owner_id(), and
// pico/lock_core.h defines that as get_core_num() — the CORE, not the task. A
// caller that finds the other core holding it waits in
// lock_internal_spin_unlock_with_wait, which is spin_unlock() then __wfe():
// no yield, no timeout, no way out.
//
// So a task on the wrong core does not get an error. It stops. And if the core
// it stopped is core 0, the scheduler stops with it — which is where the
// watchdog is fed from, and preempt_decide() returns PREEMPT_DEFER for as long
// as a lock is held, so the graded stall killer never fires either. Sixteen
// seconds later the board resets with no fault, no crash record and an empty
// log. That was #98, and it took a third of all cold boots.
//
// --- the lock half ------------------------------------------------------------
//
// The same core-keyed mutex has a second failure, in the opposite direction. A
// caller that already owns it — meaning, a caller on the same CORE — is let
// straight through and the recursion count goes up. Two TASKS on one core are
// the same owner as far as that mutex is concerned, so it excludes neither
// from the other.
//
// Cooperative scheduling made that survivable by accident: none of these
// scopes yields, so a task inside one used to run to the end of it. Preemption
// removed the accident. What restores it is g_net_op — an RpcLock, so
// lock_try() calls crit_enter(), so preempt_decide() sees in_critical and
// defers. A task inside lwIP under the operation lock cannot be taken off the
// core, and no other task can be inside at the same time because the operation
// lock is per TASK rather than per core.
//
// That is why net.cpp never hung and nettcp.cpp could: every NetLock in net.cpp
// is taken under g_net_op, and the LwipLocks in nettcp.cpp and netapps.cpp
// were not taken under anything.
#ifndef RPC_NETOWN_H
#define RPC_NETOWN_H

#include <stdint.h>

// Take the network for one operation, and put the caller on the radio's core.
// Recursive: nesting is counted, and the affinity a task started with is given
// back only when the outermost release happens.
void net_op_acquire(void);
void net_op_release(void);

// Whether the caller is on the core the radio was brought up on. False means
// the migration in net_op_acquire could not be done, and the ONLY correct
// answer then is to refuse the call — going ahead is the wedge above.
bool net_core_ok(void);

// Is the operation lock held by this task right now? For the tripwire below.
bool net_op_held(void);

// Someone entered cyw43 or lwIP without holding the operation lock. Counted
// rather than fatal: this is a latent wedge, not a present one, and a board
// that panics on it is worse than a board that reports it. `wifi` prints the
// count when it is not zero.
void     net_unowned_note(void);
uint32_t net_unowned_count(void);

// The whole rule as one object. Declare it first in any function that will
// reach the radio, check `ok`, and let the scope give it back.
//
//     NetOwn own;
//     if (!own.ok) return -1;
//
// Deliberately NOT a plain LockGuard on g_net_op: the migration and the check
// are the point, and a guard that could be used without them would be a guard
// that gets used without them.
struct NetOwn {
    bool ok;
    NetOwn()  { net_op_acquire(); ok = net_core_ok(); }
    ~NetOwn() { net_op_release(); }
    NetOwn(const NetOwn &) = delete;
    NetOwn &operator=(const NetOwn &) = delete;
};

#endif  // RPC_NETOWN_H
