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

extern "C" {

// Set while an app is running, so a fault can say WHOSE fault it was.
volatile const char *g_current_app = nullptr;

struct FaultFrame {
    uint32_t r0, r1, r2, r3, r12, lr, pc, psr;
};

extern "C" const char *apps_locate(uint32_t addr, uint32_t *offset, bool *in_veneer);

void fault_report(FaultFrame *f, const char *kind) {
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

    // The fault STATUS registers, which say exactly what kind of fault this was
    // rather than leaving it to be inferred from a program counter. This should
    // have been the first thing recorded:
    //
    //   INVSTATE  a call landed in ARM state — a function pointer without its
    //             Thumb bit, which is the classic loader bug
    //   IACCVIOL  instruction fetch refused — the memory is not executable
    //   PRECISERR a bad data address, with BFAR holding it
    //   IMPRECIS  a deferred write fault; the PC is PAST the real instruction
    //   STKERR    the exception frame itself could not be pushed: a bad SP
    //
    // Recorded as a second line so the first still names the package and offset.
    {
        uint32_t cfsr = *(volatile uint32_t *)0xE000ED28;
        uint32_t hfsr = *(volatile uint32_t *)0xE000ED2C;
        const char *what = "?";
        if      (cfsr & (1u << 1))  what = "STKERR bad SP";
        else if (cfsr & (1u << 3))  what = "UNSTKERR";
        else if (cfsr & (1u << 0))  what = "IACCVIOL not executable";
        else if (cfsr & (1u << 7))  what = "MMARVALID";
        else if (cfsr & (1u << 9))  what = "PRECISERR bad addr";
        else if (cfsr & (1u << 10)) what = "IMPRECISERR";
        else if (cfsr & (1u << 12)) what = "STKERR bus";
        else if (cfsr & (1u << 17)) what = "INVSTATE no Thumb bit";
        else if (cfsr & (1u << 16)) what = "UNDEFINSTR";
        else if (cfsr & (1u << 18)) what = "INVPC";
        else if (cfsr & (1u << 24)) what = "UNALIGNED";
        else if (hfsr & (1u << 30)) what = "FORCED escalated";

        printf("    cfsr=0x%08lx hfsr=0x%08lx sp=%p  %s\n",
               (unsigned long)cfsr, (unsigned long)hfsr, (void *)f, what);
        // And into the log ring, which survives to be read by logdump.
        log_addf(LOG_K_ERR, "fault %s cfsr=%08lx sp=%08lx lr=%08lx",
                 what, (unsigned long)cfsr, (unsigned long)(uintptr_t)f,
                 (unsigned long)f->lr);
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

// Pick the stack the exception came from (MSP or PSP) out of EXC_RETURN, then
// hand the frame to the C reporter. Naked so the prologue cannot disturb it.
__attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
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
        "b    fault_report  \n"
        ".align 2           \n"
    );
}

extern const char fault_kind_hard[] = "HARD FAULT";

}  // extern "C"
