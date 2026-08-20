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
#include "hardware/timer.h"
#include "hardware/irq.h"
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
void intr_input_task_ended(int slot);

void task_slot_recycled(int slot) {
    sandbox_forget(slot);
    apps_task_ended(slot);
    // A listener left bound holds its port until the next reboot, and a
    // half-open connection holds a PCB nobody will collect. Neither is the
    // package author's to clean up after their task has already gone.
    net_tcp_task_ended(slot);
    // Same argument for the console. A task that claimed it and then died —
    // faulted, killed, or simply returned — would otherwise own the input
    // stream until a reboot, with nothing able to take it back and no way to
    // see who was holding it. The claim goes where its owner went.
    intr_input_task_ended(slot);
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
// Sixteen, not eight, and the reason is that the old premise stopped being
// true. The comment above says the longest legitimate gap between yields is a
// flash erase-and-write at under 200 ms. That was written before this OS could
// speak TLS.
//
// A handshake is driven from the cyw43 background context, which is an
// interrupt on core 0 — and core 0 is the only core that feeds this. While the
// crypto runs there nothing reaches the scheduler, so the gap is however long
// the handshake takes, and no timeout in the calling task can shorten it
// because the task is not running either.
//
// The curve list is cut down so that gap should now be well under a second.
// This is the margin for the case where it is not: a slow handshake should be
// a slow connection, not a reboot.
#define WATCHDOG_MS   16000
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
// OWNING THE INTERRUPT, which is the whole of this.
//
// The previous version hung off add_alarm_in_us, and an SDK alarm callback runs
// several frames below the exception: by the time it is entered the timer IRQ
// has been taken, the alarm pool's dispatcher has been called, and the callback
// has its own frame. So `mov r0, lr` reads an ordinary return address rather
// than EXC_RETURN, and `mrs r0, msp` gives the callback's stack pointer rather
// than the value MSP had at exception entry.
//
// It therefore computed a "frame" pointing into the dispatcher's locals and
// wrote a program counter into word six of it. `havoc spin` came back as
// pc=0x11020202 with an UNALIGNED fault — the handler returning into its own
// corrupted stack. The mechanism had been DEVICE-UNCONFIRMED since it was
// written; that made it device-disproven.
//
// A naked handler on the alarm's own IRQ vector is entered BY the exception,
// with nothing between. LR is EXC_RETURN and MSP still points at the frame the
// hardware pushed, because nothing has pushed anything yet. That is the only
// place those two are true.
static int g_alarm = -1;
// One intervention per stall. Without it the tick would keep rewriting a frame
// it has already redirected.
static bool g_acted;

extern "C" void preempt_tick(uint32_t *frame);

// Read MSP or PSP BEFORE pushing anything, hand the frame to C, then return
// through the EXC_RETURN that was saved. r4 rides along only to keep the stack
// eight-byte aligned across the call, which AAPCS requires.
__attribute__((naked)) void isr_preempt(void) {
    __asm volatile(
        "movs r0, #4            \n"
        "mov  r1, lr            \n"
        "tst  r0, r1            \n"
        "beq  1f                \n"
        "mrs  r0, psp           \n"
        "b    2f                \n"
        "1:                     \n"
        "mrs  r0, msp           \n"
        "2:                     \n"
        "push {r4, lr}          \n"
        "bl   preempt_tick      \n"
        "pop  {r4, pc}          \n"
        ".align 2               \n"
    );
}

// NOTHING HERE PRINTS OR FORMATS.
//
// This runs on the interrupted task's stack, and vsnprintf alone wants over a
// kilobyte of it — on a task that may already be deep, in an interrupt, which
// is the stack overflow this whole mechanism exists to survive rather than
// cause. Both outcomes already report from task context: task_forced_exit
// says what it terminated, and apps.cpp says when a call was taken back.
extern "C" void preempt_tick(uint32_t *frame) {
    // Cleared and re-armed FIRST, so however the decision below goes the tick
    // keeps coming.
    if (g_alarm >= 0) {
        timer_hw->intr = 1u << g_alarm;
        timer_hw->alarm[g_alarm] = time_us_32() + PREEMPT_TICK_US;
    }

    uint32_t stall = bb_stall_ms(task_now_ms());
    // How long core 0 was ever held, recorded whether or not anything is done
    // about it. This is the other half of naming a hang: if the tick was still
    // arriving while the device was dead, the core was running and refusing to
    // yield; if the largest stall ever recorded is a normal one, the core
    // stopped taking interrupts at all and the cause is a spinlock. Two plain
    // stores, and no formatting — see the note above.
    // frame[6] is the stacked PC — the layout the redirect below relies on.
    bb_note_stall(stall, crit_active(), frame ? frame[6] : 0);
    if (stall < STALL_KILL_MS) { g_acted = false; return; }
    if (g_acted) return;

    // WHICH OF THE THREE ANSWERS THIS IS, decided in core/preempt.cpp so the
    // combinations can be built directly in a host test. What is left here is
    // reading the facts and carrying the answer out.
    //
    // ALL FOUR ARE READ EAGERLY, INCLUDING should_force, which the old chain
    // only reached when the abandon had already failed. That is safe and it is
    // worth saying why, because it is the first thing to check here: everything
    // it consults is lock-free. task_crit_active reads one field of the current
    // task, task_count scans the table, task_current reads a memo, and
    // preempt_decide is arithmetic. None of them touches the hardware spinlock
    // — which from an interrupt, on a core that already holds it, is the
    // permanent stop lock_hw_enter warns about rather than a wait.
    StallFacts f{};
    f.past_kill        = true;
    f.in_package       = task_in_package();
    // Not the same question. `in_package` is whose code is running, which is
    // answerable on every part. This is whether there is a supervisor call to
    // return from differently — false for the whole of ARMv6-M, where a package
    // branches straight into the firmware and there is no call to take back.
    f.call_reclaimable = sandbox_in_package();
    f.may_end_task     = should_force();

    StallAction act = stall_decide(f);
    if (act == STALL_LEAVE) return;

    // A TASK WEDGED INSIDE A PACKAGE LOSES THE CALL, NOT THE TASK.
    //
    // This is the case that matters, because a package command runs on the
    // SHELL task — so ending the task would take the session with it, which is
    // why the shell is exempt from being forced at all. The unwind is the one
    // `svc #1` performs when a package returns normally: the exception return
    // goes into the shim's tail, app_call_unpriv returns, and its caller
    // carries on into sandbox_return and app_leave exactly as it would have.
    if (act == STALL_ABANDON_CALL) {
        if (sandbox_abandon_call(frame)) {
            g_acted = true;
            // THE STALL IS OVER, so say so. Without this the watchdog reports
            // it again a moment later — "stalled 6097 ms at 'havoc returned'" —
            // which describes a problem that has just been dealt with and reads
            // as a second failure.
            bb_note_yield(task_now_ms());
            return;             // apps.cpp says so, back in task context
        }
        // It refused: there is no parked firmware stack pointer to return onto,
        // so the exception return would land on a stack pointer nobody chose.
        // Ask the task-level question instead, which is what this path did
        // before the decision was written down.
        act = f.may_end_task ? STALL_END_TASK : STALL_NO_RECOURSE;
    }

    if (act == STALL_END_TASK) {
        g_acted = true;
        bb_note_yield(task_now_ms());   // dealt with; see above
        exc_frame_redirect(frame, (uint32_t)(uintptr_t)&task_forced_exit);
        return;
    }

    // STALL_NO_RECOURSE — the watchdog is going to reboot this device in a few
    // seconds and there is nothing here that can stop it. THE FRAME IS NOT
    // TOUCHED: every mechanism above has been tried and declined, and inventing
    // an unwind at this point would be aiming an exception return at a stack
    // that nothing has prepared.
    //
    // What is left is to make the reboot an explained one. A single byte into
    // the black box, which survives the reset, so the next boot can say the OS
    // saw this and had nothing left — rather than leaving a bare watchdog
    // reset that reads as if nobody noticed. g_acted is deliberately not set:
    // the tick keeps asking, because should_force can start saying yes.
    bb_note_stuck(f.in_package ? BB_STUCK_PACKAGE : BB_STUCK_TASK);
}

void task_preempt_start(void) {
    // Core 0 only, deliberately. The stall clock is fed by bb_note_yield, which
    // only core 0 calls, so core 0 is the only core whose idea of "nothing has
    // yielded" means anything. A wedge on core 1 is not visible to this and is
    // not pretended to be.
    g_alarm = hardware_alarm_claim_unused(false);
    if (g_alarm < 0) return;              // no spare alarm: no preemption, no harm

    uint irq = timer_hardware_alarm_get_irq_num(timer_hw, (uint)g_alarm);
    irq_set_exclusive_handler(irq, isr_preempt);
    irq_set_enabled(irq, true);

    timer_hw->inte |= 1u << g_alarm;
    timer_hw->alarm[g_alarm] = time_us_32() + PREEMPT_TICK_US;
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
    //
    // The phase note is kept for a reader looking at the console, but it is NOT
    // what makes this visible: `phase` is one shared slot and fw_millis writes
    // it on every ABI call, so the OTHER core — which is still running, because
    // only this one has stopped — overwrites the note within a millisecond.
    // Every hang this guard was meant to name came back as "entered fw_millis".
    // bb_note_hw_twice has no other writer, and it carries the return address
    // of whoever asked, which is the only way to find the path.
    if (g_hw_held[c]) {
        bb_note_phase("hw lock taken twice - deadlock");
        bb_note_hw_twice((uint8_t)c, (uint32_t)(uintptr_t)__builtin_return_address(0));
    }

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
