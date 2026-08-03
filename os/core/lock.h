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

bool lock_held_by_me(const RpcLock *l);

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
void lock_hw_enter(void);
void lock_hw_exit(void);
}

#endif  // RPC_LOCK_H
