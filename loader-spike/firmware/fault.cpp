// Crash reporting.
//
// In MicroPython a bad pointer produces a traceback over serial for free. Here
// it produces a silent lockup unless this exists, which is why it is written in
// week one rather than when it is first needed: without it, every bug in the
// rest of the port is debugged blind.
//
// The handler prints the stacked frame the hardware pushed and then resets. It
// deliberately does NOT try to resume the faulting app: the stack is in an
// unknown state and continuing would turn one clear failure into a vague one.

#include <stdio.h>
#include "blackbox.h"
#include "logring.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

// Supplied by the OS, which knows the task and sandbox layout. Weak so the
// loader spike, which has neither, still links.
extern "C" void __attribute__((weak)) fault_report_stacks(uint32_t sp) { (void)sp; }

// Can this fault be contained rather than rebooted?
//
// Supplied by the OS, which knows whether a package was running and how to
// unwind out of one. Weak so the loader spike, which has no sandbox, keeps the
// old behaviour: every fault is fatal.
//
// Returns non-zero when it has REWRITTEN the frame so the exception return
// lands somewhere survivable. The handler then resumes instead of resetting.
extern "C" int __attribute__((weak)) fault_try_contain(uint32_t *frame) {
    (void)frame; return 0;
}


extern "C" {

// Set while an app is running, so a fault can say WHOSE fault it was.
volatile const char *g_current_app = nullptr;

struct FaultFrame {
    uint32_t r0, r1, r2, r3, r12, lr, pc, psr;
};

extern "C" const char *apps_locate(uint32_t addr, uint32_t *offset, bool *in_veneer);

// Returns non-zero when the fault was contained and execution may resume.
int fault_report(FaultFrame *f, const char *kind) {
    // Whether the STACK itself was the casualty. Set below, once CFSR is read,
    // and it decides whether this fault may be contained. See the note there.
    bool stack_intact = true;
    // RECORD FIRST, print second.
    //
    // A fault runs with interrupts off, so nothing services USB and everything
    // printed here sits in a buffer that is never delivered — the terminal sees
    // the device disappear and that is all. Writing to memory the reset does not
    // clear is the only way the report survives to be read, and it has to happen
    // before anything that might itself fault.
    char note[40];

    // Turn the PC into "package+offset" when it lands inside a loaded package.
    // The absolute address changes every boot because the image goes wherever
    // malloc puts it; an offset can be looked up in the .app with objdump and
    // names the exact instruction.
    uint32_t off = 0;
    bool in_veneer = false;
    const char *pkg = apps_locate(f->pc, &off, &in_veneer);
    const char *who = pkg ? pkg
                          : (g_current_app ? (const char *)g_current_app : "firmware");
    // Hand-built rather than snprintf: this is a fault handler, and calling into
    // stdio here is exactly the sort of thing that faults again.
    int n = 0;
    const char *k = kind;
    while (*k && n < 12) note[n++] = *k++;
    note[n++] = ' '; note[n++] = 'i'; note[n++] = 'n'; note[n++] = ' ';
    while (*who && n < 30) note[n++] = *who++;
    // "+0x1a2" when it is inside a package, the raw address when it is not.
    uint32_t show = pkg ? off : f->pc;
    if (pkg) { note[n++] = in_veneer ? '~' : '+'; }
    else     { note[n++] = ' '; note[n++] = 'p'; note[n++] = 'c'; note[n++] = '='; }
    bool started = false;
    for (int shift = 28; shift >= 0 && n < 39; shift -= 4) {
        uint32_t nib = (show >> shift) & 0xF;
        if (!nib && !started && shift) continue;      // trim leading zeros
        started = true;
        note[n++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    if (!started && n < 39) note[n++] = '0';
    note[n] = 0;
    bb_note_phase(note);

    // The fault STATUS register, which says exactly what kind of fault this was
    // rather than leaving it to be inferred from a program counter.
    //
    // CFSR is three registers in one word, and the bit numbering below is the
    // whole point of this block — it was wrong, in a way that mattered. Bit 1 is
    // DACCVIOL, a data access the memory protection refused, and it was labelled
    // "STKERR bad SP". It also came first, so it won every comparison. Now that
    // the protection hardware is actually configured, that bit is the ONE most
    // likely to be set, and it would have been reported as the opposite of what
    // it is: a corrupted stack pointer rather than a pointer that went where it
    // should not.
    //
    //   MMFSR, bits 0-7 — the memory protection unit
    //     IACCVIOL   an instruction fetch from memory marked non-executable
    //     DACCVIOL   a read or write the region permissions refused; MMFAR says
    //                where, and on this OS it usually means a package touched
    //                something that is not its own
    //     MSTKERR    the exception frame could not be pushed: a stack overflow
    //                caught by the guard at the bottom of a task's stack
    //   BFSR, bits 8-15 — the bus
    //     PRECISERR  a bad data address, with BFAR holding it
    //     IMPRECIS   a deferred write fault; the PC is PAST the instruction
    //   UFSR, bits 16-31 — the instruction stream
    //     INVSTATE   a call landed in ARM state — a function pointer without its
    //                Thumb bit, which is the classic loader bug
    //     STKOF      the stack pointer went below its limit. On ARMv8-M this is
    //                what a stack overflow looks like, and it names the exact
    //                instruction that did it.
    //
    // Recorded as a second line so the first still names the package and offset.
    {
        uint32_t cfsr = *(volatile uint32_t *)0xE000ED28;
        uint32_t hfsr = *(volatile uint32_t *)0xE000ED2C;
        uint32_t mmfar = *(volatile uint32_t *)0xE000ED34;
        uint32_t bfar  = *(volatile uint32_t *)0xE000ED38;
        const char *what = "?";
        // Most specific first. A stack overflow is the one worth naming plainly,
        // because it is the failure that used to corrupt whatever the heap had
        // put below the stack and get blamed on something else entirely.
        if      (cfsr & (1u << 20)) what = "STKOF stack overflow";
        else if (cfsr & (1u << 4))  what = "MSTKERR stack overflow on entry";
        else if (cfsr & (1u << 1))  what = "DACCVIOL wrote where it may not";
        else if (cfsr & (1u << 0))  what = "IACCVIOL not executable";
        else if (cfsr & (1u << 3))  what = "MUNSTKERR";
        else if (cfsr & (1u << 17)) what = "INVSTATE no Thumb bit";
        else if (cfsr & (1u << 16)) what = "UNDEFINSTR";
        else if (cfsr & (1u << 18)) what = "INVPC";
        else if (cfsr & (1u << 19)) what = "NOCP no coprocessor";
        else if (cfsr & (1u << 24)) what = "UNALIGNED";
        else if (cfsr & (1u << 25)) what = "DIVBYZERO";
        else if (cfsr & (1u << 9))  what = "PRECISERR bad addr";
        else if (cfsr & (1u << 10)) what = "IMPRECISERR";
        else if (cfsr & (1u << 12)) what = "STKERR bus";
        else if (cfsr & (1u << 11)) what = "UNSTKERR bus";
        else if (cfsr & (1u << 8))  what = "IBUSERR";
        else if (hfsr & (1u << 30)) what = "FORCED escalated";

        // The ADDRESS, when there is one. Without it a protection fault says
        // only that something was refused, and the whole reason to configure the
        // hardware was to be told what.
        // A STACK OVERFLOW CANNOT BE CONTAINED, and the comment on
        // FAULT_RELEASE_STACK_LIMIT is why: this handler clears MSPLIM so it
        // can run at all, and everything it prints then runs off the bottom of
        // the exhausted stack into whatever the heap put below it. That was an
        // acceptable trade against "no report at all" on a device two seconds
        // from a reset — and it stops being acceptable the moment the device
        // carries on, because the corruption carries on with it.
        //
        // So an overflow still resets. The report is the same and it names the
        // package; what is not offered is a machine that keeps running on
        // memory this handler has already written through.
        stack_intact = !(cfsr & (1u << 4)) && !(cfsr & (1u << 20));  // MSTKERR, STKOF

        bool have_mm = (cfsr & (1u << 7)) != 0;      // MMARVALID
        bool have_bf = (cfsr & (1u << 15)) != 0;     // BFARVALID
        printf("    cfsr=0x%08lx hfsr=0x%08lx sp=%p  %s\n",
               (unsigned long)cfsr, (unsigned long)hfsr, (void *)f, what);
        if (have_mm || have_bf)
            printf("    faulting address = 0x%08lx\n",
                   (unsigned long)(have_mm ? mmfar : bfar));
        // And into the log ring, which survives to be read by logdump.
        log_addf(LOG_K_ERR, "fault %s cfsr=%08lx addr=%08lx sp=%08lx lr=%08lx",
                 what, (unsigned long)cfsr,
                 (unsigned long)(have_mm ? mmfar : have_bf ? bfar : 0),
                 (unsigned long)(uintptr_t)f, (unsigned long)f->lr);
    }

    printf("\n");
    printf("*** %s ***\n", kind);
    if (g_current_app) printf("    in app: %s\n", (const char *)g_current_app);
    printf("    pc  = 0x%08lx   lr  = 0x%08lx\n",
           (unsigned long)f->pc, (unsigned long)f->lr);
    printf("    r0  = 0x%08lx   r1  = 0x%08lx\n",
           (unsigned long)f->r0, (unsigned long)f->r1);
    printf("    r2  = 0x%08lx   r3  = 0x%08lx\n",
           (unsigned long)f->r2, (unsigned long)f->r3);
    printf("    psr = 0x%08lx\n", (unsigned long)f->psr);

    // WHICH STACK, and how much of it was left.
    //
    // A stacking fault says the exception frame would not fit, and nothing
    // else. Which stack it would not fit in is the entire question, and four
    // rounds of reasoning from an address and a fault code got it wrong twice —
    // so the device answers it directly instead.
    //
    // The regions come from the same table the protection unit is programmed
    // from, so this reports where the stack pointer ACTUALLY was rather than
    // where it was expected to be.
    fault_report_stacks((uint32_t)(uintptr_t)f);

    // CONTAIN IT IF IT BELONGS TO A PACKAGE.
    //
    // A sandboxed package faulting is the case the sandbox exists for. Taking
    // the whole device down for it is the opposite of containment — and the
    // report above has already been printed, so nothing is hidden by carrying
    // on.
    if (stack_intact && fault_try_contain((uint32_t *)f)) {
        printf("    the package was stopped; the device is still running\n\n");

        // CLEAR THE FAULT REGISTERS. CFSR bits are sticky and MMFAR keeps its
        // last value, and nothing cleared them before because every fault ended
        // in a reset. Now that one can be survived, the next report inherits
        // them: a stack overflow was reported as "addr=10000000", the address
        // from a DACCVIOL two commands earlier, which is a lie about the one
        // number in that report that comes from hardware.
        *(volatile uint32_t *)0xE000ED28 = *(volatile uint32_t *)0xE000ED28;  // CFSR, w1c
        *(volatile uint32_t *)0xE000ED2C = *(volatile uint32_t *)0xE000ED2C;  // HFSR, w1c

        // LET IT OUT BEFORE RESUMING. The reset path spends two seconds
        // flushing; this one returned immediately, so the tail of the report
        // was still in the USB buffer when the shell printed over it — the
        // region dump ended mid-number.
        fflush(stdout);
        for (int i = 0; i < 30; i++) sleep_ms(10);
        return 1;
    }

    printf("    resetting in 2s\n\n");
    // Flush before the reset, or the report never leaves the USB buffer.
    for (int i = 0; i < 200; i++) { sleep_ms(10); }
    watchdog_reboot(0, 0, 0);
    while (1) {}
}

// Explicitly `extern`: a plain `const char[]` at namespace scope has INTERNAL
// linkage in C++, so the inline asm below cannot see it and the link fails on a
// symbol that is plainly right there in the same file.
extern const char fault_kind_hard[];

// A '~' before the offset marks a veneer rather than the package's own code —
// which would point at the loader's trampolines instead of anything the package
// author wrote.
extern "C" const char *apps_locate(uint32_t addr, uint32_t *offset, bool *in_veneer);

// Release the stack limit before anything else.
//
// On ARMv8-M the stack guard is MSPLIM, and a task that overflows its stack
// arrives here with the stack pointer sitting AT the limit — the push that
// violated it was refused, which is how the fault was raised. Everything below
// runs on that same stack and pushes to it, so with the limit still in place
// the very first push in the handler violates it again. A fault taken while
// already handling a fault is not another report; it is the processor entering
// lockup, and the device goes dark holding the one piece of information that
// would have explained it.
//
// Writing zero costs two instructions and makes the handler reachable. It does
// mean the report itself can run off the bottom of an exhausted stack into the
// heap — but the crash note is written to memory that survives a reset before
// any of the printing starts, so it is a corrupted heap on a device that is
// two seconds from rebooting anyway, against no report at all.
#if defined(__ARM_ARCH_6M__)
  #define FAULT_RELEASE_STACK_LIMIT ""
#else
  #define FAULT_RELEASE_STACK_LIMIT  "movs r0, #0     \n" \
                                     "msr  msplim, r0 \n"
#endif

// Pick the stack the exception came from (MSP or PSP) out of EXC_RETURN, then
// hand the frame to the C reporter. Naked so the prologue cannot disturb it.
__attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        FAULT_RELEASE_STACK_LIMIT
        // Interrupts back on, whatever the faulting code had done.
        //
        // The report goes out over USB, and USB is serviced by a task — so with
        // interrupts masked the console stalls after the first buffered flush
        // and the watchdog resets the board before the rest is printed. That is
        // why some of these crashes arrived as a single line and some as
        // nothing at all: not that the handler failed, but that it could not
        // get its words out.
        //
        // Nothing is being protected here. The device is two seconds from a
        // reset either way, and a fault nobody can read is worth less than the
        // small chance of a second one while reading it.
        "cpsie i            \n"
        "movs r0, #4        \n"
        "mov  r1, lr        \n"
        "tst  r0, r1        \n"
        "beq  1f            \n"
        "mrs  r0, psp       \n"
        "b    2f            \n"
        "1:                 \n"
        "mrs  r0, msp       \n"
        "2:                 \n"
        "ldr  r1, =fault_kind_hard \n"
        // Was a tail branch, because the reporter never came back. It can now:
        // a fault inside a package is contained rather than fatal, and then the
        // exception return has to happen. r4 rides along only to keep the stack
        // eight-byte aligned across the call.
        "push {r4, lr}      \n"
        "bl   fault_report  \n"
        "pop  {r4, pc}      \n"
        ".align 2           \n"
    );
}

extern const char fault_kind_hard[] = "HARD FAULT";

}  // extern "C"
