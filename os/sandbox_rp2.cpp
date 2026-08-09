// Running a package without the OS's own privileges.
//
// A sandboxed package executes in unprivileged Thread mode with the memory
// protection describing exactly five regions — its code, its data, its veneers,
// its stack and its heap. Everything else in the machine is denied, because
// unprivileged accesses get no default memory map when the MPU is on. Not the
// OS's tables, not the peripherals, and not flash.
//
// Denying flash is what makes this work, and it is also what makes it awkward:
// the package cannot branch into the firmware, so every ABI call has to be a
// supervisor call. The loader builds veneers that carry the function's INDEX
// and execute SVC; this file is what answers them.
//
// --- the two directions are deliberately not symmetric -----------------------
//
// RAISING privilege needs an exception; there is no other way. DROPPING it is
// one instruction. So an ABI call costs one exception on the way in and a
// handful of register moves on the way out, rather than two exceptions.
//
// The awkward part of the cheap direction is that the instruction AFTER `msr
// CONTROL` runs unprivileged, and everything in this file is in flash. So the
// last two instructions of the return path live in the package's veneer pool,
// which the loader wrote and which is read-only to the package. A package can
// jump straight to that gate with registers of its choosing and gains nothing:
// a write to CONTROL from unprivileged Thread mode is ignored by the
// architecture, so it cannot raise itself.
//
// --- ARMv8-M only ------------------------------------------------------------
//
// ARMv6-M regions are power-of-two sized and aligned to their own size, so the
// five regions above would cost more RAM than an RP2040 has. There, packages
// run privileged exactly as they did before, and this file compiles to nothing.
#include "core/mpu.h"
#include "core/task.h"
#include "core/excframe.h"
#include "sandbox.h"
#include "api.h"
#include "loader.h"
#include "core/blackbox.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"

#if defined(__ARM_ARCH_6M__) || PICO_RP2040
  #define SANDBOX_SUPPORTED 0
#else
  #define SANDBOX_SUPPORTED 1
#endif

bool sandbox_supported(void) { return SANDBOX_SUPPORTED; }

#if SANDBOX_SUPPORTED

// How many supervisor calls have been served, and how many were refused.
// Refusals are the interesting number: every one is a package that asked for a
// function index the firmware does not export, which is either a corrupted
// veneer pool or a package doing something it was never built to do.
static uint32_t g_calls, g_refused;

void sandbox_counts(uint32_t *calls, uint32_t *refused) {
    if (calls)   *calls = g_calls;
    if (refused) *refused = g_refused;
}

// --- CONTROL -----------------------------------------------------------------
//
// Read-modify-write, never a constant. CONTROL also carries SPSEL (which stack
// Thread mode uses) and FPCA (whether this context has live floating-point
// registers). RP2350's Cortex-M33 has an FPU and the build uses it, so writing
// a literal 1 here would clear FPCA under a task in the middle of floating-point
// work — and the registers would silently stop being preserved across the next
// exception.
#define CONTROL_NPRIV  1u

// --- where the package's stack guard goes ------------------------------------
//
// Not at the bottom. A stack limit that fires with no room left to TAKE the
// exception is not a guard: the violation is reported by an exception, the
// exception pushes a frame below the stack pointer, and if that frame does not
// fit the fault becomes MSTKERR — "the frame could not be pushed" — which is
// the one fault that cannot be contained, because there is then nothing to read
// or rewrite.
//
// This build has an FPU, so the frame is the extended one: 0x68 bytes, plus up
// to four more if the hardware has to realign. 128 rounds that up and leaves a
// little over.
#define PKG_STACK_GUARD_BAND  128u

// Rounded up as well, because MSPLIM's low three bits are RES0 — an unaligned
// value would quietly set the limit BELOW the intended one.
static inline uint32_t guard_for(const void *base) {
    uint32_t b = (uint32_t)(uintptr_t)base + PKG_STACK_GUARD_BAND;
    return (b + 7u) & ~7u;
}

static inline uint32_t control_get(void) {
    uint32_t v;
    __asm volatile ("mrs %0, control" : "=r"(v));
    return v;
}

static inline void control_set(uint32_t v) {
    __asm volatile ("msr control, %0" :: "r"(v) : "memory");
    __asm volatile ("isb");
}

// --- the supervisor call handler ---------------------------------------------

// Where a package that is mid-ABI-call will resume, and the stack the firmware
// was on when it entered package code. Both are per task, because an ABI call
// can yield: fw_printf reaches task_alive, which can hand the core to somebody
// else and come back much later.
struct SandboxState {
    uint32_t package_lr;     // where to return inside the package
    uint32_t return_gate;    // the drop-privilege gate in its veneer pool
    uint32_t kernel_sp;      // the firmware stack app_call_unpriv left behind
    // The stack the package is running ON, so the guard can be armed against
    // the right one.
    //
    // Without this the scheduler re-armed MSPLIM to the TASK's stack every time
    // a sandboxed task resumed, while its stack pointer was somewhere else
    // entirely — the sandbox stack, a separate allocation. Whether that faulted
    // came down to which of the two the heap had put lower, so it was
    // intermittent, it moved with the heap layout, and it looked like a stack
    // overflow in a task that had plenty left.
    void    *stack_base;
    uint32_t stack_size;
    uint8_t  depth;          // 0 when this task is not inside a package
};

// One per task. Indexed by the scheduler rather than kept per core: a package
// yields, and the core it comes back on is usually not the one it left.
static SandboxState g_state[TASK_MAX];

static inline SandboxState *state_of_current(void) {
    int slot = task_slot_index();
    if (slot < 0 || slot >= (int)TASK_MAX) return nullptr;
    return &g_state[slot];
}

// Reached from `syscall_return`, below, once the firmware function has finished.
// Returns the value the gate needs in r3.
extern "C" uint32_t sandbox_finish_call(void) {
    SandboxState *s = state_of_current();
    if (!s) return 0;
    return s->package_lr;
}

extern "C" uint32_t sandbox_return_gate(void) {
    SandboxState *s = state_of_current();
    return s ? s->return_gate : 0;
}

// Where app_call_unpriv parked the firmware's stack pointer. Asked for from the
// tail, which arrives back on the PACKAGE's stack and has to stand on the
// firmware's again before it can pop the frame it pushed.
extern "C" uint32_t sandbox_kernel_sp(void) {
    SandboxState *s = state_of_current();
    return s ? s->kernel_sp : 0;
}

// The C half of the handler. `frame` is the eight words the hardware pushed.
//
// `svc #0` — a package asking for an ABI function by index.
// `svc #1` — a package's app_main or command has returned.
extern "C" void sandbox_svc(uint32_t *frame, uint32_t which) {
    SandboxState *s = state_of_current();

    if (which == 1) {
        // The package is done. Send the exception return into the shim's tail,
        // which puts the firmware's own stack back; privilege is restored here
        // because everything from that point on is firmware.
        if (s) s->depth = 0;
        control_set(control_get() & ~CONTROL_NPRIV);
        exc_frame_redirect(frame, (uint32_t)(uintptr_t)&app_call_unpriv_tail);
        return;
    }

    // An ABI call. The index is entirely under the package's control, which is
    // exactly why api_addr_at bounds-checks it — a refused index leaves the
    // frame untouched and the package takes the fault at its own SVC rather
    // than at whatever address happened to follow the table.
    g_calls++;
    uint32_t index = exc_frame_syscall_index(frame);
    uint32_t target = api_addr_at(index);
    uint32_t saved = 0;

    // Not every caller is sandboxed, and the ones that are not must not be sent
    // home through the gate.
    //
    // A package can spawn a task, and that task runs the package's code — which
    // contains supervisor-call veneers, because that is the only kind the
    // loader built — but it never went through sandbox_enter, so it has no
    // return gate and no saved return address. Sending it to
    // sandbox_syscall_return anyway meant reading two zeroes and branching to
    // address zero: `INVSTATE no Thumb bit`, pc=0, in a task whose own stack
    // had not been touched. Reported from a board twice, from stress and probe,
    // both of which spawn helpers.
    //
    // Such a task is already privileged, so there is nothing to raise and
    // nothing to hand back. Leave the stacked LR exactly as the package set it
    // and the firmware function returns straight to the caller.
    bool boxed = s && s->depth;

    if (exc_frame_enter_firmware(frame, target,
                                 boxed ? (uint32_t)(uintptr_t)&sandbox_syscall_return
                                       : frame[EXC_LR_WORD],
                                 &saved) != SYSCALL_OK) {
        g_refused++;
        return;                    // nothing written: it faults where it asked
    }
    if (!boxed) return;            // privileged already; no gate, no CONTROL

    s->package_lr = saved;
    // Privileged from the exception return until sandbox_syscall_return hands
    // it back. The firmware function runs on the package's stack, which is why
    // that stack is sized like any other task's rather than like a scratch area.
    control_set(control_get() & ~CONTROL_NPRIV);
}

// --- entering package code ---------------------------------------------------

int sandbox_enter(void *fn, int arg0, void *arg1, void *stack_top,
                  uint32_t return_gate, uint32_t enter_gate, uint32_t exit_gate,
                  uint32_t stack_size, uint32_t pic_base) {
    SandboxState *s = state_of_current();
    if (!s || !fn || !stack_top || !return_gate || !enter_gate || !exit_gate) return -1;

    // ONE PACKAGE CALL PER TASK AT A TIME.
    //
    // This state is the task's only record of how to get back out of a package:
    // the return gate, the saved firmware stack pointer, the stack band the
    // guard is armed on. Entering again on the same task OVERWRITES all of it,
    // and the exit clears depth to zero — so the outer call comes back to a
    // kernel_sp belonging to the inner one, with no gate and no depth, and
    // returns into nothing.
    //
    // It is reachable from the shell, not just from a misbehaving package. A
    // package command can call fw_shell_run, and the shell will happily
    // dispatch another package command onto the same task. `novad1 service
    // restart` did exactly that — stop the screen, then fw_shell_run("novad1
    // gui --bg") — and the device died on an IACCVIOL at address zero with the
    // fault handler reporting "no package was running", because by then depth
    // said so.
    //
    // Refusing is the whole fix. Nesting could be MADE to work by stacking the
    // state, but a package that wants to run a package command wants a second
    // task, not a second frame on this one — and a refusal that says why costs
    // nothing to understand.
    if (s->depth) return SANDBOX_REENTERED;

    s->return_gate = return_gate;
    s->package_lr  = 0;
    s->depth       = 1;
    // The guard follows the package onto its own stack. `stack_top` is the top
    // of the sandbox block and the region is the size the caller planned it
    // with, so the base is one below the top.
    s->stack_base  = (uint8_t *)stack_top - stack_size;
    s->stack_size  = stack_size;
    // Two gates, both in the package's veneer pool because that is the only
    // memory a package may execute once privilege is gone. They differ in where
    // they end up: the enter gate branches to app_main and leaves LR holding
    // the exit gate, the return gate branches to LR.
    int ret = app_call_unpriv(fn, arg0, arg1, stack_top, exit_gate,
                              &s->kernel_sp, enter_gate,
                              guard_for(s->stack_base), pic_base);
    // Reached whether the package returned normally or was unwound out of by
    // the fault handler. A contained fault that never gets here died in the
    // tail, on the firmware stack, between the exception return and this line.
    bb_note_phase("sandbox: back on the firmware stack");
    // The shim let go of the stack limit to stand on the package's stack. This
    // task is back on its own now, so the guard describes it again.
    task_rearm_protection();
    return ret;
}

// Where the guard should point for a task that may be inside a package.
//
// Returns false when it is not, and the caller uses the task's own stack.
extern "C" bool sandbox_guard_stack(int slot, void **base, unsigned *size) {
    if (slot < 0 || slot >= (int)TASK_MAX) return false;
    SandboxState *s = &g_state[slot];
    if (!s->depth || !s->stack_base) return false;
    // The same band the shim armed, so a task that is rescheduled mid-package
    // comes back with the guard where it left it rather than at the bare
    // bottom. Two answers to this question is how a limit ends up somewhere
    // nobody chose.
    *base = (void *)(uintptr_t)guard_for(s->stack_base);
    *size = s->stack_size;
    return true;
}

bool sandbox_in_package(void) {
    SandboxState *s = state_of_current();
    return s && s->depth;
}

// End the package call a wedged task is inside, WITHOUT ending the task.
//
// The alarm's other answer is task_forced_exit, and for the shell that is no
// answer at all: a package command runs ON the shell task, so killing the task
// takes the session with it — which is why the shell was exempt, and why
// `havoc spin` rebooted the board instead of losing one command.
//
// This is nearly the unwind `svc #1` performs when a package returns normally:
// the exception return goes into the shim, which puts the firmware's stack back
// and returns out of app_call_unpriv; sandbox_enter then re-arms the guard and
// its caller carries on into sandbox_return and app_leave exactly as it would
// have. The task never notices it was interrupted, because from its point of
// view the package simply returned.
//
// The one difference is which entry point. A call being ABANDONED goes to
// app_call_unpriv_abandon, which is handed the firmware stack pointer in r1
// rather than calling sandbox_kernel_sp to ask for it. The ordinary tail's
// `bl` costs four words of the PACKAGE's stack, and one of the things being
// unwound out of here is a package that just ran out of exactly that — where
// those four words land below its stack, in the heap. The abandon path touches
// the package's stack not at all.
//
// r0 is set to -1 so the command reports a failure rather than whatever the
// package happened to leave in the register; r1 carries the way home.
static volatile bool g_abandoned;

extern "C" bool sandbox_abandon_call(uint32_t *frame) {
    SandboxState *s = state_of_current();
    if (!s || !s->depth || !frame) return false;
    // No parked stack pointer is no way back. Refusing here means the fault
    // stays fatal and says so, which is better than an exception return into a
    // `mov sp, #0`.
    if (!s->kernel_sp) return false;
    s->depth = 0;
    control_set(control_get() & ~CONTROL_NPRIV);
    frame[EXC_R0_WORD] = (uint32_t)-1;
    frame[EXC_R1_WORD] = s->kernel_sp;
    exc_frame_redirect(frame, (uint32_t)(uintptr_t)&app_call_unpriv_abandon);
    g_abandoned = true;
    return true;
}

// Read once and cleared, by whoever is about to report it. The alarm cannot
// print — it fires inside an arbitrary instruction — so it leaves a flag and
// the task context says so when it gets there.
extern "C" bool sandbox_took_call_back(void) {
    bool v = g_abandoned;
    g_abandoned = false;
    return v;
}

// Called from the preemption alarm before it redirects a stalled task into
// firmware. An unprivileged package cannot fetch instructions from flash, so
// the redirect would fault instead of terminating anything — and a fault
// reported as a protection violation, in a task that was about to be killed
// anyway, explains nothing to anybody.
void sandbox_release_for_kill(void) {
    SandboxState *s = state_of_current();
    if (s) s->depth = 0;
    control_set(control_get() & ~CONTROL_NPRIV);
}

void sandbox_forget(int slot) {
    if (slot >= 0 && slot < (int)TASK_MAX) memset(&g_state[slot], 0, sizeof(g_state[0]));
}

#else   // !SANDBOX_SUPPORTED

// =============================================================================
// A BOARD THAT CANNOT SANDBOX: what it can still do, and what it cannot
// =============================================================================
//
// That is the RP2040 parts — Pico and Pico W. The reason is at the top of this
// file and it is not a gap waiting to be filled: unprivileged execution needs
// the package's ABI calls to arrive as supervisor calls, and ARMv6-M cannot
// build that gateway at a price the part can pay. It is a property of the
// silicon, so the honest thing is to say precisely what follows from it rather
// than to leave people guessing which half of the feature list still applies.
//
// --- what still works, in full ----------------------------------------------
//
// PACKAGES RUN. main.cpp asks the loader for LOADER_VENEER_DIRECT instead of
//   LOADER_VENEER_SVC, so a veneer branches straight to the firmware's address
//   rather than raising an exception that names a function by index. Everything
//   above that is identical: the same .app files, the same loader, the same 156
//   ABI symbols, the same commands registered at load. pkgrepo refuses an
//   armv8m-only package up front — that is an instruction-set question, not
//   this one.
//
// MULTITASKING RUNS. The scheduler, both cores, affinity, sleeping, the job
//   list and the graded watchdog are all architecture-independent and none of
//   them consults this file.
//
// STACK GUARDS RUN. There is no MSPLIM, so mpu_rp2.cpp spends protection
//   region 0 on a 32-byte no-access block at the bottom of every stack. It
//   catches the write rather than the stack pointer crossing a line, which is a
//   shade weaker and enough for the overflow it exists to catch.
//
// FLASH SLOTS AND UPDATES RUN. A/B staging, rollback and the filesystem all
//   live behind RPC_FW_RESERVE, which is 768 KB here against 1024 KB on RP2350.
//   Smaller, not absent.
//
// --- what does not, and what each one costs ---------------------------------
//
// A PACKAGE RUNS WITH THE OS'S OWN PRIVILEGES. It can reach the task table, the
//   command table, the peripherals and its own machine code. Nothing stops a
//   bad pointer, so a package is as trusted as the firmware is.
//
// PACKAGE W^X IS NOT ENFORCED. task_app_mem_apply is a no-op on ARMv6-M: its
//   regions are power-of-two sized and self-aligned, so describing a 5 KB
//   package would cost up to 16 KB of a 264 KB part. See mpu_rp2.cpp.
//
// ABI POINTER ARGUMENTS ARE NOT RANGE-CHECKED. task_app_mem_current() returns
//   null for a privileged package on purpose — there is no region description
//   to test a pointer against, and the package could reach the memory itself
//   regardless, so a check would be theatre rather than protection.
//
// A FAULT IN A PACKAGE IS FATAL. fault_try_contain asks sandbox_in_package
//   first, and it is false below. Containment needs the parked firmware stack
//   pointer app_call_unpriv leaves behind and a separate sandbox stack to test
//   the faulting SP against; neither exists here, so the device reports and
//   reboots.
//
// A WEDGED PACKAGE COMMAND COSTS A REBOOT. This is the one that hurts, and it
//   is worth being exact about why there is no fix rather than no fix yet:
//
//     * A package command runs ON THE SHELL TASK, so the preemption alarm's
//       other answer — end the task — would end the session.
//     * Nothing respawns the shell. main.cpp spawns it once and shell_run never
//       returns, so a device that ended it would come back with no console at
//       all. That is worse than the reboot, not better.
//     * There is no call to take back. sandbox_abandon_call works by pointing
//       an exception return at the shim that stood the firmware's stack back
//       up; with a direct veneer the package was never on a different stack and
//       no shim ran, so there is nothing to return into.
//     * Unwinding the shell's own C stack instead — a setjmp around the
//       dispatch — was considered and rejected. The kill threshold is 6 s and
//       the watchdog is 16 s deliberately, because a TLS handshake can hold the
//       scheduler for seconds legitimately; a 6-second unwind would abort
//       working HTTPS fetches. And a package wedged inside a flash write has
//       core 1 parked by flash_safe_execute, which unwinding would leave parked
//       for good — trading a reboot for a hang.
//
//   So the watchdog is the answer, and the improvement is that it now says so:
//   preempt_tick records BB_STUCK_PACKAGE in the black box, which survives the
//   reset, and the next boot prints why nothing could be done instead of
//   leaving a bare "the device restarted".
//
// --- one intended thing that reads like a bug --------------------------------
//
// apps_spawn_in_sandbox passes require_sandbox=true so that a task a package
// asks for cannot quietly run unprotected. Here it has no effect: app_run_stack
// gates the refusal on sandbox_supported(), so the task simply runs privileged
// like every other package thread on this part. That is right — refusing would
// mean fw_task_spawn never worked on an RP2040 — and it is stated because the
// flag's name promises otherwise. apps.cpp also gives such a task the full
// stack the package asked for rather than TASK_STACK_MIN, precisely because the
// package's code runs on it rather than on a sandbox stack.
//
// --- reading it on the device ------------------------------------------------
//
// `mpu` prints all of the above for the board in front of you, and prints no
// sandbox counters here rather than printing the two hard zeroes below as if
// they were measurements. `help packages` says the privilege part in one line.
// =============================================================================

// Zero and zero, and they mean "not counted" rather than "none happened".
// Nothing in the shell prints these on a part with no sandbox, and nothing
// should start: two zeroes in a column of real numbers read as good news.
void sandbox_counts(uint32_t *calls, uint32_t *refused) {
    if (calls) *calls = 0;
    if (refused) *refused = 0;
}
int sandbox_enter(void *, int, void *, void *, uint32_t, uint32_t, uint32_t, uint32_t,
                  uint32_t) {
    return -1;
}

// No sandbox on ARMv6-M, so a task is always on its own stack and the
// scheduler's usual answer is the right one.
extern "C" bool sandbox_guard_stack(int, void **, unsigned *) { return false; }

extern "C" bool sandbox_abandon_call(uint32_t *) { return false; }
extern "C" bool sandbox_took_call_back(void) { return false; }

bool sandbox_in_package(void) { return false; }
void sandbox_release_for_kill(void) {}
void sandbox_forget(int) {}

#endif
