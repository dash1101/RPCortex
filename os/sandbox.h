// Running a package without the OS's own privileges.
//
// See sandbox_rp2.cpp for how, and UNPRIV-DESIGN.md for why it is shaped this
// way. The short version: a sandboxed package can reach its own five regions
// and nothing else in the machine, and every call it makes into the firmware is
// a supervisor call rather than a branch.
#ifndef RPC_SANDBOX_H
#define RPC_SANDBOX_H

#include <stdint.h>
#include <stddef.h>

// False on ARMv6-M, where the protection regions would cost more RAM than the
// part has. Packages run privileged there, exactly as they did before.
bool sandbox_supported(void);

// Run `fn(arg)` unprivileged on `stack_top`, returning what it returned.
//
// `return_gate` and `exit_gate` are addresses in the package's own veneer pool.
// They have to be: the last instructions of both paths out of privileged code
// run unprivileged, and unprivileged code cannot fetch from flash.
//
// -1 if the sandbox could not be set up, which is a refusal to run the package
// rather than a decision to run it unprotected.
//
// SANDBOX_REENTERED if this task is already inside a package call. One call per
// task: the state that says how to get back out is per-task and singular, and a
// second entry overwrites the first one's way home. See the note at the top of
// sandbox_enter for how the shell reaches that on an ordinary command.
//
// Distinct from -1 because it is not a failure of the sandbox — it is a thing
// the caller asked for that cannot be done, and the caller can say so usefully.
#define SANDBOX_REENTERED (-2)

// `pic_base` is the value r9 must hold on entry — the package's GOT origin — or
// 0 for a non-PIC package, which leaves r9 alone. It is set at the enter gate,
// inside the window where app_call_unpriv has saved the firmware's r9, and every
// callee the package reaches (firmware, libgcc, the SVC round trip) treats r9 as
// callee-saved, so it survives from there. See app_pic_base in the loader.
int sandbox_enter(void *fn, int arg0, void *arg1, void *stack_top,
                  uint32_t return_gate, uint32_t enter_gate, uint32_t exit_gate,
                  uint32_t stack_size, uint32_t pic_base);

// Enter package code that runs PRIVILEGED — ARMv6-M, where there is no sandbox,
// and the RP2350 fallback when one could not be allocated — with r9 pointed at
// the package's GOT. A non-PIC package needs none of this and takes the ordinary
// C call; this exists for the PIC case, where r9 has to be the GOT base on entry.
// `fn` is int(int) for app_main and int(int,char**) for a registered command;
// both pass their arguments in r0/r1 under AAPCS, so one trampoline serves both.
extern "C" int app_call_direct(void *fn, int arg0, void *arg1, uint32_t pic_base);

// The stack a task is running on while inside a package, so the scheduler can
// arm the stack guard against THAT rather than against the task's own stack.
//
// They are different memory: a sandboxed package runs on its own allocation.
// Arming the guard against the wrong one is a fault whose address depends on
// which the heap happened to put lower.
extern "C" bool sandbox_guard_stack(int slot, void **base, unsigned *size);

// Whether THIS task is currently inside package code.
bool sandbox_in_package(void);

// Supervisor calls served, and how many were refused.
//
// Refusals are the number worth watching: each one is a package that named a
// function index the firmware does not export, which means either a corrupted
// veneer pool or a package doing something it was not built from.
void sandbox_counts(uint32_t *calls, uint32_t *refused);

// Give privilege back before a stalled task is redirected into firmware.
//
// The preemption alarm terminates a runaway task by rewriting the PC the
// hardware stacked. That target is in flash, which an unprivileged package may
// not fetch from — so without this the redirect faults instead of terminating
// anything, and reports a protection violation in a task that was already being
// killed, which explains nothing to anybody.
void sandbox_release_for_kill(void);

// Drop everything remembered about a task slot, when the scheduler reuses it.
void sandbox_forget(int slot);

// The assembly half. Declared here so there is one place that says what the
// arguments are; sandbox_switch.S is where they mean something.
extern "C" int  app_call_unpriv(void *fn, int arg0, void *arg1, void *stack_top,
                                uint32_t exit_gate, uint32_t *park_sp_here,
                                uint32_t enter_gate, uint32_t stack_guard,
                                uint32_t pic_base);
extern "C" void app_call_unpriv_tail(void);
// The same unwind for a call that is being abandoned rather than finished. It
// takes the firmware stack pointer in r1 instead of asking for it, so it uses
// none of the package's stack — which the package may have just exhausted.
extern "C" void app_call_unpriv_abandon(void);
extern "C" void sandbox_syscall_return(void);
extern "C" uint32_t sandbox_kernel_sp(void);
extern "C" uint32_t sandbox_return_gate(void);
extern "C" uint32_t sandbox_finish_call(void);

#endif  // RPC_SANDBOX_H

// End the package call a wedged task is inside, without ending the task. Called
// from the preemption alarm; see sandbox_rp2.cpp.
extern "C" bool sandbox_abandon_call(uint32_t *frame);
// Whether a call was taken back since this was last asked. Cleared on read.
extern "C" bool sandbox_took_call_back(void);
