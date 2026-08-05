// Locks.
//
// A lock means "only one task at a time inside here". The filesystem is the
// reason it exists: littlefs keeps bookkeeping in RAM while a file is open, and
// two tasks writing at once interleave those updates and corrupt it. Same story
// for the registry and the command table, just less catastrophic.
//
// Two things make this different from a normal mutex:
//
//   * A waiter YIELDS instead of spinning. Spinning on one core with
//     cooperative tasks is a deadlock, not a delay — the holder can never run
//     to release it, because the waiter never gives the core back.
//   * It is RECURSIVE. storage_copy holds the filesystem lock and calls things
//     that take it again; a non-recursive lock would deadlock against itself.
//     Depth is counted so only the outermost release actually frees it.
//
// The test-and-set has to be atomic against the OTHER CORE, not just against
// other tasks — two cores can genuinely execute the check at the same instant.
// That is what lock_hw_enter/exit are for; on a single-core build they cost
// nothing.
#ifndef RPC_LOCK_H
#define RPC_LOCK_H

#include <stdint.h>

struct RpcLock {
    int      owner;      // pid holding it, or 0
    uint32_t depth;      // recursion count
    uint32_t waits;      // how often someone had to wait — for diagnosing contention
};

// Take the lock, yielding until it is free. Recursive for the same task.
void lock_acquire(RpcLock *l);

// Release one level. Only the outermost release frees it.
void lock_release(RpcLock *l);

// Take it only if free. Returns false rather than waiting — for a caller that
// has something better to do, or must not block.
bool lock_try(RpcLock *l);

// True when this task holds the lock and is about to give it up entirely — the
// outermost of however many nested acquires. For a caller that has to undo
// something once, not once per level.
bool lock_held_once(const RpcLock *l);

bool lock_held_by_me(const RpcLock *l);

// --- critical sections ------------------------------------------------------
//
// "Is it safe to take the core away from whatever is running right now?"
//
// Holding a lock is one reason it is not: the forced-exit path below terminates
// a task where it stands, and a task killed mid-lock leaves that lock owned by
// a pid that no longer exists — every later acquire waits forever. Flash
// operations are the other: an erase interrupted halfway leaves a corrupt
// sector, which has already cost boards once.
//
// Counted rather than boolean, because these nest: a filesystem operation built
// from other filesystem operations takes the same recursive lock several times.
// Per core, since the two cores are in different places at any moment.
void crit_enter(void);
void crit_leave(void);
bool crit_active(void);

// Scoped form, so an early return cannot leave the count raised.
struct CritGuard {
    CritGuard()  { crit_enter(); }
    ~CritGuard() { crit_leave(); }
};

// Scope guard, so a lock cannot be leaked by an early return. Anything with
// more than one exit path should use this rather than paired calls.
struct LockGuard {
    RpcLock *l;
    explicit LockGuard(RpcLock *lock) : l(lock) { lock_acquire(l); }
    ~LockGuard() { lock_release(l); }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;
};

// --- the platform seam ------------------------------------------------------
//
// A brief, non-yielding critical section that is atomic across cores. On the
// device this is a hardware spinlock; on the host, nothing. It is held for a
// few instructions only — never across a yield.
extern "C" {
// Claim the lock up front, before a second core exists. Claiming it lazily on
// first use let two cores reach the check together and take two different
// locks, each excluding nobody. Safe to call more than once; a no-op on the
// host, which has no second core to exclude.
void lock_hw_init(void);
void lock_hw_enter(void);
// Which core is asking. Same per-platform split as the two above: the hardware
// answer on a device, always 0 on the host.
unsigned lock_hw_core(void);
void lock_hw_exit(void);

// Interrupts off across a context switch, and on again once the incoming task
// owns the hardware.
//
// Not a performance guard — a correctness one. Between the instruction that
// changes the stack pointer and the one that reprograms the protection unit,
// the processor is running on one task's stack with another task's regions.
// An interrupt arriving in that window pushes its frame onto memory the
// protection unit does not cover, and faults: MSTKERR, on a stack with
// kilobytes free, at an address that IS inside the region the task was given.
//
// Symmetric by construction. Every context masks before it switches out and
// unmasks after it has armed itself on the way back in, so the state carries
// across the switch without being saved anywhere.
// Returns what the state WAS, so it can be put back rather than forced.
//
// Forcing interrupts on after the switch would be wrong for any caller that
// had them off for its own reasons, and each context restores its own saved
// value from its own stack frame — which is what makes this work across a
// switch without storing anything globally.
unsigned task_irq_save(void);
void     task_irq_restore(unsigned state);
}

#endif  // RPC_LOCK_H
