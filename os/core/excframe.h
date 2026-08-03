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

#endif  // RPC_EXCFRAME_H
