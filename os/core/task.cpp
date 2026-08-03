#include "task.h"
#include "lock.h"
#include "blackbox.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// A task's bookkeeping plus the two things TaskInfo does not expose: its saved
// stack pointer, and the block it was allocated from.
struct Task {
    TaskInfo info;
    void    *sp;            // saved stack pointer while not running
    void    *stack;         // the malloc'd block; null for the adopted main task
    TaskFn   fn;
    void    *arg;
    uint32_t wake_at_ms;    // for TASK_SLEEPING
    uint32_t entered_ms;    // when it last got the core, for cpu_ms
};

static bool     g_up;                // task_init has run
static Task     g_tasks[TASK_MAX];
static uint32_t g_used;              // highest slot ever used; slots are stable
static int      g_next_pid = 1;

// One "currently running" slot per core, so both cores can be inside the
// scheduler at once without agreeing on a single global.
#define MAX_CORES 2
static int      g_current[MAX_CORES] = { -1, -1 };
static void    *g_sched_sp[MAX_CORES];    // where the scheduler loop parked
static uint32_t g_cores = 1;

// The stack is filled with this before use, so the high-water mark can be found
// by counting how much of it is still untouched. That is the only way to report
// stack usage honestly without instrumenting every call.
#define STACK_PAINT 0xA5A5A5A5u
// A distinct value at the very bottom, so an overflow is detectable even after
// the paint above it has legitimately been used.
#define STACK_GUARD 0xDEADBE57u

static Task *slot_of(int pid) {
    for (uint32_t i = 0; i < TASK_MAX; i++)
        if (g_tasks[i].info.state != TASK_FREE && g_tasks[i].info.pid == pid)
            return &g_tasks[i];
    return nullptr;
}

static Task *cur(void) {
    int pid = g_current[task_this_core() % MAX_CORES];
    return pid < 0 ? nullptr : slot_of(pid);
}

// --- stack accounting -------------------------------------------------------

static void paint(void *base, uint32_t bytes) {
    uint32_t *p = (uint32_t *)base;
    for (uint32_t i = 0; i < bytes / 4; i++) p[i] = STACK_PAINT;
    for (uint32_t i = 0; i < TASK_GUARD_BYTES / 4; i++) p[i] = STACK_GUARD;
}

// True when the tripwire is intact. The stack grows DOWN, so the guard sits at
// the lowest addresses and is the first thing an overflow reaches.
static bool guard_ok(const Task *t) {
    if (!t->stack) return true;                  // the adopted boot stack
    const uint32_t *p = (const uint32_t *)t->stack;
    for (uint32_t i = 0; i < TASK_GUARD_BYTES / 4; i++)
        if (p[i] != STACK_GUARD) return false;
    return true;
}

static uint32_t high_water(const Task *t) {
    if (!t->stack) return 0;                     // adopted main stack: unknown
    const uint32_t *p = (const uint32_t *)t->stack;
    const uint32_t words = t->info.stack_size / 4;
    // Start ABOVE the guard. Its words hold STACK_GUARD rather than STACK_PAINT,
    // so a scan that began at zero would stop on the very first word and report
    // the entire stack as used.
    const uint32_t first = TASK_GUARD_BYTES / 4;
    uint32_t untouched = 0;
    for (uint32_t i = first; i < words; i++) {
        if (p[i] != STACK_PAINT) break;
        untouched++;
    }
    return t->info.stack_size - TASK_GUARD_BYTES - untouched * 4;
}

// --- scheduling -------------------------------------------------------------

static bool runnable_on(const Task *t, uint32_t core) {
    if (t->info.state == TASK_READY) {
        // fallthrough to the affinity test
    } else if (t->info.state == TASK_SLEEPING) {
        if ((int32_t)(task_now_ms() - t->wake_at_ms) < 0) return false;
    } else {
        return false;
    }
    if (t->info.affinity == AFFINITY_ANY) return true;
    if (t->info.affinity == AFFINITY_CORE0) return core == 0;
    return core == 1;
}

// Round-robin from the slot after the current one, so a task that yields does
// not immediately get the core back while something else is waiting.
static Task *pick(uint32_t core, int after_pid) {
    int start = 0;
    for (uint32_t i = 0; i < TASK_MAX; i++)
        if (g_tasks[i].info.state != TASK_FREE && g_tasks[i].info.pid == after_pid) {
            start = (int)i + 1;
            break;
        }
    for (uint32_t n = 0; n < TASK_MAX; n++) {
        Task *t = &g_tasks[(start + n) % TASK_MAX];
        if (t->info.state == TASK_FREE) continue;
        if (runnable_on(t, core)) return t;
    }
    return nullptr;
}

// --- lifecycle --------------------------------------------------------------

void task_init(const char *name) {
    memset(g_tasks, 0, sizeof(g_tasks));
    g_cores = task_core_count();
    if (g_cores > MAX_CORES) g_cores = MAX_CORES;
    for (int i = 0; i < MAX_CORES; i++) g_current[i] = -1;

    g_up = true;

    Task &t = g_tasks[0];
    t.info.pid      = g_next_pid++;
    t.info.state    = TASK_RUNNING;
    t.info.affinity = AFFINITY_CORE0;      // it owns the terminal
    t.info.core     = 0;
    t.stack         = nullptr;             // the C startup stack; not ours to measure
    t.entered_ms    = task_now_ms();
    snprintf(t.info.name, sizeof(t.info.name), "%s", name ? name : "init");
    snprintf(t.info.path, sizeof(t.info.path), "%s", "(kernel)");
    g_used = 1;
    g_current[0] = t.info.pid;
}

// Every task starts here rather than at fn directly, so a task that simply
// returns is cleaned up the same way as one that calls task_exit. Without this
// a returning task would run off the end of its stack into nothing.
static void task_trampoline(void) {
    Task *t = cur();
    int rc = t->fn ? t->fn(t->arg) : 0;
    task_exit(rc);
}

int task_spawn(const char *name, const char *path, TaskFn fn, void *arg,
               uint32_t stack_bytes, TaskAffinity affinity) {
    if (!fn) return -1;
    if (stack_bytes < TASK_STACK_MIN) stack_bytes = TASK_STACK_MIN;
    stack_bytes = (stack_bytes + 7u) & ~7u;      // AAPCS wants 8-byte alignment

    // Claim a slot under the guard so two cores cannot take the same one. The
    // allocation happens after, outside it: malloc can be slow and the guard
    // masks interrupts.
    lock_hw_enter();
    Task *t = nullptr;
    for (uint32_t i = 0; i < TASK_MAX; i++)
        if (g_tasks[i].info.state == TASK_FREE) {
            t = &g_tasks[i];
            t->info.state = TASK_BLOCKED;      // reserved; not yet runnable
            break;
        }
    lock_hw_exit();
    if (!t) return -1;

    void *stack = malloc(stack_bytes);
    if (!stack) { t->info.state = TASK_FREE; return -1; }
    paint(stack, stack_bytes);

    memset(&t->info, 0, sizeof(t->info));
    t->info.pid        = g_next_pid++;
    t->info.affinity   = affinity;
    t->info.stack_size = stack_bytes;
    snprintf(t->info.name, sizeof(t->info.name), "%s", name ? name : "task");
    snprintf(t->info.path, sizeof(t->info.path), "%s", path ? path : "-");

    t->stack = stack;
    t->fn    = fn;
    t->arg   = arg;
    t->sp    = task_ctx_init((uint8_t *)stack + stack_bytes, task_trampoline);

    if ((uint32_t)(t - g_tasks) + 1 > g_used) g_used = (uint32_t)(t - g_tasks) + 1;
    // READY last, and only once everything it needs is in place — the instant
    // this is set, the other core may pick it up and run it.
    t->info.state = TASK_READY;
    return t->info.pid;
}

// The heart of it: park the current task and run the next runnable one.
//
// The selection has to be atomic ACROSS CORES. Without that, both cores can run
// pick() at the same instant, choose the same READY task, and start executing it
// on two cores at once — one task, one stack, two CPUs. Marking the chosen task
// RUNNING before releasing the guard is what prevents it, because pick only ever
// considers READY and SLEEPING.
//
// The guard is NOT held across the context switch. It cannot be: the switch
// leaves on one stack and returns on another, so the matching release would run
// in a different context from the acquire. Everything that needs protecting is
// done before then.
static void reschedule(TaskState park_as) {
    // Nothing to schedule before task_init has run. This is reachable for real:
    // intr_check yields, and it is called from code that also runs during boot,
    // before the scheduler exists. Falling through would reach the "wait for
    // something runnable" spin below and hang the device with no clue why.
    if (!g_up) return;

    uint32_t core = task_this_core() % MAX_CORES;

    // Only CORE 0 feeds the watchdog.
    //
    // Core 1's idle loop yields continuously, so letting it feed meant a
    // deadlocked core 0 was masked by a perfectly healthy core 1 — the device
    // hung with the watchdog happily being petted, which is why the lockup never
    // self-recovered. A watchdog that any running thing can satisfy is not
    // watching the thing that matters.
    if (core == 0) task_watchdog_feed();

    // Progress, recorded where it survives a reset. This is what turns "the
    // watchdog fired" into "the watchdog fired while 'stress' was running and it
    // had already yielded 812 times".
    bb_note_yield(task_now_ms());

    lock_hw_enter();
    Task *me = cur();

    // Check the tripwire before anything else. A task that has overrun its stack
    // has already written over its neighbour; the only useful thing left is to
    // say which task it was, loudly, rather than let it keep running and put the
    // damage somewhere it will be blamed on something else.
    if (me && !guard_ok(me)) {
        lock_hw_exit();
        task_stack_overflow(me->info.name, me->info.stack_size);
    }

    if (me) {
        me->info.cpu_ms += task_now_ms() - me->entered_ms;
        // A finished task already recorded its high-water mark in task_exit;
        // recomputing it here would overwrite the real figure.
        if (me->info.state != TASK_DONE) me->info.stack_used = high_water(me);
        if (me->info.state == TASK_RUNNING) me->info.state = park_as;
    }

    Task *next = pick(core, me ? me->info.pid : 0);

    // Nothing else to run. If this task is still runnable, just carry on —
    // switching to ourselves would be a pointless save/restore.
    if (!next) {
        if (me && (me->info.state == TASK_READY || me->info.state == TASK_SLEEPING)) {
            me->info.state = TASK_RUNNING;
            me->entered_ms = task_now_ms();
            lock_hw_exit();
            return;
        }
        // A FINISHED task must never return from here.
        //
        // task_exit calls reschedule and relies on never coming back. Returning
        // unwinds into task_trampoline, which was entered by popping a synthetic
        // frame and therefore has no return address — it branches to zero and the
        // device locks up hard. That is the crash: a task finishing at a moment
        // when nothing else happened to be runnable, which is exactly what a test
        // spawning short-lived tasks does over and over.
        //
        // So a finished task either goes back to the core's own scheduler
        // context, or waits here until something becomes runnable. Either way it
        // does not return.
        if (me && me->info.state == TASK_DONE) {
            if (g_sched_sp[core]) {
                void *back = g_sched_sp[core];
                g_sched_sp[core] = nullptr;
                g_current[core]  = -1;
                lock_hw_exit();
                task_ctx_switch(&me->sp, back);
                return;                      // not reached: this stack is finished
            }
            // No scheduler context on this core (core 0 never parks one). Wait
            // for a sleeper to come due or the other core to release something.
            // The guard is released between attempts so the other core can make
            // the progress being waited for.
            while (true) {
                lock_hw_exit();
                task_watchdog_feed();
                lock_hw_enter();
                next = pick(core, 0);
                if (next) break;
            }
            // Fall through with `next` chosen.
        } else {
            // Still runnable, just not now: release and let the caller come back.
            lock_hw_exit();
            return;
        }
    }

    if (!next) { lock_hw_exit(); return; }

    if (me && next == me) {
        me->info.state = TASK_RUNNING;
        me->entered_ms = task_now_ms();
        lock_hw_exit();
        return;
    }

    next->info.state    = TASK_RUNNING;   // claims it; no other core will pick it
    next->info.core     = (uint8_t)core;
    if (core == 0)
        bb_note_task(next->info.pid, (uint8_t)core, next->info.name,
                     next->info.stack_used, next->info.stack_size);
    next->info.switches++;
    next->entered_ms    = task_now_ms();
    g_current[core]     = next->info.pid;
    lock_hw_exit();

    if (me) task_ctx_switch(&me->sp, next->sp);
    else    task_ctx_switch(&g_sched_sp[core], next->sp);
}

void task_yield(void) { reschedule(TASK_READY); }

void task_sleep_ms(uint32_t ms) {
    Task *me = cur();
    if (!me) return;
    me->wake_at_ms = task_now_ms() + ms;
    reschedule(TASK_SLEEPING);
}

bool task_kill(int pid) {
    Task *t = slot_of(pid);
    if (!t) return false;
    if (t->info.pid == 1) return false;          // the shell that owns the boot
    if (t->info.state == TASK_DONE) return false;
    // Cooperative: the task stops itself at its next yield point, where it is by
    // definition not holding a lock or part-way through a flash write. Tearing
    // it down here would corrupt whatever it was doing.
    t->info.kill_requested = true;
    if (t->info.state == TASK_SLEEPING) {        // wake it so it notices
        t->wake_at_ms = task_now_ms();
        t->info.state = TASK_READY;
    }
    return true;
}

bool task_should_stop(void) {
    Task *t = cur();
    return t && t->info.kill_requested;
}

void task_exit(int code) {
    Task *me = cur();
    if (!me) return;
    me->info.exit_code  = code;
    me->info.stack_used = high_water(me);
    me->info.state      = TASK_DONE;
    // The stack is NOT freed here. This function is still RUNNING on it, and so
    // is the reschedule below, right up to the register save inside the context
    // switch. Freeing it here hands live memory back to the allocator while it
    // is in use — the next malloc from anywhere could take it, and the switch
    // would then save registers into someone else's buffer. task_reap frees it,
    // once nothing is executing on it any more.
    reschedule(TASK_DONE);
}

int task_self(void) {
    Task *t = cur();
    return t ? t->info.pid : -1;
}

const TaskInfo *task_current(void) {
    Task *t = cur();
    return t ? &t->info : nullptr;
}

// --- introspection ----------------------------------------------------------

uint32_t task_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < TASK_MAX; i++) if (g_tasks[i].info.state != TASK_FREE) n++;
    return n;
}

const TaskInfo *task_at(uint32_t idx) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].info.state == TASK_FREE) continue;
        // Keep the live high-water mark fresh for whoever is listing, so `ps`
        // does not report a number frozen at the last context switch.
        if (g_tasks[i].info.state != TASK_DONE)
            g_tasks[i].info.stack_used = high_water(&g_tasks[i]);
        if (n++ == idx) return &g_tasks[i].info;
    }
    return nullptr;
}

const TaskInfo *task_find(int pid) {
    Task *t = slot_of(pid);
    return t ? &t->info : nullptr;
}

bool task_reap(int pid) {
    Task *t = slot_of(pid);
    if (!t || t->info.state != TASK_DONE) return false;
    if (t->stack) { free(t->stack); t->stack = nullptr; }
    t->info.state = TASK_FREE;
    return true;
}
