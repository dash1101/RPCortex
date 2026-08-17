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
//
// `quiet` asks for the log ring only. A contained fault does as little as
// possible inside the handler and reports itself afterwards from task context,
// so the printing half of this belongs to the fatal path alone.
extern "C" void __attribute__((weak)) fault_report_stacks(uint32_t sp, int quiet) {
    (void)sp; (void)quiet;
}

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

// --- a stack of the handler's own --------------------------------------------
//
// This handler used to run on the stack of whatever faulted. For a fault that
// ends in a reset that is only untidy; for a STACK OVERFLOW it is the reason
// the overflow could not be survived. The guard is MSPLIM, the handler has to
// clear it to run at all, and everything it does from then on writes below a
// stack that had already run out — into whatever the heap put there. Resetting
// two seconds later hid the damage. Carrying on would not.
//
// So the handler stands on its own memory instead. One per core, because both
// can fault, indexed by CPUID straight out of SIO. Nothing else uses these, so
// the depth reached is a true measure of what a fault report costs, and it is
// reported rather than assumed — see fault_stack_used below.
#define FAULT_STACK_WORDS  1024                 // 4 KB per core
#define FAULT_PAINT        0xA5A5A5A5u

// A dead band UNDER each of them, and it is not decoration.
//
// The two stacks are one array, so core 1's grows down into core 0's and core
// 0's grows down into whatever the linker put before the array. A handler that
// outgrew its 4 KB would therefore corrupt the other core's crash stack — the
// exact failure this whole arrangement exists to stop a package doing — and
// nothing would say so afterwards, because "used the whole stack" and "used more
// than the stack" read identically from the paint.
//
// The band is what makes them different. Thirty-two bytes below each stack that
// nothing is supposed to touch: an overflow lands there first, and it is still
// there to be read at the next `mpu`. It cannot be MSPLIM — the handler runs
// with the limit released, because releasing it is how the handler is reachable
// at all after a stack overflow — so this is evidence rather than prevention,
// which is the right trade for a path that must never fault twice.
#define FAULT_GUARD_WORDS  8                    // 32 bytes below each stack
#define FAULT_ROW_WORDS    (FAULT_GUARD_WORDS + FAULT_STACK_WORDS)

static uint32_t g_fault_stack[2][FAULT_ROW_WORDS] __attribute__((aligned(8)));

// Read by the assembly below. Pointers rather than integers so each is a
// link-time constant in .rodata and needs no startup code to be correct — a
// fault before main() must still land somewhere valid.
//
// TWO SYMBOLS RATHER THAN AN ARRAY. Indexing one would mean scaling the core
// number, and every Thumb-1 way of writing that (`lsls`, `adds`) is UAL-only
// while inline assembly is assembled in divided syntax — so the RP2040 build
// refuses it. A compare and a branch pick between two names instead, which is
// plain Thumb in either syntax and needs no directive that would leak into
// everything the compiler emits afterwards.
//
// Explicitly `extern`, for the same reason fault_kind_hard is: a `const` at
// namespace scope has INTERNAL linkage in C++, so the assembly cannot see a
// symbol that is plainly right there in the same file.
extern uint32_t * const g_fault_stack_top0;
extern uint32_t * const g_fault_stack_top1;
extern uint32_t * const g_fault_stack_top0 = &g_fault_stack[0][FAULT_ROW_WORDS];
extern uint32_t * const g_fault_stack_top1 = &g_fault_stack[1][FAULT_ROW_WORDS];

// Painted at boot so the high-water mark can be read afterwards. Deliberately
// not a static initialiser: 8 KB of 0xA5 in .data is 8 KB of flash, and this
// firmware is watching its flash.
void fault_stack_init(void) {
    for (int c = 0; c < 2; c++)
        for (uint32_t i = 0; i < FAULT_ROW_WORDS; i++)
            g_fault_stack[c][i] = FAULT_PAINT;
}

// How deep any fault report has ever gone, in bytes. Zero before the first one.
//
// Counted from the top of the guard band, so the answer is depth into the STACK
// and never includes anything written below it. A report that went past the
// bottom reads as the full size here and is named by fault_stack_overflowed.
uint32_t fault_stack_used(int core) {
    if (core < 0 || core > 1) return 0;
    uint32_t untouched = 0;
    while (untouched < FAULT_STACK_WORDS &&
           g_fault_stack[core][FAULT_GUARD_WORDS + untouched] == FAULT_PAINT) untouched++;
    return (FAULT_STACK_WORDS - untouched) * 4;
}

// Did a crash report run off the bottom of the stack it was given?
//
// The distinction fault_stack_used cannot make on its own: 4096 of 4096 means
// either "it fitted exactly" or "it did not fit and wrote into the other core's
// crash stack", and those are a measurement and a serious bug. Any word of the
// band below the stack being gone is the second one.
bool fault_stack_overflowed(int core) {
    if (core < 0 || core > 1) return false;
    for (uint32_t i = 0; i < FAULT_GUARD_WORDS; i++)
        if (g_fault_stack[core][i] != FAULT_PAINT) return true;
    return false;
}

uint32_t fault_stack_size(void) { return FAULT_STACK_WORDS * 4; }

// Set while an app is running, so a fault can say WHOSE fault it was.
volatile const char *g_current_app = nullptr;

struct FaultFrame {
    uint32_t r0, r1, r2, r3, r12, lr, pc, psr;
};

extern "C" const char *apps_locate(uint32_t addr, uint32_t *offset, bool *in_veneer);

// --- what a contained fault leaves behind ------------------------------------
//
// THE HANDLER RECORDS; TASK CONTEXT REPORTS.
//
// A fault that ends in a reset can afford to print from the handler, because
// there is nothing else left to do with the device. A fault that is CONTAINED
// cannot: the report is printed on the faulting package's own stack, which is
// the stack the machine is about to carry on using, and printf is not a small
// thing to run there. On this port it reaches the stdio mutex, the alarm pool's
// spinlock, and TinyUSB's device task — thousands of instructions and a
// kilobyte or two of stack, at a priority where no interrupt can service any of
// it, on a stack whose remaining depth is exactly what is in question.
//
// So the contained path stores these plain words and returns. The values are
// printed once, later, by whoever notices the package was stopped — where
// printing is ordinary and safe.
static struct {
    bool     pending;
    uint32_t cfsr, addr, pc, lr, sp;
    bool     have_addr;
    const char *what;          // a literal; safe to keep the pointer
    char     where[40];        // "havoc+1b28", built by hand below
} g_contained;

static void copy_where(const char *src) {
    unsigned n = 0;
    while (n + 1 < sizeof(g_contained.where) && src[n]) {
        g_contained.where[n] = src[n];
        n++;
    }
    g_contained.where[n] = 0;
}

// Print the last contained fault, if there is one, and forget it. Called from
// task context by the code that reports the package was stopped.
int fault_report_contained(void) {
    if (!g_contained.pending) return 0;
    g_contained.pending = false;
    printf("\n*** FAULT IN A PACKAGE — contained ***\n");
    printf("    %s\n", g_contained.where);
    printf("    %s\n", g_contained.what);
    printf("    pc  = 0x%08lx   lr  = 0x%08lx   sp = 0x%08lx\n",
           (unsigned long)g_contained.pc, (unsigned long)g_contained.lr,
           (unsigned long)g_contained.sp);
    printf("    cfsr= 0x%08lx", (unsigned long)g_contained.cfsr);
    if (g_contained.have_addr)
        printf("   faulting address = 0x%08lx", (unsigned long)g_contained.addr);
    printf("\n    the package was stopped; the device is still running\n\n");
    return 1;
}

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
    uint32_t cfsr, addr;
    bool have_addr;
    const char *what;
    {
        cfsr = *(volatile uint32_t *)0xE000ED28;
        uint32_t hfsr = *(volatile uint32_t *)0xE000ED2C;
        uint32_t mmfar = *(volatile uint32_t *)0xE000ED34;
        uint32_t bfar  = *(volatile uint32_t *)0xE000ED38;
        what = "?";
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

        // CAN THE FRAME BE TRUSTED? That, and not the kind of fault, is what
        // decides whether this may be contained.
        //
        // Containing means reading eight words the hardware pushed and writing
        // two of them back. MSTKERR says that push FAILED — the memory under
        // the stack pointer was not writable — so those eight words are not
        // the frame, they are whatever was already there. Rewriting them aims
        // an exception return at a value nobody chose. That stays fatal.
        //
        // STKOF is a different failure wearing the same word. The stack pointer
        // went below its limit, the instruction that did it was REFUSED, and
        // the frame was stacked normally. Only the room BELOW it is gone —
        // which is survivable now that the handler runs on a stack of its own
        // and the unwind out of a package uses none of the package's.
        //
        // Until both of those were true this had to refuse an overflow as well,
        // because everything it did ran off the bottom of the exhausted stack
        // into whatever the heap put there. Resetting hid that; carrying on
        // would not have.
        stack_intact = !(cfsr & (1u << 4));      // MSTKERR

        bool have_mm = (cfsr & (1u << 7)) != 0;      // MMARVALID
        bool have_bf = (cfsr & (1u << 15)) != 0;     // BFARVALID
        have_addr = have_mm || have_bf;
        addr = have_mm ? mmfar : have_bf ? bfar : 0;
        // Into the log ring, which survives to be read by logdump. This is the
        // one line that is written either way, before anything is decided.
        log_addf(LOG_K_ERR, "fault %s cfsr=%08lx addr=%08lx sp=%08lx lr=%08lx",
                 what, (unsigned long)cfsr, (unsigned long)addr,
                 (unsigned long)(uintptr_t)f, (unsigned long)f->lr);
    }

    // DECIDE BEFORE PRINTING.
    //
    // Everything below this point runs on the way to a reset, and printing on
    // the way to a reset is free. Printing on the way BACK is not: it happens
    // on the faulting package's stack, which the machine is about to keep
    // using, and on this port printf reaches the stdio mutex, the alarm pool's
    // spinlock and TinyUSB's device task — none of which can be serviced at
    // fault priority, and all of which want stack whose remaining depth is
    // precisely the thing in doubt.
    //
    // So a contained fault records what happened and returns. The report is
    // printed a moment later from task context by whoever notices the package
    // was stopped, where printing is ordinary.
    //
    // The frame is read BEFORE the attempt, because containing rewrites it.
    // sandbox_abandon_call points the stacked PC at app_call_unpriv_tail, so
    // reading f->pc afterwards reported the address the unwind was aimed at
    // rather than the instruction that faulted — "pc = 0x100064cb" under a
    // heading that correctly said "havoc+1b28". Two different answers to the
    // same question in the same report, one of them wrong.
    uint32_t pc = f->pc, lr = f->lr, sp = (uint32_t)(uintptr_t)f;

    if (stack_intact && fault_try_contain((uint32_t *)f)) {
        g_contained.cfsr = cfsr;
        g_contained.addr = addr;
        g_contained.have_addr = have_addr;
        g_contained.pc = pc;
        g_contained.lr = lr;
        g_contained.sp = sp;
        g_contained.what = what;
        copy_where(note);
        g_contained.pending = true;

        // The two numbers that say whether this handler had room to run: how
        // far the fault was from the bottom of the package's stack, and how
        // deep anything ever went. Log only — see above.
        fault_report_stacks((uint32_t)(uintptr_t)f, /*quiet*/1);

        // CLEAR THE FAULT REGISTERS. CFSR bits are sticky and MMFAR keeps its
        // last value, and nothing cleared them before because every fault ended
        // in a reset. Now that one can be survived, the next report inherits
        // them: a stack overflow was reported as "addr=10000000", the address
        // from a DACCVIOL two commands earlier, which is a lie about the one
        // number in that report that comes from hardware.
        *(volatile uint32_t *)0xE000ED28 = *(volatile uint32_t *)0xE000ED28;  // CFSR, w1c
        *(volatile uint32_t *)0xE000ED2C = *(volatile uint32_t *)0xE000ED2C;  // HFSR, w1c

        // The last thing this handler does. If the next report stops here, the
        // decision was right and the RESUME is what failed — a distinction
        // worth having, because they are different bugs in different files.
        bb_note_phase("fault: resuming into the unwind");
        return 1;
    }

    printf("    cfsr=0x%08lx sp=%p  %s\n",
           (unsigned long)cfsr, (void *)f, what);
    if (have_addr)
        printf("    faulting address = 0x%08lx\n", (unsigned long)addr);
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
    fault_report_stacks((uint32_t)(uintptr_t)f, /*quiet*/0);

    printf("    resetting in 2s\n\n");
    // Flush before the reset, or the report never leaves the USB buffer.
    //
    // BUSY-WAIT, NOT sleep_ms. sleep_ms goes through the default alarm pool: a
    // hardware spinlock, an alarm the IRQ for which cannot possibly fire at
    // fault priority, and a WFE waiting for it. Whether it returned at all was
    // never tested — a hang inside it and a successful reboot both arrive as
    // "the watchdog reset the device", so the two were never told apart.
    // busy_wait_us_32 reads the timer and nothing else.
    busy_wait_us_32(2000u * 1000u);
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
        // STAND SOMEWHERE ELSE BEFORE DOING ANYTHING.
        //
        // Nothing has been pushed yet, so SP is still exactly the frame the
        // hardware stacked — which is what the exception return needs it to be,
        // and why it is saved rather than recomputed. EXC_RETURN goes with it:
        // this used to ride home in a stacked LR, and there is no stack to
        // stack it on any more.
        //
        // r4 and r5 are safe to lose. The only path that comes back from here
        // is a contained fault, which returns through app_call_unpriv_abandon
        // and its `pop {r3, r4-r11, pc}`; everything else reboots.
        "mov  r4, lr        \n"
        "mov  r5, sp        \n"
        // Which core, and therefore which of the two stacks. See the note on
        // the symbols for why this is a branch and not an indexed load.
        "ldr  r2, =0xd0000000 \n"       // SIO. CPUID is the first word.
        "ldr  r2, [r2]      \n"
        "cmp  r2, #0        \n"
        "beq  3f            \n"
        "ldr  r3, =g_fault_stack_top1 \n"
        "b    4f            \n"
        "3:                 \n"
        "ldr  r3, =g_fault_stack_top0 \n"
        "4:                 \n"
        "ldr  r3, [r3]      \n"
        "mov  sp, r3        \n"
        "ldr  r1, =fault_kind_hard \n"
        // Was a tail branch, because the reporter never came back. It can now:
        // a fault inside a package is contained rather than fatal, and then the
        // exception return has to happen.
        "bl   fault_report  \n"
        "mov  sp, r5        \n"
        "bx   r4            \n"
        ".align 2           \n"
    );
}

extern const char fault_kind_hard[] = "HARD FAULT";

}  // extern "C"
