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
//   [sp, #40] = exit_gate,  in the veneer pool: where the package returns to
//   [sp, #44] = where to park the firmware's stack pointer
//   [sp, #48] = enter_gate, in the veneer pool: where privilege is dropped
//   [sp, #52] = where to set the stack limit once standing on that stack
//   [sp, #56] = pic_base, the GOT origin r9 must hold in package code (0 = none)
//
// The package needs a stack of its own and this is the reason: app_main and
// every command a package registers would otherwise run on the SHELL's stack,
// which holds the shell's saved return addresses into privileged firmware. A
// package able to write those can send the OS anywhere it likes, so dropping
// privilege while sharing a stack is not weaker isolation — it is none.
//
// The drop itself does NOT happen here, and that is the whole subtlety. The
// instruction after `msr CONTROL` runs unprivileged, and this file is in flash
// — which a sandboxed package may not fetch from. Writing the msr, an isb and a
// bx in a row therefore faults on the BX, after the pipeline flush forces it to
// be re-fetched under the new permissions. It is not the branch that is wrong,
// it is where the branch lives.
//
// So this hands two registers to the gate in the package's veneer pool and
// jumps there while still privileged. The gate is `msr CONTROL, r2 ; isb ;
// bx r3`, and every instruction of it is somewhere the package is allowed to
// execute. It is the same gate the return path uses, for the same reason.
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
    ldr     r7, [sp, #48]           // the gate that drops privilege
    mov     r8, r0                  // fn, before r0 becomes an argument
    ldr     r9, [sp, #52]           // where the guard goes once SP has moved
    ldr     r10, [sp, #56]          // pic_base, read NOW: the stacked arguments
                                    // are on the firmware stack and vanish the
                                    // moment SP moves to the package's below

    // RELEASE THE LIMIT, MOVE, THEN ARM IT AGAIN.
    //
    // The release is unavoidable: the package's stack is a different heap block
    // and may be below the limit currently protecting the task's, so the limit
    // has to go before SP can. Leaving it AT ZERO for the whole call is the
    // part that was wrong.
    //
    // The claim was that MPU region 4 covers the package's stack exactly, so
    // running off the end faults anyway. It does — but too late to be caught
    // usefully. `sub sp, #264` is not a memory access, so with no limit set
    // nothing stops the stack pointer walking straight past the bottom of the
    // region; only the first WRITE faults, and by then SP is already outside.
    // The exception frame is then pushed at that outside address, which is why
    // a package stack overflow arrived as MSTKERR with a frame 256 bytes below
    // the region — unreadable, unrewritable, and therefore uncontainable.
    //
    // With the limit on the package's own stack the SUB itself faults, SP is
    // left untouched, and the frame lands inside the region where the fault
    // handler can find it. The caller leaves a band above the bottom so there
    // is room for that frame; see PKG_STACK_GUARD_BAND.
    movs    r5, #0
    msr     msplim, r5
    mov     sp, r3                  // stand on the package's stack
    msr     msplim, r9              // and guard it at its own bottom
    mov     r9, r10                 // r9 = pic_base: the package's GOT origin, or
                                    // 0 for a non-PIC package, which never reads
                                    // it. This is the last write before the enter
                                    // gate; from here every callee treats r9 as
                                    // callee-saved, so it holds all the way in.

    mov     r0, r1                  // the package's two arguments
    mov     r1, r2
    mov     r3, r8                  // the gate branches here: app_main
    mov     lr, r6                  // ...and this is where app_main returns to
    mrs     r12, control
    orr     r12, r12, #1            // nPRIV, for the gate to write. Read-modify-
                                    // write: CONTROL also carries SPSEL and
                                    // FPCA, and this build has an FPU.
    bx      r7                      // -> the enter gate, still privileged

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
    mov     r5, #0
    msr     msplim, r5              // released before SP moves; see above
    mov     sp, r0                  // back onto the firmware's stack
    mov     r0, r4
    pop     {r3, r4-r11, pc}

// --- unwinding out of a package that is not going to return ------------------
//
// Where a fault or the preemption alarm sends the exception return when a call
// has to be ABANDONED rather than finished.
//
// The difference from the tail above is that this touches the package's stack
// not at all. The firmware stack pointer arrives in r1, put there by
// sandbox_abandon_call and restored by the exception return, so there is no
// call to make and nothing to push.
//
// That matters for exactly one case, and it is the case that made this
// necessary: a package that overflowed its OWN stack. There, the four words a
// `bl` costs would be written past the bottom of its stack and into whatever
// the heap put below — which is the corruption the containment exists to
// avoid, arriving through the code doing the containing.
//
// r0 is the exit code the abandon wrote, and it survives untouched.
    .global app_call_unpriv_abandon
    .type   app_call_unpriv_abandon, %function
    .thumb_func
app_call_unpriv_abandon:
    movs    r2, #0
    msr     msplim, r2              // released before SP moves; see above
    mov     sp, r1                  // straight onto the firmware's stack
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
    // r0-r3 all belong to the caller and none of them may be touched.
    //
    // A function may return a value in any of r0-r3, and __aeabi_uldivmod
    // returns its remainder in r2 and r3 specifically — so using either as
    // scratch corrupts every 64-bit division a package performs. That showed up
    // as `calc 2 ^ 3 ^ 2` printing '===' where the digits should be, with the
    // fractional part, which needs no such helper, exactly right.
    //
    // The way out therefore carries nothing in r0-r3: the new CONTROL value
    // travels in r12, the address to resume at in LR, and the gate itself is
    // reached by writing it over the saved LR slot and popping straight into
    // PC. That is also why there are two gates rather than one — this one ends
    // `bx lr`, the entry gate ends `bx r3`, because entering has to leave LR
    // holding somewhere for app_main to return to.
    push    {r0, r1, r2, r3, r4, lr}    // six words: 8-byte aligned
    bl      sandbox_return_gate
    mov     r4, r0                      // where privilege is given back
    bl      sandbox_finish_call
    mov     lr, r0                      // where the package resumes
    mrs     r12, control
    orr     r12, r12, #1                // the value the gate is about to write
    str     r4, [sp, #20]               // over the saved LR: the pop branches there
    pop     {r0, r1, r2, r3, r4, pc}
    .size   sandbox_syscall_return, . - sandbox_syscall_return

#endif  // !__ARM_ARCH_6M__
