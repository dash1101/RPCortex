// The scheduler's platform seam on RP2040 / RP2350.
//
// task_ctx_switch is in task_switch.S; everything else it needs is here.

#include "task.h"
#include "preempt.h"
#include "excframe.h"
#include "lock.h"
#include "mpu.h"
#include "sandbox.h"

#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "lock.h"
#include "logring.h"
#include "blackbox.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "out.h"
#include <stdio.h>

// The frame task_ctx_switch pops: nine words, of which only the last matters.
//
// The eight below it land in r4-r11 (in a different order on ARMv6-M, which is
// exactly why their VALUES are don't-care — a task starts with garbage in its
// callee-saved registers, and the first thing any compiled function does is
// establish its own). The ninth is popped into PC, so it is the entry point.
//
// Sizing: sp starts at top-36 and the pop of nine words leaves it at `top`,
// which is 8-byte aligned as AAPCS requires at a function boundary. It is
// transiently unaligned during the pop itself, which is allowed.
#define CTX_WORDS 9

void *task_ctx_init(void *stack_top, TaskEntry entry) {
    uint32_t *sp = (uint32_t *)stack_top - CTX_WORDS;
    memset(sp, 0, CTX_WORDS * 4);
    // Thumb bit: POP into PC performs an interworking branch and faults if bit 0
    // is clear. A C function pointer already carries it, but the cast is where
    // that would silently be lost, so it is set explicitly.
    sp[CTX_WORDS - 1] = (uint32_t)(uintptr_t)entry | 1u;
    return sp;
}

uint32_t task_now_us(void) { return time_us_32(); }

extern "C" void apps_task_ended(int pid);

// A task slot is being reused, so anything held against the old occupant has to
// go: the sandbox state, and the stack and heap the package pool was keeping
// for it. Missing either leaves memory owned by a task that no longer exists.
extern "C" unsigned task_irq_save(void) {
    uint32_t primask;
    __asm volatile ("mrs %0, primask \n cpsid i" : "=r"(primask) :: "memory");
    return primask;
}
extern "C" void task_irq_restore(unsigned state) {
    if (!state) __asm volatile ("cpsie i" ::: "memory");
}

extern "C" void net_tcp_task_ended(int slot);

void task_slot_recycled(int slot) {
    sandbox_forget(slot);
    apps_task_ended(slot);
    // A listener left bound holds its port until the next reboot, and a
    // half-open connection holds a PCB nobody will collect. Neither is the
    // package author's to clean up after their task has already gone.
    net_tcp_task_ended(slot);
}

uint32_t task_now_ms(void) {
    return (uint32_t)(time_us_64() / 1000ull);
}

// --- the watchdog -----------------------------------------------------------
//
// Fed from the scheduler, which runs constantly: every yield, every sleep, every
// interrupt check. If nothing yields for WATCHDOG_MS the device has stopped
// making progress — a deadlock, a runaway loop, a task that will never come back
// — and rebooting is strictly better than sitting there dark until someone
// unplugs it.
//
// Generous on purpose. The longest legitimate gap between yields is a flash
// erase-and-write, which is under 200 ms; 8 seconds cannot be hit by anything
// that is actually working.
// The hardware watchdog is the LAST resort, not the only one. Before it fires,
// two softer stages get a chance to save the session:
//
//   3 s   say which task has stopped yielding, and ask it to stop. A task stuck
//         in a loop that checks task_should_stop — which every loop in this OS
//         does, because intr_check folds it in — will exit here and the shell
//         carries on as if nothing happened.
//   6 s   it ignored the request. Say so, and let the hardware take it.
//   8 s   the hardware watchdog reboots.
//
// The point is that a wedged COMMAND should cost you that command, not the
// device. Only a wedged kernel should cost a reboot.
//
// A task that never yields at all cannot be asked anything, and that is what
// forced exit below is for. It does NOT need the process stack and PendSV that
// a general preemptive scheduler needs, because a task being killed is never
// resumed — so its registers do not have to be preserved, and none of the
// machinery that exists to preserve them is required.
//
// What it needs instead is one write. When an interrupt fires, the hardware has
// already pushed the interrupted PC onto the stack; changing that word makes
// the task resume somewhere else when the handler returns. Pointing it at a
// routine that marks the task dead and reschedules turns a runaway loop into a
// terminated command, using the task's own stack and the existing cooperative
// switch.
//
// The whole mechanism is a stack write and an ordinary function. That is a very
// different amount of risk from re-basing every task onto PSP, and it buys the
// thing that was actually wanted: a wedged package costs that package.
#define PREEMPT_TICK_US  100000     // 100 ms; fine enough against a 6 s threshold
#define WATCHDOG_MS   8000
#define STALL_WARN_MS 3000
#define STALL_KILL_MS 6000

static bool     g_wd_on;
static uint32_t g_last_stage;      // which escalation has already been reported
static bool     g_named;           // the stalling task has been recorded once

void task_watchdog_start(void) {
    watchdog_enable(WATCHDOG_MS, /*pause_on_debug*/true);
    g_wd_on = true;
}

// Called from the scheduler on core 0. Feeding is only half of it: the other
// half is noticing that the task holding the core has stopped making progress
// and doing something about it before the reboot.
void task_watchdog_feed(void) {
    if (!g_wd_on) return;
    watchdog_update();

    uint32_t stall = bb_stall_ms(task_now_ms());
    if (stall < STALL_WARN_MS) { g_last_stage = 0; g_named = false; return; }

    const TaskInfo *t = task_current();

    // SAY WHO, WHATEVER IT IS. This used to return here for the idle task and
    // the shell, before recording anything — so the one case that matters most
    // produced a completely silent reboot. Three rounds of a hang inside a
    // package's HTTPS fetch were debugged with an empty logdump for exactly
    // that reason: the stalling task WAS the shell, so nothing was written.
    //
    // Killing it is still off the table for those two. Naming it is not.
    // The PHASE IS NOT TOUCHED. Writing a note here would overwrite the one
    // thing worth having — where the machine had got to — with the fact that it
    // stopped, which the reboot reason already says. It goes to the log ring
    // instead, which survives the reset and is what logdump prints.
    if (!g_named) {
        g_named = true;
        log_addf(LOG_K_ERR, "watchdog: '%s' pid %d core %u stalled %u ms at '%s'",
                 t ? t->name : "?", t ? t->pid : -1, (unsigned)get_core_num(),
                 (unsigned)stall, bb_phase());
    }

    // Never force the idle task or the shell to exit — asking the shell to stop
    // set a flag nothing cleared, which silently broke every later scan and ping.
    if (!t || t->pid == 1) return;
    if (t->pid == task_shell_pid()) return;

    if (stall >= STALL_KILL_MS && g_last_stage < 2) {
        g_last_stage = 2;
        out_errp("watchdog", "'%s' (pid %d) has not responded for %u s.",
                 t->name, t->pid, (unsigned)(stall / 1000));
        out_multi("  It ignored the request to stop. Rebooting shortly.");
        log_addf(LOG_K_ERR, "watchdog: '%s' pid %d unresponsive %u ms",
                 t->name, t->pid, (unsigned)stall);
        return;
    }

    if (g_last_stage < 1) {
        g_last_stage = 1;
        out_warnp("watchdog", "'%s' (pid %d) has not yielded for %u s — asking it to stop.",
                  t->name, t->pid, (unsigned)(stall / 1000));
        log_addf(LOG_K_WARN, "watchdog: asking '%s' pid %d to stop after %u ms",
                 t->name, t->pid, (unsigned)stall);
        task_kill(t->pid);
    }
}

// --- forced exit ------------------------------------------------------------
//
// DEVICE-UNCONFIRMED. The policy this consults is host-tested; the stack write
// below is ARM and can only be proven on hardware.

// Where a forced task resumes. Runs on the task's OWN stack, in thread mode,
// as an ordinary function — the interrupt has already returned by the time this
// executes, so everything here is allowed to yield, log and allocate.
//
// It never returns. task_exit does not come back, and reaching the end of this
// would return into a stack frame that has been abandoned.
extern "C" void task_forced_exit(void) {
    // Whatever this task was inside, it is not inside it any more. The alarm
    // already handed privilege back before redirecting here; this clears the
    // bookkeeping so a reused slot does not inherit it.
    sandbox_release_for_kill();
    const TaskInfo *t = task_current();
    if (t) {
        out_errp("watchdog", "'%s' (pid %d) would not stop. Terminated.", t->name, t->pid);
        log_addf(LOG_K_ERR, "watchdog: force-terminated '%s' pid %d", t->name, t->pid);
    }
    task_exit(1);
    for (;;) task_yield();     // unreachable; task_exit does not return
}

// Redirect the interrupted task to task_forced_exit by rewriting the PC the
// hardware stacked on exception entry.
//
// The frame the hardware pushed is r0, r1, r2, r3, r12, LR, PC, xPSR, so PC is
// word 6 and xPSR word 7.
//
// xPSR needs care on ARMv8-M. If the interrupt landed inside an IT block, the
// stacked xPSR carries the condition bits for the instructions that were about
// to run; returning with those set would execute the first instructions of
// task_forced_exit under a stale predicate, possibly skipping them. Clearing
// them costs nothing on ARMv6-M, which has no IT block at all.
//
// Bit 24 (Thumb) is preserved rather than assumed: an exception return with it
// clear faults in a way that looks nothing like its cause.
// The layout, the Thumb bit and the IT clearing all live in core/excframe.cpp
// and are host-tested there. What is left here is reading two registers.

// Should the task holding this core be terminated where it stands?
static bool should_force(void) {
    const TaskInfo *t = task_current();
    if (!t || t->pid == 1) return false;              // the idle task
    if (t->pid == task_shell_pid()) return false;    // and never the shell

    uint32_t now = task_now_ms();
    PreemptState ps{};
    ps.now_ms        = now;
    // bb_stall_ms already handles the pre-reset case; expressing it back as a
    // timestamp keeps one definition of "how long since it yielded".
    ps.last_yield_ms = now - bb_stall_ms(now);
    ps.enabled       = true;
    ps.in_critical   = crit_active();
    // More than one task in play. task_count includes DONE slots, so this is a
    // deliberately loose test: forcing when there is genuinely nothing else to
    // run gains nothing, and the watchdog still covers that case.
    ps.runnable_peer = task_count() > 1;
    ps.is_idle_task  = false;
    // Only after asking politely has already failed. Stage 1 sets a stop flag
    // that every loop in this OS checks; forcing before that would terminate
    // tasks that were about to stop on their own.
    ps.slice_ms      = STALL_KILL_MS;

    return preempt_decide(ps) == PREEMPT_SWITCH;
}

// The alarm handler. Deliberately does almost nothing: it decides, writes one
// word, and returns. Printing or allocating from an interrupt that fired inside
// an arbitrary instruction is how a fault handler comes to fault.
static int64_t preempt_alarm(alarm_id_t, void *) {
    if (should_force()) {
        // If the stalled task is a SANDBOXED package, it is running
        // unprivileged and cannot fetch instructions from flash — so redirecting
        // it into task_forced_exit would fault rather than terminate it, and
        // report a protection violation in a task that was already being killed.
        // Privilege is handed back first. The task is ending either way, so it
        // has nothing left to protect.
        if (sandbox_in_package()) sandbox_release_for_kill();
        // EXC_RETURN bit 2 says which stack the interrupted code was using.
        // Both are checked rather than assumed, because assuming MSP would
        // silently rewrite the wrong stack the day tasks move to PSP.
        uint32_t exc_return;
        __asm volatile ("mov %0, lr" : "=r"(exc_return));
        uint32_t *frame;
        if (exc_return_used_psp(exc_return)) __asm volatile ("mrs %0, psp" : "=r"(frame));
        else                                 __asm volatile ("mrs %0, msp" : "=r"(frame));
        exc_frame_redirect(frame, (uint32_t)(uintptr_t)&task_forced_exit);
    }
    return PREEMPT_TICK_US;    // reschedule this alarm
}

void task_preempt_start(void) {
    add_alarm_in_us(PREEMPT_TICK_US, preempt_alarm, nullptr, /*fire_if_past*/true);
}

// --- stack overflow ---------------------------------------------------------

// pid 1 runs on the C startup stack, which this scheduler did not allocate and
// therefore cannot paint. The linker knows where it ends, so the check is
// against that: how much room is left below the current stack pointer.
extern "C" char __StackBottom;

extern "C" char __StackTop;

uint32_t task_main_stack_size(void) {
    return (uint32_t)((uintptr_t)&__StackTop - (uintptr_t)&__StackBottom);
}

uint32_t task_main_stack_headroom(void) {
    uint32_t sp;
    __asm volatile ("mov %0, sp" : "=r" (sp));
    uint32_t floor = (uint32_t)(uintptr_t)&__StackBottom;
    return sp > floor ? sp - floor : 0;
}

// The high-water mark of the boot stack, by the same fill-pattern trick every
// spawned stack uses.
//
// This one was never measured, and it is the one that matters most. The boot
// stack is 4 KB in SCRATCH_Y and cannot be made larger, the whole boot sequence
// runs on it, and __sbprintf alone wants 1128 bytes of that. Below it sits
// core 1's stack — __StackBottom and __StackOneTop are the same address — so an
// overflow here does not land in spare memory, it lands in the other
// processor's frames.
//
// It also decides whether arming a hardware guard at __StackBottom is safe. A
// guard is only ever as good as the headroom above it: set one on a stack that
// was already running close to the edge and the result is not protection, it is
// a device that faults during boot. So the number is measured and printed
// rather than assumed, and `mpu` is where to read it.
#define MAIN_STACK_PAINT 0xA5A5A5A5u

void task_main_stack_paint(void) {
    uint32_t sp;
    __asm volatile ("mov %0, sp" : "=r" (sp));
    uint32_t floor = (uint32_t)(uintptr_t)&__StackBottom;
    // Stop well short of the current frame. This runs from main, so everything
    // below sp is fair game — but the margin costs nothing and painting over a
    // live frame would not be recoverable.
    if (sp < floor + 64) return;
    uint32_t *p = (uint32_t *)(uintptr_t)floor;
    uint32_t words = (sp - 64 - floor) / 4;
    for (uint32_t i = 0; i < words; i++) p[i] = MAIN_STACK_PAINT;
}

uint32_t task_main_stack_used(void) {
    const uint32_t *p = (const uint32_t *)(uintptr_t)&__StackBottom;
    uint32_t words = task_main_stack_size() / 4;
    uint32_t untouched = 0;
    while (untouched < words && p[untouched] == MAIN_STACK_PAINT) untouched++;
    // Zero means the paint never ran, not that the whole stack is in use — the
    // difference matters, because reporting a full stack on a healthy device is
    // the kind of diagnostic that invents a problem.
    if (untouched == 0) return 0;
    return task_main_stack_size() - untouched * 4;
}

void task_stack_overflow(const char *name, uint32_t size) {
    // Interrupts stay on: this needs USB to deliver the message, and the damage
    // is already done so there is nothing left to protect.
    //
    // Unsynchronised from here, because this is reached from inside reschedule
    // and the other core may be holding the output lock — or may be the reason
    // this task's stack is gone. Interleaved text beats a silent hang.
    out_panic_mode();
    printf("\n");
    out_fatal("STACK OVERFLOW in task '%s'", name ? name : "?");
    out_multi("   It used more than its %u byte stack and has written past the end.",
              (unsigned)size);
    out_multi("   Memory below it is corrupt, so continuing would put the damage");
    out_multi("   somewhere it would be blamed on something else.");
    out_multi("   Rebooting.");
    log_addf(LOG_K_ERR, "stack overflow in '%s' (%u bytes)", name ? name : "?",
             (unsigned)size);
    for (int i = 0; i < 60; i++) { sleep_ms(10); fflush(stdout); }
    watchdog_reboot(0, 0, 0);
    while (true) { tight_loop_contents(); }
}

uint32_t task_this_core(void) {
    return get_core_num();
}

static volatile bool g_core1_up;

uint32_t task_core_count(void) {
    return g_core1_up ? 2 : 1;
}

// --- the cross-core critical section ----------------------------------------
//
// One of the RP2's hardware spinlocks. It is held for a handful of instructions
// inside lock_try/lock_release and never across a yield, so it cannot be the
// thing that stalls anything. Interrupts are masked while it is held, because
// an interrupt handler that touched the same spinlock on the same core would
// deadlock against itself.
// Per core. spin_lock_blocking returns the interrupt state to restore, and the
// lock serialises the holders so one shared word happened to work — but a saved
// interrupt state belongs to the core that saved it, and relying on the lock to
// keep that straight is a trap for the next person to nest one of these.
static uint32_t g_hw_save[2];
static spin_lock_t *g_hw;

extern "C" unsigned lock_hw_core(void) { return get_core_num(); }

// Claimed once, before core 1 exists. Claiming it lazily on first use meant two
// cores could reach the check together and claim two DIFFERENT locks, each
// happily excluding nobody. In practice core 0 is long past first use before
// core 1 starts, so it never fired — but "never fired" and "cannot fire" are
// different things, and this is the lock everything else is built on.
extern "C" void lock_hw_init(void) {
    if (!g_hw) g_hw = spin_lock_instance(spin_lock_claim_unused(true));
}

// Whether THIS core is already inside. Only ever read and written by its own
// core, so it needs no protection of its own.
static volatile bool g_hw_held[2];

extern "C" void lock_hw_enter(void) {
    if (!g_hw) lock_hw_init();          // fallback: a lock taken before init
    unsigned c = get_core_num() & 1;

    // TAKING IT TWICE ON ONE CORE IS A HANG, NOT A WAIT.
    //
    // This is a hardware spinlock and it is not recursive, so a core that asks
    // for it while already holding it spins against itself forever — with
    // interrupts masked, so nothing else on that core can run either. There is
    // no fault and no output: the board simply stops, and the watchdog reboots
    // it some seconds later with nothing at all to say about why.
    //
    // That is not hypothetical. The sandbox pool was given this lock, and the
    // scheduler calls the pool's slot-recycled hook while already holding it —
    // one flash cycle spent on a silent reboot to find a two-line cause.
    //
    // The note is written before blocking, into memory a reset does not clear,
    // so the next boot names it. It costs one byte and a branch on a path that
    // should never be taken.
    if (g_hw_held[c]) bb_note_phase("hw lock taken twice - deadlock");

    g_hw_save[c] = spin_lock_blocking(g_hw);
    g_hw_held[c] = true;
}

extern "C" void lock_hw_exit(void) {
    if (!g_hw) return;
    unsigned c = get_core_num() & 1;
    g_hw_held[c] = false;
    spin_unlock(g_hw, g_hw_save[c]);
}

// --- core 1 -----------------------------------------------------------------
//
// Core 1 runs the same scheduler. It has no task of its own to start with, so
// it parks in a loop that yields: reschedule() picks any task whose affinity
// allows core 1, runs it, and comes back here when that task yields. There is
// no separate "core 1 task list" — one table, two cores taking from it.
static void core1_main(void) {
    // Its own memory protection. The two processors do not share any of it, so
    // a core that never runs this has no stack guard and no package regions —
    // and since an unpinned task runs on core 1 almost all the time, that would
    // be nearly all of them.
    mpu_platform_init();
    // And arm the guard for the stack this loop is standing on. Without it,
    // core 1 has no stack limit at all until the first task happens to be
    // scheduled here — which on a quiet device can be a long time, and which
    // `mpu` reported honestly as a guard address of zero.
    task_stack_guard_set(nullptr, 0);

    // Register with the flash subsystem BEFORE running anything. Without this
    // flash_safe_execute cannot park this core, and every filesystem write from
    // core 0 would be refused — or, worse, done anyway.
    flash_safe_execute_core_init();

    while (true) {
        task_yield();
        // Nothing runnable for this core right now. Sleeping briefly rather
        // than spinning keeps it off the memory bus, which core 0 is using.
        busy_wait_us(200);
    }
}

void task_start_core1(void) {
    if (g_core1_up) return;
    // The flag is set BEFORE launching, so that by the time core 1 first calls
    // task_yield the scheduler already knows there are two cores. Setting it
    // after would leave a window where core 1 is running but reschedule still
    // believes it is alone.
    g_core1_up = true;
    multicore_launch_core1(core1_main);
}
