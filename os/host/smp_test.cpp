// The scheduler, on two real cores.
//
// Every other host test runs task_core_count() == 1, with the note that "the
// cross-core guard has nothing to guard". That is true, and it is also why a
// whole class of bug shipped undetected: the device runs two cores, and a
// single-core harness cannot see a task being scheduled onto both at once. The
// symptom was a hard fault with a nonsense PC whenever anything ran in the
// background, and twenty-eight green suites had nothing to say about it.
//
// This one runs the real reschedule() from two POSIX threads against one task
// table, with a real mutex behind lock_hw_enter, and asserts the single
// invariant the scheduler exists to keep:
//
//     no task is ever executing on more than one core at a time.
//
// WHAT IS AND IS NOT MODELLED
//
// The platform seam here does not switch stacks. It does not need to: the bug
// is in the bookkeeping, not the register save, and emulating Cortex-M stack
// switching across pthreads would test ucontext rather than task.cpp. What the
// seam does is what the device does — write the outgoing stack pointer, clear
// the outgoing `live` flag, then hand the core over — and it counts how many
// cores believe they are running each task while doing it. A core thread here
// is therefore a core that runs whatever the scheduler gives it, forever,
// which is exactly what core1_main does on the device.
//
// The one thing this CANNOT catch is a missing barrier. The clear of `live`
// must not become visible before the stack pointer is written, and x86 will
// not reorder those stores where Cortex-M33 will. The DMB in task_switch.S is
// there on the strength of the architecture, not of this test.
#include "task.h"
#include "lock.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <atomic>

static int g_checks, g_fails;
static void ck(bool cond, const char *what) {
    g_checks++;
    if (!cond) { printf("  FAIL: %s\n", what); g_fails++; }
}

// --- the platform seam, on two threads --------------------------------------

#define SLOT_MAX 32

// One slot per spawned context. The address of a slot is the opaque "stack
// pointer" the scheduler stores and hands back, which is what lets the seam map
// a switch back to the task it belongs to.
struct Slot {
    std::atomic<int> on_cpu;      // how many cores think they are running it
};
static Slot  g_slot[SLOT_MAX];
static int   g_slot_n;

// Violations are recorded rather than asserted on the spot: the failure happens
// on whichever core loses the race, and aborting there would lose the count.
static std::atomic<int> g_double_sched;

static int idx_of(const void *p) {
    const Slot *s = (const Slot *)p;
    if (!p || s < g_slot || s >= g_slot + SLOT_MAX) return -1;
    return (int)(s - g_slot);
}

extern "C" {

void *task_ctx_init(void *, TaskEntry) {
    if (g_slot_n >= SLOT_MAX) { fprintf(stderr, "  seam exhausted\n"); abort(); }
    int i = g_slot_n++;
    g_slot[i].on_cpu.store(0);
    return &g_slot[i];
}

// The device's sequence, with the window between the unlock and the stack-
// pointer store widened so two threads reliably overlap it. On hardware that
// window is a handful of instructions; here it has to be big enough that the
// scheduler on the other core gets a chance to look at the table while this
// task's saved context is still stale.
void task_ctx_switch(void **save_sp, void *to_sp, volatile bool *live_out) {
    int to = idx_of(to_sp);

    // Arriving. If anyone else still believes they are running this task, its
    // stack is about to be used by two cores at once.
    if (to >= 0 && g_slot[to].on_cpu.fetch_add(1) != 0)
        g_double_sched++;

    for (int i = 0; i < 8; i++) sched_yield();

    int from = idx_of(*save_sp);
    if (from >= 0) {
        *save_sp = &g_slot[from];        // the store the assembly does
        g_slot[from].on_cpu--;           // ...before advertising, so that a
    }                                    //    cleared flag implies a dropped claim
    if (live_out) *live_out = false;
}

// A real lock: the whole point is that two cores contend for it.
static pthread_mutex_t g_hw = PTHREAD_MUTEX_INITIALIZER;
void lock_hw_init(void) {}
void lock_hw_enter(void) { pthread_mutex_lock(&g_hw); }
void lock_hw_exit(void)  { pthread_mutex_unlock(&g_hw); }

static __thread unsigned t_core;
unsigned lock_hw_core(void)     { return t_core; }
uint32_t task_this_core(void)   { return t_core; }
uint32_t task_core_count(void)  { return 2; }

// A real clock, so sleeping tasks actually come due.
static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
uint32_t task_now_us(void) { return (uint32_t)now_us(); }
uint32_t task_now_ms(void) { return (uint32_t)(now_us() / 1000u); }

void task_watchdog_start(void) {}
void task_watchdog_feed(void)  {}
uint32_t task_main_stack_headroom(void) { return 1024 * 1024; }
uint32_t task_main_stack_size(void)     { return 1024 * 1024; }
void task_stack_overflow(const char *n, uint32_t) {
    fprintf(stderr, "  *** stack overflow reported for '%s' ***\n", n ? n : "?");
    abort();
}

}  // extern "C"

// --- the cores --------------------------------------------------------------

static std::atomic<bool> g_stop;

// Tasks never run their own entry function — the seam does not switch stacks —
// so a core is simply something that keeps asking the scheduler for work. Both
// kinds of yield are exercised: a plain one, which parks the task READY, and a
// sleep, which parks it SLEEPING with a deadline. They open different windows.
static std::atomic<long> g_rounds;

static void run_core(unsigned core) {
    t_core = core;
    unsigned n = core * 7919 + 1;
    while (!g_stop.load()) {
        n = n * 1103515245u + 12345u;
        if ((n >> 16) % 4 == 0) task_sleep_ms(1);
        else                    task_yield();
        g_rounds++;
    }
}

static void *core1_main(void *) { run_core(1); return nullptr; }

// Phase 2: a sleeping task must not keep its core while others come due.
//
// The scheduler only reaches the sleep path when NOTHING else is runnable, so
// phase 1 never touches it — six busy workers mean pick() always finds someone.
// This phase runs ONE core with every task pinned to it and every task asleep,
// which is the only shape where the question arises at all.
//
// This is a SMOKE TEST for that path, not a proof of the handover. It is the
// only place in the suite the sleep path executes at all, so it is what stops a
// change there from going completely unexercised, and the double-schedule
// assertion below applies to it as much as to phase 1.
//
// It is deliberately not a performance assertion. Handing the core over does
// measure better — roughly 101 short sleeps against 53 when the handover is
// removed — but the ratio swings with the task mix, and every threshold tried
// either passed both ways or would be flaky on a loaded machine. A number that
// cannot fail honestly is worse than no number, so the bound below is loose and
// only catches the path breaking outright.
#define LONG_SLEEP_MS 500
#define SHORT_SLEEP_MS 20
static std::atomic<long> g_short_done;

static void run_core_staggered(void) {
    t_core = 0;
    while (!g_stop.load()) {
        // Exactly two tasks: the adopted one sleeps long, the other briefly.
        // Any more and somebody is always due, pick() never returns null, and
        // the sleep path this exists to exercise is never reached at all.
        if (task_self() == 1) {
            task_sleep_ms(LONG_SLEEP_MS);
        } else {
            task_sleep_ms(SHORT_SLEEP_MS);
            g_short_done++;
        }
    }
}

static int spin(void *) { return 0; }        // never reached; see above

int main(void) {
    printf("smp_test — the scheduler on two cores\n");

    t_core = 0;
    task_init("shell");

    // --- phase 2: sleepers must release the core ----------------------------
    //
    // Everything pinned to core 0, so the tasks genuinely contend for one core
    // rather than spreading out and never needing to hand anything over.
    task_spawn("sleeper", "(test)", spin, nullptr, TASK_STACK_MIN, AFFINITY_CORE0);

    pthread_t stopper2;
    pthread_create(&stopper2, nullptr, [](void *) -> void * {
        struct timespec ts = { 2, 0 };
        nanosleep(&ts, nullptr);
        g_stop.store(true);
        return nullptr;
    }, nullptr);
    run_core_staggered();
    pthread_join(stopper2, nullptr);

    long sd = g_short_done.load();
    printf("  %ld short sleeps completed while a long one was pending\n", sd);
    // Loose on purpose; see the note above the phase. This catches the sleep
    // path failing to complete sleeps at all, not the handover specifically.
    ck(sd > 20, "sleeps complete while another task holds a long deadline");
    ck(g_double_sched.load() == 0, "and still nothing is double-scheduled");


    g_stop.store(false);      // phase 2 left it set

    // AFFINITY_ANY throughout: a task that cannot migrate cannot be scheduled
    // onto two of them, so pinned tasks would make this test vacuous. On the
    // device these are the package, job and network tasks.
    int spawned = 0;
    for (int i = 0; i < 6; i++)
        if (task_spawn("worker", "(test)", spin, nullptr,
                       TASK_STACK_MIN, AFFINITY_ANY) > 0) spawned++;
    ck(spawned == 6, "six migratable tasks spawned");

    pthread_t c1;
    ck(pthread_create(&c1, nullptr, core1_main, nullptr) == 0, "second core started");

    // Core 0 runs the clock as well as its share of the work. Long enough that
    // a window of a few instructions, sampled by the other core continuously,
    // is hit many times over rather than once if lucky.
    pthread_t stopper;
    pthread_create(&stopper, nullptr, [](void *) -> void * {
        struct timespec ts = { 3, 0 };
        nanosleep(&ts, nullptr);
        g_stop.store(true);
        return nullptr;
    }, nullptr);

    run_core(0);
    pthread_join(c1, nullptr);
    pthread_join(stopper, nullptr);

    long rounds = g_rounds.load();
    printf("  %ld scheduling rounds across two cores\n", rounds);
    ck(rounds > 10000, "the cores actually got work done");

    int dbl = g_double_sched.load();
    if (dbl) printf("  %d task(s) scheduled onto two cores at once\n", dbl);
    ck(dbl == 0, "no task ever ran on two cores at once");

    // Every worker got scheduled, repeatedly. Without this the test could pass
    // by never exercising anything — six tasks nobody ever picked up cannot be
    // picked up twice either.
    uint32_t busy = 0;
    for (uint32_t i = 0; i < task_count(); i++) {
        const TaskInfo *ti = task_at(i);
        if (ti && ti->switches > 100) busy++;
    }
    ck(busy >= 6, "every task was scheduled many times over");

    printf("\n  %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
