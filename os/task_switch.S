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
    // r0 = void **save_sp   r1 = void *to_sp
#if defined(__ARM_ARCH_6M__) || defined(PICO_RP2040)
    push    {r4-r7, lr}
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11
    push    {r4-r7}                 // frame, low->high: r8-r11, r4-r7, LR
    mov     r2, sp
    str     r2, [r0]                // *save_sp = sp
    mov     sp, r1                  // switch stacks
    pop     {r4-r7}
    mov     r8, r4
    mov     r9, r5
    mov     r10, r6
    mov     r11, r7
    pop     {r4-r7, pc}             // the last word becomes PC
#else
    push    {r4-r11, lr}            // frame, low->high: r4-r11, LR
    mov     r2, sp
    str     r2, [r0]
    mov     sp, r1
    pop     {r4-r11, pc}
#endif
    .size   task_ctx_switch, . - task_ctx_switch
