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

    if (exc_frame_enter_firmware(frame, target,
                                 (uint32_t)(uintptr_t)&sandbox_syscall_return,
                                 &saved) != SYSCALL_OK) {
        g_refused++;
        return;                    // nothing written: it faults where it asked
    }
    if (s) s->package_lr = saved;
    // Privileged from the exception return until sandbox_syscall_return hands
    // it back. The firmware function runs on the package's stack, which is why
    // that stack is sized like any other task's rather than like a scratch area.
    control_set(control_get() & ~CONTROL_NPRIV);
}

// --- entering package code ---------------------------------------------------

int sandbox_enter(void *fn, int arg0, void *arg1, void *stack_top,
                  uint32_t return_gate, uint32_t exit_gate) {
    SandboxState *s = state_of_current();
    if (!s || !fn || !stack_top || !return_gate || !exit_gate) return -1;
    s->return_gate = return_gate;
    s->package_lr  = 0;
    s->depth       = 1;
    return app_call_unpriv(fn, arg0, arg1, stack_top, exit_gate, &s->kernel_sp);
}

bool sandbox_in_package(void) {
    SandboxState *s = state_of_current();
    return s && s->depth;
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

void sandbox_counts(uint32_t *calls, uint32_t *refused) {
    if (calls) *calls = 0;
    if (refused) *refused = 0;
}
int sandbox_enter(void *, int, void *, void *, uint32_t, uint32_t) { return -1; }
bool sandbox_in_package(void) { return false; }
void sandbox_release_for_kill(void) {}
void sandbox_forget(int) {}

#endif
