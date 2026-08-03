#include "lock.h"
#include "task.h"

// A task id that is not any real pid, used when the scheduler is not up yet.
// Boot runs single-threaded before task_init, and code that takes locks runs
// then too — treating that as "some task holds it" keeps the recursion counting
// honest instead of special-casing every call site.
#define PRE_INIT_OWNER (-2)

static int me(void) {
    int pid = task_self();
    return pid > 0 ? pid : PRE_INIT_OWNER;
}

bool lock_held_by_me(const RpcLock *l) {
    return l && l->owner == me();
}

bool lock_try(RpcLock *l) {
    if (!l) return false;
    int self = me();
    lock_hw_enter();
    bool got = false;
    if (l->owner == 0) { l->owner = self; l->depth = 1; got = true; }
    else if (l->owner == self) { l->depth++; got = true; }
    lock_hw_exit();
    return got;
}

void lock_acquire(RpcLock *l) {
    if (!l) return;
    while (!lock_try(l)) {
        l->waits++;
        // Yield rather than spin. On one core with cooperative tasks a spin is
        // a permanent deadlock: the task holding the lock cannot run to release
        // it while this one refuses to give the core back. On two cores a spin
        // would merely be wasteful, but the same call is correct for both.
        task_yield();
    }
}

void lock_release(RpcLock *l) {
    if (!l) return;
    lock_hw_enter();
    // Releasing a lock this task does not hold is a bug in the caller, not
    // something to paper over — but clearing another task's ownership would turn
    // it into corruption somewhere else entirely, so it is ignored.
    if (l->owner == me() && l->depth > 0) {
        if (--l->depth == 0) l->owner = 0;
    }
    lock_hw_exit();
}
