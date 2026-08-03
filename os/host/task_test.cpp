// The scheduler, driven for real.
//
// The platform seam (task_ctx_init / task_ctx_switch) is implemented here with
// ucontext, which does exactly what the Cortex-M switch does: give a function
// its own stack and swap between them. So this is not a test of the bookkeeping
// with switching stubbed out — tasks genuinely run, yield, sleep, and finish on
// their own stacks, and the round-robin order below is the order they really ran
// in.
//
// That matters because the failure mode of a scheduler is not a wrong number, it
// is a task that never runs again or a stack that quietly overflows, and neither
// shows up in a table you only inspect.

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

// --- the platform seam, on ucontext -----------------------------------------

// One ucontext per spawn, never reused — the real device allocates a fresh
// stack each time too. It has to be big enough for every task the test starts
// across the whole run, and overrunning it is a global-buffer-overflow rather
// than anything to do with the scheduler. ASan found exactly that when the
// burst loop was added.
#define CTX_MAX 64
static ucontext_t g_ctx[CTX_MAX];
static int        g_ctx_n;
static TaskEntry  g_entry[CTX_MAX];

static void ctx_trampoline(int idx) { g_entry[idx](); }

void *task_ctx_init(void *stack_top, TaskEntry entry) {
    if (g_ctx_n >= CTX_MAX) {
        fprintf(stderr, "  test seam exhausted: raise CTX_MAX\n");
        abort();
    }
    int idx = g_ctx_n++;
    g_entry[idx] = entry;
    getcontext(&g_ctx[idx]);
    // The scheduler hands us the TOP of the block; ucontext wants the base and
    // a size, so step back over the region task.cpp allocated.
    g_ctx[idx].uc_stack.ss_sp   = (char *)stack_top - 8192;
    g_ctx[idx].uc_stack.ss_size = 8192;
    g_ctx[idx].uc_link          = nullptr;
    makecontext(&g_ctx[idx], (void (*)())ctx_trampoline, 1, idx);
    return &g_ctx[idx];
}

void task_ctx_switch(void **save_sp, void *to_sp) {
    static ucontext_t root;
    ucontext_t *from = *save_sp ? (ucontext_t *)*save_sp : &root;
    *save_sp = from;
    swapcontext(from, (ucontext_t *)to_sp);
}

// Single-threaded host: the cross-core guard has nothing to guard. extern "C"
// because lock.h declares them that way — the device side is paired with
// assembly, and a mangled definition here would simply not be found.
extern "C" void lock_hw_enter(void) {}
extern "C" void lock_hw_exit(void)  {}

static uint32_t g_now;
uint32_t task_now_ms(void)    { return g_now; }
// Microseconds, for the rate limit inside task_alive. Driven from the same
// fake clock so a test that advances time advances both.
uint32_t task_now_us(void)   { return g_now * 1000; }
uint32_t task_core_count(void){ return 1; }
uint32_t task_this_core(void) { return 0; }

// The scheduler's new safety hooks. On the host there is no watchdog and no
// stack to overflow (ucontext gives each task a generous one), but the symbols
// have to exist for anything linking task.cpp.
void task_watchdog_start(void) {}
void task_watchdog_feed(void)  {}
uint32_t task_main_stack_headroom(void) { return 1024 * 1024; }
uint32_t task_main_stack_size(void)     { return 1024 * 1024; }
void task_stack_overflow(const char *, uint32_t) {
    fprintf(stderr, "  *** task_stack_overflow called on the host ***\n");
    abort();
}


// --- what the tasks do ------------------------------------------------------

static char g_log[256];
static void logc(const char *s) {
    if (strlen(g_log) + strlen(s) + 1 < sizeof(g_log)) strcat(g_log, s);
}

static int task_ab(void *arg) {
    const char *tag = (const char *)arg;
    for (int i = 0; i < 3; i++) { logc(tag); task_yield(); }
    return 7;
}

static int task_quick(void *) { logc("q"); return 0; }

static int task_sleeper(void *) {
    logc("s");
    task_sleep_ms(50);
    logc("S");
    return 0;
}

static int task_forever(void *) {
    // A well-behaved long-running task: checks whether it has been asked to
    // stop, which is the only way a cooperative kill can work.
    while (!task_should_stop()) { logc("."); task_yield(); }
    logc("k");
    return 42;
}

int main(void) {
    // Big stacks: ucontext needs far more than a Cortex-M does, and task_spawn
    // hands task_ctx_init the top of whatever it allocated.
    const uint32_t ST = 8192;

    task_init("shell");
    ck(task_self() == 1, "the adopted context is pid 1");
    ck(task_count() == 1, "one task after init");
    const TaskInfo *me = task_current();
    ck(me && strcmp(me->name, "shell") == 0, "pid 1 keeps its name");
    ck(me && me->state == TASK_RUNNING, "pid 1 is running");

    // --- round robin --------------------------------------------------------
    g_log[0] = 0;
    int a = task_spawn("a", "/bin/a", task_ab, (void *)"a", ST, AFFINITY_ANY);
    int b = task_spawn("b", "/bin/b", task_ab, (void *)"b", ST, AFFINITY_ANY);
    ck(a > 0 && b > 0 && a != b, "spawn returns distinct pids");
    ck(task_count() == 3, "three tasks after two spawns");

    // Yield repeatedly from pid 1; a and b should interleave, not run to
    // completion one after the other.
    for (int i = 0; i < 10; i++) task_yield();
    eq(g_log, "ababab", "two tasks interleave round-robin");

    ck(task_find(a) && task_find(a)->state == TASK_DONE, "a finished");
    ck(task_find(a)->exit_code == 7, "a's exit code is kept");
    ck(task_find(b)->exit_code == 7, "b's exit code is kept");

    // --- accounting ---------------------------------------------------------
    const TaskInfo *ai = task_find(a);
    ck(ai->switches >= 3, "switch count is recorded");
    ck(ai->stack_used > 0, "stack high-water is measured");
    ck(ai->stack_used < ai->stack_size, "and is under the allocation");
    eq(ai->path, "/bin/a", "the path is kept for the task manager");

    // --- reaping ------------------------------------------------------------
    ck(task_reap(a), "a DONE task can be reaped");
    ck(!task_reap(a), "reaping twice does nothing");
    ck(task_find(a) == nullptr, "a reaped task is gone");
    ck(task_reap(b), "b reaped too");
    ck(task_count() == 1, "back to just pid 1");

    // --- a task that returns immediately ------------------------------------
    g_log[0] = 0;
    int q = task_spawn("q", nullptr, task_quick, nullptr, ST, AFFINITY_ANY);
    task_yield();
    eq(g_log, "q", "a short task runs to completion");
    ck(task_find(q)->state == TASK_DONE, "and is marked done");
    task_reap(q);

    // --- sleeping -----------------------------------------------------------
    g_log[0] = 0;
    g_now = 1000;
    int s = task_spawn("s", nullptr, task_sleeper, nullptr, ST, AFFINITY_ANY);
    task_yield();
    eq(g_log, "s", "the sleeper ran up to its sleep");
    ck(task_find(s)->state == TASK_SLEEPING, "and is parked as SLEEPING");

    // Not yet due: it must not be picked.
    g_now = 1010;
    task_yield();
    eq(g_log, "s", "a sleeping task is not scheduled before its deadline");

    g_now = 1060;
    task_yield();
    eq(g_log, "sS", "it wakes once the deadline passes");
    task_reap(s);

    // A sleeper with NOTHING else runnable must still wait out its deadline.
    // Returning early would set it back to RUNNING and continue immediately —
    // sleep(200) finishing in microseconds, which is not sleeping. pid 1 drives
    // the clock forward here the way the real timer would.
    g_log[0] = 0;
    g_now = 2000;
    int alone = task_spawn("alone", nullptr, task_sleeper, nullptr, ST, AFFINITY_ANY);
    task_yield();                       // it runs and parks itself
    eq(g_log, "s", "the lone sleeper parked");
    ck(task_find(alone)->state == TASK_SLEEPING, "and is genuinely SLEEPING");
    g_now = 2049;
    ck(task_find(alone)->state == TASK_SLEEPING, "still asleep one ms early");
    g_now = 2050;
    task_yield();
    eq(g_log, "sS", "and wakes exactly on the deadline, not before");
    task_reap(alone);

    // --- cooperative kill ---------------------------------------------------
    g_log[0] = 0;
    int f = task_spawn("f", nullptr, task_forever, nullptr, ST, AFFINITY_ANY);
    task_yield();
    task_yield();
    ck(strlen(g_log) >= 2, "the long-running task is getting the core");

    ck(task_kill(f), "kill is accepted");
    ck(task_find(f)->kill_requested, "and recorded as a request, not a teardown");
    task_yield();
    task_yield();
    ck(task_find(f)->state == TASK_DONE, "the task stops at its next yield");
    ck(task_find(f)->exit_code == 42, "and exits with its own status");
    ck(g_log[strlen(g_log) - 1] == 'k', "it ran its own cleanup on the way out");
    task_reap(f);

    // --- a task finishing when nothing else can run --------------------------
    //
    // THE crash. task_exit calls reschedule and never expects to come back; if
    // it does, it unwinds into task_trampoline, which was entered by popping a
    // synthetic stack frame and has no return address. On the device that
    // branches to zero and locks up hard.
    //
    // The setup matters: pid 1 must be the ONLY other task and must be parked as
    // not-runnable at the moment the spawned task finishes, so pick() finds
    // nothing. Sleeping pid 1 past the finisher's exit does exactly that.
    g_log[0] = 0;
    g_now = 5000;
    int lone = task_spawn("lone", nullptr, task_quick, nullptr, ST, AFFINITY_ANY);
    ck(lone > 0, "spawned a task that will finish with nothing to follow it");

    // Hand over and come back only once it is done. Before the fix, control
    // never returned here at all.
    for (int i = 0; i < 6 && task_find(lone) && task_find(lone)->state != TASK_DONE; i++)
        task_yield();

    eq(g_log, "q", "the lone task ran");
    ck(task_find(lone) && task_find(lone)->state == TASK_DONE,
       "and finished cleanly instead of running off the end of its stack");
    ck(task_self() == 1, "control came back to the shell");
    task_reap(lone);

    // Several in a row, which is what the stress test actually does.
    g_log[0] = 0;
    for (int round = 0; round < 5; round++) {
        int p = task_spawn("burst", nullptr, task_quick, nullptr, ST, AFFINITY_ANY);
        for (int i = 0; i < 4; i++) task_yield();
        task_reap(p);
    }
    eq(g_log, "qqqqq", "five short-lived tasks in a row all finish");
    ck(task_count() == 1, "and none of them leaked a slot");

    // --- guards -------------------------------------------------------------
    ck(!task_kill(1), "pid 1 cannot be killed");
    ck(!task_kill(9999), "an unknown pid is refused");
    ck(task_spawn("bad", nullptr, nullptr, nullptr, ST, AFFINITY_ANY) < 0,
       "a null entry point is refused");

    // Affinity: a CORE1 task must not be picked on core 0, which is what makes
    // a single-core build safe.
    int c1 = task_spawn("c1", nullptr, task_quick, nullptr, ST, AFFINITY_CORE1);
    g_log[0] = 0;
    for (int i = 0; i < 4; i++) task_yield();
    eq(g_log, "", "a core-1 task is never scheduled on core 0");
    ck(task_find(c1)->state == TASK_READY, "it stays ready, waiting for its core");
    task_kill(c1);

    // Fill the table and check the failure is clean rather than a crash.
    int spawned = 0;
    while (task_spawn("fill", nullptr, task_quick, nullptr, 1024, AFFINITY_CORE1) > 0)
        spawned++;
    ck(spawned > 0 && task_count() <= TASK_MAX, "the table fills and then refuses");

    // --- a stop request must not outlive the command ------------------------
    //
    // This one reached hardware. The watchdog asked the SHELL to stop during a
    // slow WiFi join; nothing cleared the flag, so task_should_stop stayed true
    // forever. intr_check folds it in, so every interruptible loop afterwards
    // gave up at once — scans found no networks, pings timed out immediately —
    // while the shell itself carried on looking perfectly healthy.
    task_clear_stop();
    ck(!task_should_stop(), "no stop request outstanding to begin with");

    // The shell is exempt from being asked at all. Ending it leaves a device
    // nobody can type at, and the guard used to be a hardcoded pid 1 — which
    // stopped being the shell the moment main became the idle task.
    task_mark_shell();
    ck(task_shell_pid() == task_self(), "the shell identifies itself by pid");
    ck(!task_kill(task_self()), "and cannot be asked to stop");
    ck(!task_should_stop(), "so its flag is never set");

    // Asserted rather than assumed. The boot WiFi join was given
    // TASK_STACK_DEF, overflowed inside the cyw43 driver, and corrupted memory
    // until it hard faulted somewhere unrelated — an overflow does not report
    // itself, and the guard only checks at a yield, which a driver call never
    // reaches.
    ck(TASK_STACK_NET >= TASK_STACK_SHELL, "a network task gets at least what the shell gets");
    ck(TASK_STACK_DEF < TASK_STACK_NET, "and more than the default, which the driver overruns");

    printf("  task: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
