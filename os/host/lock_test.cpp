// Locks: recursion, hand-off, and the property that makes them safe on one core.
//
// The failure mode here is a deadlock, not a wrong answer — and a deadlock on a
// device with no debugger looks exactly like a crash. The two cases that would
// cause one are checked directly: taking a lock twice from the same task, and
// waiting for a lock the running task cannot release without being scheduled.

#include "lock.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ucontext.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}
static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want) == 0) return;
    fails++;
    printf("  FAIL: %s  (got '%s', want '%s')\n", what, got, want);
}

// --- the platform seam ------------------------------------------------------

#define CTX_MAX 8
static ucontext_t g_ctx[CTX_MAX];
static int        g_ctx_n;
static TaskEntry  g_entry[CTX_MAX];
static void ctx_tramp(int i) { g_entry[i](); }

extern "C" {
void *task_ctx_init(void *stack_top, TaskEntry entry) {
    int i = g_ctx_n++;
    g_entry[i] = entry;
    getcontext(&g_ctx[i]);
    g_ctx[i].uc_stack.ss_sp   = (char *)stack_top - 16384;
    g_ctx[i].uc_stack.ss_size = 16384;
    g_ctx[i].uc_link          = nullptr;
    makecontext(&g_ctx[i], (void (*)())ctx_tramp, 1, i);
    return &g_ctx[i];
}
void task_ctx_switch(void **save_sp, void *to_sp) {
    static ucontext_t root;
    ucontext_t *from = *save_sp ? (ucontext_t *)*save_sp : &root;
    *save_sp = from;
    swapcontext(from, (ucontext_t *)to_sp);
}
void lock_hw_enter(void) {}
void lock_hw_exit(void)  {}
uint32_t task_now_ms(void)     { return 0; }
uint32_t task_core_count(void) { return 1; }
uint32_t task_this_core(void)  { return 0; }

// The scheduler's new safety hooks. On the host there is no watchdog and no
// stack to overflow (ucontext gives each task a generous one), but the symbols
// have to exist for anything linking task.cpp.
void task_watchdog_start(void) {}
void task_watchdog_feed(void)  {}
void task_stack_overflow(const char *, uint32_t) {
    fprintf(stderr, "  *** task_stack_overflow called on the host ***\n");
    abort();
}

}

// --- the shared thing being protected ---------------------------------------

static RpcLock g_lock;
static char    g_log[128];
static void logc(const char *s) {
    if (strlen(g_log) + strlen(s) + 1 < sizeof(g_log)) strcat(g_log, s);
}

// A task that holds the lock across a yield. Whoever is waiting must NOT get in
// while it is inside, and must get in once it leaves.
static int holder(void *) {
    lock_acquire(&g_lock);
    logc("[");
    task_yield();          // still holding it
    task_yield();
    logc("]");
    lock_release(&g_lock);
    return 0;
}

static int waiter(void *) {
    lock_acquire(&g_lock);
    logc("w");
    lock_release(&g_lock);
    return 0;
}

// Recursion: an operation built out of other operations takes the same lock
// again. storage_copy does exactly this, and a non-recursive lock deadlocks.
static int nested(void *) {
    lock_acquire(&g_lock);
    lock_acquire(&g_lock);
    lock_acquire(&g_lock);
    logc("n");
    lock_release(&g_lock);
    lock_release(&g_lock);
    ck(g_lock.owner != 0, "still held after inner releases");
    lock_release(&g_lock);
    ck(g_lock.owner == 0, "free only after the outermost release");
    return 0;
}

int main(void) {
    const uint32_t ST = 16384;
    task_init("main");

    // --- basics -------------------------------------------------------------
    ck(g_lock.owner == 0, "a fresh lock is free");
    lock_acquire(&g_lock);
    ck(lock_held_by_me(&g_lock), "the taker holds it");
    ck(g_lock.depth == 1, "depth one");
    lock_acquire(&g_lock);
    ck(g_lock.depth == 2, "recursive take raises the depth");
    lock_release(&g_lock);
    ck(g_lock.owner != 0, "one release is not enough");
    lock_release(&g_lock);
    ck(g_lock.owner == 0, "the outermost release frees it");
    ck(!lock_held_by_me(&g_lock), "and nobody holds it");

    // lock_try must not wait.
    ck(lock_try(&g_lock), "try succeeds on a free lock");
    ck(lock_try(&g_lock), "try succeeds again for the same task (recursive)");
    lock_release(&g_lock);
    lock_release(&g_lock);
    ck(g_lock.owner == 0, "released both");

    // Releasing a lock this task does not hold must not steal it.
    lock_acquire(&g_lock);
    int saved_owner = g_lock.owner;
    g_lock.owner = 999;                        // pretend another task holds it
    lock_release(&g_lock);
    ck(g_lock.owner == 999, "releasing someone else's lock does nothing");
    g_lock.owner = saved_owner;
    lock_release(&g_lock);
    ck(g_lock.owner == 0, "cleaned up");

    // --- recursion inside a task -------------------------------------------
    g_log[0] = 0;
    int n = task_spawn("nested", nullptr, nested, nullptr, ST, AFFINITY_ANY);
    for (int i = 0; i < 4; i++) task_yield();
    eq(g_log, "n", "a nested acquire does not deadlock");
    task_reap(n);

    // --- hand-off between tasks --------------------------------------------
    //
    // The waiter is spawned second, so it runs while the holder still has the
    // lock. If lock_acquire spun instead of yielding, this test would hang
    // forever rather than fail — which is precisely the bug it exists to catch.
    g_log[0] = 0;
    int h = task_spawn("holder", nullptr, holder, nullptr, ST, AFFINITY_ANY);
    int w = task_spawn("waiter", nullptr, waiter, nullptr, ST, AFFINITY_ANY);
    for (int i = 0; i < 12; i++) task_yield();
    eq(g_log, "[]w", "the waiter gets in only after the holder is finished");
    ck(g_lock.waits > 0, "the wait was counted");
    ck(g_lock.owner == 0, "the lock ends up free");
    task_reap(h); task_reap(w);

    printf("  lock: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
