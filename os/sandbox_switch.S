// The parts of the sandbox that have to be instructions.
//
// Three of them, and each exists because there is no way to say it in C:
//
//   isr_svcall               reads the stack the exception came from
//   app_call_unpriv          swaps to the package's stack and drops privilege
//   sandbox_syscall_return   hands privilege back without a second exception
//
// ARMv8-M only. On ARMv6-M packages run privileged, none of this is reachable,
// and half of it does not assemble.

    .syntax unified
    .thumb

#if !defined(__ARM_ARCH_6M__)

// --- the supervisor call -----------------------------------------------------
//
// Naked, because the frame the hardware pushed is the entire subject and a
// compiler prologue would move the stack pointer out from under it.
//
// EXC_RETURN bit 2 says whether the interrupted code was on the main stack or
// the process stack. Both are read rather than assumed: this OS runs its tasks
// on MSP today, and assuming so would quietly rewrite the wrong frame on the
// day that changes.
//
// The SVC number is not in a register anywhere. It is an immediate inside the
// instruction, so it has to be read back out of the code — two bytes below the
// address the hardware stacked, which is the instruction AFTER the SVC.
//
// Two words are pushed rather than one. AAPCS wants the stack eight-byte
// aligned at a call, and a single `push {lr}` leaves it four out.
    .section .text.isr_svcall, "ax"
    .global isr_svcall
    .type   isr_svcall, %function
    .thumb_func
isr_svcall:
    tst     lr, #4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    ldr     r1, [r0, #24]           // the stacked PC
    ldrb    r1, [r1, #-2]           // the SVC immediate
    push    {r4, lr}
    bl      sandbox_svc
    pop     {r4, pc}                // pc = EXC_RETURN: the exception return
    .size   isr_svcall, . - isr_svcall

// --- entering package code ---------------------------------------------------
//
//   r0 = fn         the package's entry point
//   r1 = arg0       becomes the package's first argument
//   r2 = arg1       ...and its second, so a registered command's argc/argv fit
//   r3 = stack_top  the package's own stack
//   [sp, #40] = exit_gate, in the veneer pool: where the package returns to
//   [sp, #44] = where to park the firmware's stack pointer
//
// The package needs a stack of its own and this is the reason: app_main and
// every command a package registers would otherwise run on the SHELL's stack,
// which holds the shell's saved return addresses into privileged firmware. A
// package able to write those can send the OS anywhere it likes, so dropping
// privilege while sharing a stack is not weaker isolation — it is none.
//
// Ten words are pushed, not nine, to keep the stack eight-byte aligned.
    .section .text.app_call_unpriv, "ax"
    .global app_call_unpriv
    .type   app_call_unpriv, %function
    .thumb_func
app_call_unpriv:
    push    {r3, r4-r11, lr}
    ldr     r4, [sp, #44]           // where to park the firmware stack
    mov     r5, sp
    str     r5, [r4]
    ldr     r6, [sp, #40]           // the exit gate

    mov     sp, r3                  // stand on the package's stack
    mov     r12, r0
    mov     r0, r1                  // the package's two arguments
    mov     r1, r2
    mov     lr, r6                  // return to the exit gate, not to here

    mrs     r2, control
    orr     r2, r2, #1              // nPRIV. Read-modify-write, because CONTROL
    msr     control, r2             // also carries SPSEL and FPCA, and this
    isb                             // build has an FPU that uses FPCA.
    bx      r12                     // into the package, unprivileged

// Where `svc #1` sends the exception return once the package is done.
// Privileged again by then, still on the package's stack, with r0 holding what
// app_main returned — the exception return restored it.
//
// r0 has to be got out of the way before asking where the firmware stack went,
// because that answer arrives in r0 as well. r4 is free for it: the pop at the
// end restores whatever the caller had there.
    .global app_call_unpriv_tail
    .type   app_call_unpriv_tail, %function
    .thumb_func
app_call_unpriv_tail:
    mov     r4, r0                  // the package's exit code
    bl      sandbox_kernel_sp
    mov     sp, r0                  // back onto the firmware's stack
    mov     r0, r4
    pop     {r3, r4-r11, pc}
    .size   app_call_unpriv, . - app_call_unpriv

// --- coming back from an ABI call --------------------------------------------
//
// The firmware function has returned. r0 — and r1, for a 64-bit result — hold
// its value and must survive untouched all the way back into the package.
//
// The `msr` that gives privilege back cannot be here. Everything after it runs
// unprivileged, and this is flash, which a sandboxed package may not fetch
// from. So this sets up the two registers the gate needs and jumps to it. The
// gate lives in the package's veneer pool, which is executable to the package
// and read-only, and does `msr CONTROL, r2 ; isb ; bx r3`.
    .section .text.sandbox_syscall_return, "ax"
    .global sandbox_syscall_return
    .type   sandbox_syscall_return, %function
    .thumb_func
sandbox_syscall_return:
    push    {r0, r1, r4, lr}        // four words: the return value, and alignment
    bl      sandbox_return_gate
    mov     r12, r0                 // where to jump
    bl      sandbox_finish_call
    mov     r3, r0                  // where the package resumes
    pop     {r0, r1, r4, lr}
    mrs     r2, control
    orr     r2, r2, #1              // the value the gate is about to write
    bx      r12
    .size   sandbox_syscall_return, . - sandbox_syscall_return

#endif  // !__ARM_ARCH_6M__
