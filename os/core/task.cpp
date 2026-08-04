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
    // The stack, twice over. `stack` is where it actually begins — aligned, so
    // the hardware guard can be placed on it — and `stack_raw` is what malloc
    // returned and what must be given back. They differ by up to 31 bytes, and
    // freeing the wrong one corrupts the heap in a way that surfaces much later
    // somewhere else.
    //
    // Both are null for the adopted main task: it runs on the C startup stack,
    // which this scheduler neither allocated nor may release.
    void    *stack;
    void    *stack_raw;
    TaskFn   fn;
    void    *arg;
    uint32_t wake_at_ms;    // for TASK_SLEEPING
    uint32_t entered_ms;    // when it last got the core, for cpu_ms

    // True while this task's context is ON a core — from the moment a core
    // claims it until its stack pointer has actually been written away.
    //
    // `state` alone cannot express that. A task is marked READY (or SLEEPING,
    // deadline passed) while it is still physically executing, because the
    // state is set inside the scheduler lock but `sp` is not written until
    // task_ctx_switch runs, after the lock is released. In that window the
    // other core would pick it up and resume from the STALE sp left by its
    // previous park — a stack region long since overwritten. Restoring from it
    // pops rubbish into pc and lr, which was every hard fault seen while
    // anything ran in the background.
    //
    // runnable_on refuses a live task, so the window closes. Cleared inside
    // task_ctx_switch, immediately after the stack pointer is stored, because
    // that is the only moment that qualifies: from a task's own point of view
    // task_ctx_switch does not return until it is resumed, and by then the flag
    // has to be set again rather than cleared.
    volatile bool live;

    // How many locks this task is holding. "Do not force-exit me right now"
    // belongs to the TASK, not to the core it happens to be on: a task can take
    // a lock on one core, yield, and be resumed on the other, and a per-core
    // count would then be decremented on a core that never incremented it. The
    // first core's count sticks at non-zero forever, which quietly disables
    // preemption there for the rest of the boot.
    uint32_t crit;

    // The package this task is currently executing, described for the
    // protection hardware — or nothing, which is the usual case.
    //
    // Held BY VALUE rather than as a pointer to the loader's own record. The
    // record for a package that runs and exits lives on the caller's stack, and
    // the one for a resident package lives in a table entry that unloading
    // frees; a pointer to either would outlive its subject in exactly the
    // circumstances where a stale protection region does the most damage.
    // Twenty-four bytes a task is a cheap way not to have to think about it.
    TaskAppMem app_mem;
    bool       app_mem_set;
    // Where fw_malloc sends this task while it is inside a sandboxed package.
    // Owned by whoever set it; the scheduler only carries it.
    Arena     *arena;
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

// Re-establish the hardware protection for whatever is now running on this
// core, and be called at EVERY point where execution resumes.
//
// The protection hardware belongs to the core, and a task does not: it parks on
// one and, 99 times in 100 on this chip, comes back on the other. So a guard
// installed when a task was created, or by the core that parked it, describes a
// stack that is not the one in use. That failure is completely silent — the
// guard still works, it just protects somebody else — which is why this is one
// function called from a handful of places rather than three lines copied to
// each of them.
//
// Null means the core's own scheduler context, which runs on the boot stack.
static void arm_protection(Task *t) {
    if (t && t->stack) task_stack_guard_set(t->stack, t->info.stack_size);
    else               task_stack_guard_set(nullptr, 0);
    task_app_mem_apply(t && t->app_mem_set ? &t->app_mem : nullptr);
}

static Task *slot_of(int pid) {
    for (uint32_t i = 0; i < TASK_MAX; i++)
        if (g_tasks[i].info.state != TASK_FREE && g_tasks[i].info.pid == pid)
            return &g_tasks[i];
    return nullptr;
}

// The running task on this core.
//
// Memoised, because this is now on the hot path twice over: every lock acquire
// and release asks whether the current task is in a critical section, and a
// full-screen redraw takes the output lock hundreds of times. Without the memo
// each of those walked the whole table looking up a pid it had just looked up.
//
// Safe against the other core: the entry is per-core and validated against the
// pid before use, and the task a core is currently running cannot be freed or
// reused underneath it — it is live by definition.
static Task *cur(void) {
    unsigned c = task_this_core() % MAX_CORES;
    int pid = g_current[c];
    if (pid < 0) return nullptr;
    static Task *memo[MAX_CORES];
    Task *m = memo[c];
    if (m && m->info.pid == pid && m->info.state != TASK_FREE) return m;
    Task *t = slot_of(pid);
    memo[c] = t;
    return t;
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
    // Still on a core, whatever its state says. Picking it here would run one
    // stack on two cores at once; see the comment on Task::live.
    if (t->live) return false;
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
    t.stack         = nullptr;             // the C startup stack; not ours to free
    // Its size IS known — the linker placed it — so the guard and `ps` can both
    // report something real for the task that runs every command.
    t.info.stack_size = task_main_stack_size();
    t.entered_ms    = task_now_ms();
    t.live          = true;                // it is running: this call is on its stack
    t.crit          = 0;
    snprintf(t.info.name, sizeof(t.info.name), "%s", name ? name : "init");
    snprintf(t.info.path, sizeof(t.info.path), "%s", "(kernel)");
    g_used = 1;
    g_current[0] = t.info.pid;

    // pid 1 runs on the C startup stack, which the platform locates from the
    // linker. Guarding it matters more than guarding any other: it is the task
    // the shell runs in, so every command and every package entry point is
    // somewhere on it.
    arm_protection(&t);
}

// Every task starts here rather than at fn directly, so a task that simply
// returns is cleaned up the same way as one that calls task_exit. Without this
// a returning task would run off the end of its stack into nothing.
static void task_trampoline(void) {
    Task *t = cur();
    // A brand-new task did not come back from a context switch, so nothing has
    // armed its guard yet. This is the other half of the rule in
    // arm_protection: every place execution resumes, including the first time.
    arm_protection(t);
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

    // Nothing free. Take back the oldest FINISHED task before giving up.
    //
    // A task that has exited keeps its slot so its exit status can still be
    // read, and the only thing that ever released one was somebody running
    // `ps`. So a device doing background work simply ran out: twelve slots,
    // two per run of a diagnostic, and after four runs nothing could spawn
    // again — with the stacks held too, so it leaked memory on the way.
    //
    // Oldest first, by pid, so a status that was just set survives longest and
    // the reclaimed one is the least likely to still be wanted.
    if (!t) {
        Task *oldest = nullptr;
        for (uint32_t i = 0; i < TASK_MAX; i++) {
            Task *c = &g_tasks[i];
            if (c->info.state != TASK_DONE) continue;
            if (!oldest || c->info.pid < oldest->info.pid) oldest = c;
        }
        if (oldest) {
            if (oldest->stack_raw) free(oldest->stack_raw);
            oldest->stack = oldest->stack_raw = nullptr;
            oldest->info.state = TASK_BLOCKED;   // straight to reserved
            oldest->live = false;
            oldest->crit = 0;
            oldest->app_mem_set = false;
            oldest->arena = nullptr;
            task_slot_recycled((int)(oldest - g_tasks));
            t = oldest;
        }
    }
    lock_hw_exit();
    if (!t) return -1;

    // Room for the alignment on top of the stack itself. The hardware guard has
    // to sit on a TASK_STACK_ALIGN boundary and malloc only promises eight, so
    // the slack is asked for here and the usable stack starts at the first
    // boundary inside it. Rounding the other way would put the guard below the
    // block, in whatever the heap handed out previously.
    //
    // Done in uintptr_t rather than through the 32-bit region helpers: this
    // file also compiles for the host test, where a pointer does not fit in
    // thirty-two bits and truncating one produces an address that is not a
    // stack at all.
    void *raw = malloc(stack_bytes + TASK_STACK_ALIGN);
    if (!raw) { t->info.state = TASK_FREE; return -1; }
    uintptr_t aligned = ((uintptr_t)raw + (TASK_STACK_ALIGN - 1))
                        & ~(uintptr_t)(TASK_STACK_ALIGN - 1);
    void *stack = (void *)aligned;
    paint(stack, stack_bytes);

    memset(&t->info, 0, sizeof(t->info));
    t->info.pid        = g_next_pid++;
    t->info.affinity   = affinity;
    t->info.stack_size = stack_bytes;
    snprintf(t->info.name, sizeof(t->info.name), "%s", name ? name : "task");
    snprintf(t->info.path, sizeof(t->info.path), "%s", path ? path : "-");

    t->stack     = stack;
    t->stack_raw = raw;
    t->fn        = fn;
    t->arg       = arg;
    t->app_mem_set = false;
    t->arena = nullptr;
    t->live  = false;        // slots are reused; the memset above only clears info
    t->crit  = 0;

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
//
// That last paragraph is exactly why marking the NEXT task is not sufficient on
// its own. The task being parked is advertised inside the guard but its stack
// pointer is not written until the switch, after the release — so for a short
// window it is claimable by the other core while its saved sp is still the
// stale one from its previous park. `live` covers that window; see the comment
// on the field. Reasoning about `next` and forgetting `me` is the bug this
// scheduler shipped with.
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
    // Core 0 only. Core 1's idle loop yields thousands of times a second, so
    // counting both made the figure a measure of how long the device had been
    // on rather than of what the foreground was doing.
    if (core == 0) bb_note_yield(task_now_ms());

    // Record WHO is running on every pass, not only on a context switch.
    //
    // Recording it at the switch alone meant the shell was never recorded: pid 1
    // is usually the only runnable task, so reschedule takes an early return and
    // never switches to it. A hang at the login prompt therefore left the black
    // box empty — which is exactly what happened, and left a crash report with
    // nothing in it.
    if (core == 0) {
        const Task *now = cur();
        if (now) bb_note_task(now->info.pid, (uint8_t)core, now->info.name,
                              now->info.stack_used, now->info.stack_size);
    }

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

    // The adopted task (pid 1) has no painted stack, so it gets the equivalent
    // check against the linker's floor. Without this the shell — where every
    // command actually runs — was the one task in the system with no tripwire
    // at all, which is exactly where the overflow turned out to be.
    if (me && !me->stack && core == 0) {
        uint32_t left = task_main_stack_headroom();
        me->info.stack_used = me->info.stack_size > left ? me->info.stack_size - left : 0;
        if (left < 256) {
            lock_hw_exit();
            task_stack_overflow(me->info.name, me->info.stack_size);
        }
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
        // Still runnable, just nothing better to switch to: carry on.
        if (me && me->info.state == TASK_READY) {
            me->info.state = TASK_RUNNING;
            me->entered_ms = task_now_ms();
            lock_hw_exit();
            return;
        }

        // ASLEEP and nothing else to run *at this instant*. Returning here would
        // set it back to RUNNING and continue immediately — sleep(200) would
        // return in microseconds, which is not sleeping. So the deadline has to
        // be waited out.
        //
        // The wait re-checks for work on every pass rather than spinning the
        // whole deadline out. "Nothing runnable" was only true when the loop was
        // entered: the other core finishes something, a service comes due, a
        // keystroke wakes the shell — and a sleeper that kept the core to itself
        // made all of those wait for a deadline that had nothing to do with
        // them. A 50 ms poll in the network task therefore cost the shell up to
        // 50 ms of latency for no reason, which is exactly the sort of thing
        // that reads as "the device feels sluggish while it is downloading".
        //
        // Waking early is not a correctness risk: this task is still SLEEPING
        // and still `live`, so nothing can pick it up, and it resumes here with
        // its deadline intact.
        if (me && me->info.state == TASK_SLEEPING) {
            uint32_t wake = me->wake_at_ms;
            while (true) {
                lock_hw_exit();
                if (core == 0) task_watchdog_feed();
                lock_hw_enter();

                if ((int32_t)(task_now_ms() - wake) >= 0) {
                    me->info.state = TASK_RUNNING;
                    me->entered_ms = task_now_ms();
                    lock_hw_exit();
                    return;
                }

                // Something became runnable while waiting: hand the core over
                // and let this task be woken by whoever runs next. Falls through
                // to the switch below with `next` chosen.
                next = pick(core, me->info.pid);
                if (next) break;
            }
        } else
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
                task_ctx_switch(&me->sp, back, &me->live);
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
    next->live          = true;           // ...and holds the claim past the unlock
    next->info.core     = (uint8_t)core;
    if (core == 0)
        bb_note_task(next->info.pid, (uint8_t)core, next->info.name,
                     next->info.stack_used, next->info.stack_size);
    next->info.switches++;
    next->entered_ms    = task_now_ms();
    g_current[core]     = next->info.pid;
    lock_hw_exit();

    // &me->live: cleared inside the switch, the instant after sp is stored.
    // The scheduler context (me == nullptr) has no task to advertise.
    //
    // Both calls return only when THIS context is resumed, which may be on the
    // other core and will certainly be after other tasks have reprogrammed the
    // protection hardware for themselves. So the guard is re-armed here, on the
    // far side of the switch, and not before it.
    if (me) {
        task_ctx_switch(&me->sp, next->sp, &me->live);
        arm_protection(me);
    } else {
        task_ctx_switch(&g_sched_sp[core], next->sp, nullptr);
        arm_protection(nullptr);
    }
}

void task_yield(void) { reschedule(TASK_READY); }

// Move a task to a different core.
//
// Setting the affinity is only half of it: a task already RUNNING somewhere is
// not moved by changing what it is allowed to do. The caller has to yield, and
// keep yielding, until the scheduler happens to place it where it asked — which
// works because a parked task is picked by whichever core finds it runnable,
// and the new affinity now excludes the wrong one.
//
// Safe only because `live` exists. Parking a task so another core can take it
// is precisely the operation that used to run one stack on two cores.
bool task_set_affinity(int pid, TaskAffinity aff) {
    lock_hw_enter();
    Task *t = slot_of(pid);
    if (t) t->info.affinity = aff;
    lock_hw_exit();
    return t != nullptr;
}

TaskAffinity task_affinity(int pid) {
    lock_hw_enter();
    Task *t = slot_of(pid);
    TaskAffinity a = t ? t->info.affinity : AFFINITY_ANY;
    lock_hw_exit();
    return a;
}

// Put THIS task on `core` and do not return until it is there.
//
// Bounded, because a caller that cannot be moved must not hang: on a
// single-core build, or if the target core is somehow never scheduling, this
// gives up and reports it rather than yielding for ever.
bool task_migrate_to(uint32_t core) {
    if (task_this_core() == core) return true;
    if (g_cores <= 1) return core == 0;      // nowhere else to go

    Task *me = cur();
    if (!me) return false;
    task_set_affinity(me->info.pid, core == 0 ? AFFINITY_CORE0 : AFFINITY_CORE1);

    for (int i = 0; i < 200 && task_this_core() != core; i++) task_yield();
    return task_this_core() == core;
}

// --- "busy, do not interrupt" ------------------------------------------------
//
// Held against the TASK. Before the scheduler exists there is no task to hold
// it against, and boot does take locks, so those are counted separately rather
// than special-cased at every call site.
static uint32_t g_pre_init_crit;

void task_crit_enter(void) {
    Task *t = cur();
    if (t) t->crit++;
    else   g_pre_init_crit++;
}

void task_crit_leave(void) {
    Task *t = cur();
    if (t) { if (t->crit) t->crit--; }
    else if (g_pre_init_crit) g_pre_init_crit--;
}

bool task_crit_active(void) {
    Task *t = cur();
    return t ? t->crit != 0 : g_pre_init_crit != 0;
}

void task_alive(void) {
    if (!g_up) return;
    if (task_this_core() != 0) return;      // core 0 owns the watchdog

    // Rate-limited, because this is called from EVERY ABI entry point and a
    // package can reach several dozen of them per frame.
    //
    // The work below is not free — a 64-bit division for the millisecond
    // clock, a hardware watchdog write, a stall comparison and a stack probe —
    // and doing it sixty times a frame was the single largest cost in drawing a
    // screen. It made a full-screen app feel slower than the interpreted
    // version it replaced.
    //
    // Liveness does not need millisecond resolution. The stall threshold is
    // three seconds; reporting progress every 25 ms is two orders of magnitude
    // finer than anything that reads it, and turns the common case into one
    // comparison.
    static uint32_t last_us;
    uint32_t now_us = task_now_us();
    if (now_us - last_us < 25000u) return;
    last_us = now_us;

    // Progress, recorded BEFORE feeding. The stall detector measures time since
    // the last recorded progress, so without this a busy package would be
    // reported as unresponsive and asked to stop while it was working
    // perfectly — feeding the watchdog but never clearing the stall.
    bb_note_yield(task_now_ms());
    task_watchdog_feed();

    // The stack check the yield path would have done. Without this a package
    // that never yields could overrun its stack with nothing watching, which is
    // the one place an overflow does the most damage and is hardest to find.
    Task *me = cur();
    if (!me) return;
    if (!me->stack) {
        if (task_main_stack_headroom() < 256)
            task_stack_overflow(me->info.name, me->info.stack_size);
        return;
    }
    if (!guard_ok(me)) task_stack_overflow(me->info.name, me->info.stack_size);
    me->info.stack_used = high_water(me);   // so a crash report is not stale
}

void task_sleep_ms(uint32_t ms) {
    Task *me = cur();
    if (!me) return;
    me->wake_at_ms = task_now_ms() + ms;
    reschedule(TASK_SLEEPING);
}

// Set once the shell task starts. Zero before then, which is why every use is
// guarded rather than assuming a valid pid.
static int g_shell_pid;

bool task_kill(int pid) {
    Task *t = slot_of(pid);
    if (!t) return false;
    // Never the shell: ending it leaves a device nobody can type at. The pid is
    // looked up rather than assumed — it stopped being 1 the moment main became
    // the idle task and the shell was spawned into its own.
    if (t->info.pid == 1) return false;              // the idle task
    if (g_shell_pid && t->info.pid == g_shell_pid) return false;
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

void task_clear_stop(void) {
    Task *t = cur();
    if (t) t->info.kill_requested = false;
}

void task_mark_shell(void) { Task *t = cur(); if (t) g_shell_pid = t->info.pid; }
int  task_shell_pid(void)  { return g_shell_pid; }

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

int task_slot_index(void) {
    Task *t = cur();
    return t ? (int)(t - g_tasks) : -1;
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
    // stack_raw, not stack: the latter is a few bytes further in, and handing
    // the heap a pointer it never issued corrupts it silently.
    if (t->stack_raw) free(t->stack_raw);
    t->stack = t->stack_raw = nullptr;
    t->app_mem_set = false;
    t->arena = nullptr;
    task_slot_recycled((int)(t - g_tasks));
    t->info.state = TASK_FREE;
    return true;
}

// --- package memory ---------------------------------------------------------

void task_app_mem_set(const TaskAppMem *mem) {
    Task *t = cur();
    if (!t || !mem) return;
    t->app_mem     = *mem;
    t->app_mem_set = true;
    task_app_mem_apply(&t->app_mem);
}

int task_app_mem_holder(const void *text) {
    if (!text) return -1;
    Task *me = cur();
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        Task *t = &g_tasks[i];
        if (t == me) continue;
        if (t->info.state == TASK_FREE || t->info.state == TASK_DONE) continue;
        if (t->app_mem_set && t->app_mem.text == text) return t->info.pid;
    }
    return -1;
}

bool task_app_mem_get(TaskAppMem *out) {
    Task *t = cur();
    if (!t || !t->app_mem_set) return false;
    if (out) *out = t->app_mem;
    return true;
}

Arena *task_arena(void) {
    Task *t = cur();
    return t ? t->arena : nullptr;
}

void task_arena_set(Arena *a) {
    Task *t = cur();
    if (t) t->arena = a;
}

void task_app_mem_clear(void) {
    Task *t = cur();
    // The hardware is cleared whether or not a task was found. Leaving regions
    // programmed after the package they describe has been freed is the failure
    // this is here to prevent, and "there was no current task" is not a reason
    // to leave them in place.
    task_app_mem_apply(nullptr);
    if (!t) return;
    t->app_mem_set = false;
}
