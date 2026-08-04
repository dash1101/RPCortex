// The context switch.
//
// Save the callee-saved registers on the current stack, record the stack
// pointer, swap to another stack, restore. Everything else about a task lives
// in core/task.cpp; this is the only part that has to know what a CPU is.
//
// Two variants, because ARMv6-M (Cortex-M0+, RP2040) cannot push or pop the
// high registers directly — PUSH and POP on that architecture reach r0-r7, LR
// and PC only. They are moved through the low registers instead, which is why
// the frame layouts differ between the two. Nothing above cares: the frame is
// written by task_ctx_init in the matching order, and the only word either
// version needs to be meaningful is the last one, which is popped into PC.
//
// The third argument is the scheduler's `live` flag for the OUTGOING task. It
// is cleared here, between storing the stack pointer and swapping stacks,
// because that is the only instant that qualifies: before the store the saved
// sp is still the stale one from the last park, and after the swap this code is
// no longer running on the task being parked. Clearing it any earlier lets the
// other core resume a context that has not been written yet, which is a branch
// through whatever happens to be on an old stack. The DMB is what stops the
// other core observing the clear before the store — same bug, one level down.

    .syntax unified
    .thumb

// No .cpu directive: it would pin the assembler to one architecture and reject
// the other branch below. The build already passes the right -mcpu per board,
// and __ARM_ARCH_6M__ comes from the same place, so the two always agree.

    .section .text.task_ctx_switch, "ax"
    .global task_ctx_switch
    .type   task_ctx_switch, %function
    .thumb_func
task_ctx_switch:
    // r0 = void **save_sp   r1 = void *to_sp   r2 = volatile bool *live_out
    //
    // r2 holds live_out for the whole routine, so sp is staged through r3
    // rather than r2. Both are caller-saved and neither is in the frame below.
#if defined(__ARM_ARCH_6M__) || defined(PICO_RP2040)
    push    {r4-r7, lr}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    push    {r4-r7}                 // frame, low->high: r8-r11, r4-r7, LR
    mov     r3, sp
    str     r3, [r0]                // *save_sp = sp
    cmp     r2, #0
    beq     1f
    dmb                             // sp must land before the clear is visible
    movs    r3, #0
    strb    r3, [r2]                // *live_out = false: claimable from here
1:
    mov     sp, r1                  // switch stacks
    pop     {r4-r7}
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    pop     {r4-r7, pc}             // the last word becomes PC
#else
    push    {r4-r11, lr}            // frame, low->high: r4-r11, LR
    mov     r3, sp
    str     r3, [r0]
    cbz     r2, 1f
    dmb
    movs    r3, #0
    strb    r3, [r2]
1:
    // Let go of the stack limit BEFORE standing on a different stack.
    //
    // On ARMv8-M, writing a value below MSPLIM into SP is itself a stack
    // overflow — the check is on the write, not on a later push. Every task's
    // stack comes from the heap in main SRAM while the boot stack sits high in
    // SCRATCH_Y, so the very first switch out of the boot context moves SP
    // tens of kilobytes below the limit that was protecting it, and faults on
    // this instruction rather than anywhere near a real overflow.
    //
    // The guard is re-armed for the incoming context immediately after the
    // switch returns, in arm_protection. The few instructions in between — the
    // pop, the return, the call — run unguarded, which is the price and is
    // unavoidable: there is no ordering of "change SP" and "change the limit"
    // that leaves neither of them momentarily describing the other's stack.
    movs    r3, #0
    msr     msplim, r3
    mov     sp, r1
    pop     {r4-r11, pc}
#endif
    .size   task_ctx_switch, . - task_ctx_switch
