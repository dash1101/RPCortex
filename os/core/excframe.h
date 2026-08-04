// The Cortex-M exception frame, as arithmetic.
//
// Forced termination works by rewriting the PC the hardware stacked when an
// interrupt fired, so the task resumes somewhere else. Reading a register is
// ARM; deciding WHAT to write is not, and this is that part — which matters
// because the two fields involved both fail silently when wrong.
//
// Splitting it out means the layout, the Thumb bit and the IT-state clearing
// are all checked on a machine with no Cortex-M in it, leaving four lines of
// inline assembly as the only genuinely unverifiable part.
//
// Frame layout, pushed by exception entry, low address first:
//   0:r0  1:r1  2:r2  3:r3  4:r12  5:LR  6:PC  7:xPSR
#ifndef RPC_EXCFRAME_H
#define RPC_EXCFRAME_H

#include <stdint.h>

#define EXC_FRAME_WORDS  8
#define EXC_R0_WORD      0
#define EXC_R12_WORD     4
#define EXC_LR_WORD      5
#define EXC_PC_WORD      6
#define EXC_XPSR_WORD    7

// Thumb state. An exception return with this clear faults, and the fault looks
// nothing like its cause — the same family as the loader's Thumb-bit bug.
#define XPSR_THUMB   (1u << 24)

// IT/ICI state. On ARMv8-M an interrupt can land inside an IT block, and the
// stacked xPSR then carries the condition bits for instructions that were about
// to run. Returning with those set would execute the first instructions of the
// replacement function under a stale predicate — silently skipping them.
// ARMv6-M has no IT block, so clearing these there costs nothing and is safe.
#define XPSR_IT_ICI  ((3u << 25) | (0x3fu << 10))

// Point the frame at `handler` on return. `handler` is a function address; its
// Thumb bit is set here rather than assumed, because a cast is exactly where it
// gets lost.
void exc_frame_redirect(uint32_t *frame, uint32_t handler);

// Which stack the interrupted code was using, from EXC_RETURN. Bit 2 set means
// the process stack. Checked rather than assumed: assuming the main stack would
// quietly rewrite the wrong one the day tasks move to PSP.
bool exc_return_used_psp(uint32_t exc_return);

// --- turning a package's supervisor call into a firmware call ---------------
//
// A sandboxed package cannot branch into the firmware: flash is not reachable
// from unprivileged code. So it puts the index of the function it wants in r12
// and executes SVC, and the handler redirects the exception return into the
// firmware instead of back to the package.
//
// All of that is arithmetic on the eight words the hardware pushed, which is
// why it lives here rather than in the handler: every field involved fails
// silently when wrong, and none of it needs a Cortex-M to check.

// Why a supervisor call was refused. Anything other than OK means the package
// asked for something that does not exist, and the handler must not perform it.
enum SyscallCheck {
    SYSCALL_OK = 0,
    SYSCALL_BAD_INDEX,      // no such function in the ABI table
    SYSCALL_NO_FRAME,       // nothing to work on
};

// Rewrite `frame` so the exception returns into `target` rather than back into
// the package, with `on_return` as the address it will return to afterwards.
//
// The package's OWN return address — the one the hardware stacked — is handed
// back through `saved_lr`, because the rewrite destroys it and the way home
// depends on it. Losing it does not fault; it returns the package to whatever
// address happened to be there.
//
// `target` of zero means the index was out of range and nothing is written:
// a refused call must leave the frame exactly as it was, so the caller can turn
// it into a fault report rather than a jump to address zero.
SyscallCheck exc_frame_enter_firmware(uint32_t *frame, uint32_t target,
                                      uint32_t on_return, uint32_t *saved_lr);

// The index the package asked for, from the stacked r12.
//
// It comes from a register rather than from the SVC immediate because the
// immediate is eight bits wide and the ABI passed 156 entries some time ago.
// The value is entirely under the package's control, which is exactly why the
// bounds check on the other side of it is the security property.
uint32_t exc_frame_syscall_index(const uint32_t *frame);

#endif  // RPC_EXCFRAME_H
