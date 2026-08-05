// Memory protection, the half that touches the peripheral.
//
// core/mpu.cpp works out what to write; this writes it. Everything here is
// per-core: the RP2040 and RP2350 each carry two processors with their own
// protection hardware, so a region programmed on core 0 does not exist on
// core 1. That is why mpu_platform_init runs on both, and why the stack guard
// is re-applied every time a task resumes rather than once when it is created.
//
// --- how each part enforces a stack guard ------------------------------------
//
// ARMv8-M (RP2350) has MSPLIM: a register holding the lowest address the stack
// may reach. A push below it raises a UsageFault naming the exact instruction,
// costs one `msr` to change, and leaves all eight protection regions free. It
// is strictly the better mechanism and it is used wherever it exists.
//
// ARMv6-M (RP2040) has no such register, so it spends region 0 on a 32-byte
// no-access block at the bottom of the stack. Slightly weaker — it catches a
// write into the block rather than the stack pointer crossing a line — and it
// costs a region.
//
// --- why faults are left to escalate -----------------------------------------
//
// Neither MemManage nor UsageFault is enabled separately. With SHCSR's enable
// bits clear both escalate into HardFault, which this OS already handles, and
// CFSR still records exactly which kind of fault it was — so the diagnosis is
// no worse and there is one handler instead of three. The device reboots either
// way; what matters is that it says why first.
#include <stdio.h>
#include "core/mpu.h"
#include "core/task.h"
#include "core/lock.h"      // task_irq_save / task_irq_restore

#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/structs/mpu.h"
#include "hardware/sync.h"

// The linker's idea of where each core's boot stack ends. Core 0's task (pid 1)
// runs on __StackBottom..__StackTop and core 1's scheduler loop on the other,
// and neither was allocated by this OS — so a null `bottom` is resolved here
// rather than being something the scheduler has to know.
extern "C" char __StackBottom;
extern "C" char __StackOneBottom;

// Whether the hardware is usable at all. False before init, and false forever
// if the part reports no regions — better to run unprotected than to write
// configuration registers that are not there.
static bool g_ready[2];
// What is currently programmed, for the `mpu` command. Per core, because that
// is the only honest way to report it — and recorded at init rather than read
// back on demand, because the registers only ever describe the core doing the
// reading. Core 0 asking the hardware about core 1 gets its own answer.
static uint32_t g_guard_at[2];
static bool     g_app_active[2];
static uint8_t  g_regions[2];

#if defined(__ARM_ARCH_6M__) || PICO_RP2040
  #define MPU_V6 1
#else
  #define MPU_V6 0
#endif

static inline unsigned this_core(void) { return get_core_num() & 1u; }

// Any change to protection configuration needs both barriers before the next
// instruction is fetched: DSB so the write has landed, ISB so nothing already
// in the pipeline runs under the old rules. Leaving these out gives a window of
// a few instructions where the wrong configuration applies — which is
// unobservable in a test and occasionally fatal in the field.
static inline void mpu_sync(void) {
    __dsb();
    __isb();
}

// --- setup ------------------------------------------------------------------

extern "C" void mpu_platform_init(void) {
    unsigned c = this_core();

    // Does this part actually have regions? DREGION is read-only and reports
    // the count. Eight on both, but reading it beats assuming it.
    uint32_t regions = (mpu_hw->type >> 8) & 0xFFu;
    if (regions < MPU_RGN_FIRST_FREE) { g_ready[c] = false; return; }

    // Start from nothing. The SDK's own per-core stack guard would have claimed
    // region 0 and set ctrl before this ran, but it is disabled by default
    // (PICO_USE_STACK_GUARDS is 0) and this OS does not turn it on — so the
    // hardware is untouched here and clearing is a formality that keeps it true
    // if that ever changes.
    mpu_hw->ctrl = 0;
    mpu_sync();
    for (uint32_t r = 0; r < regions; r++) {
        mpu_hw->rnr = r;
#if MPU_V6
        mpu_hw->rbar = (1u << 4) | r;      // VALID, so RNR is not consulted
        mpu_hw->rasr = 0;                  // disabled
#else
        mpu_hw->rbar = 0;
        mpu_hw->rlar = 0;                  // EN clear
#endif
    }

#if !MPU_V6
    // Attribute 0: Normal memory, Outer and Inner write-back, read and write
    // allocate. Chosen to match what the default memory map already says about
    // SRAM rather than to describe anything new — see core/mpu.cpp.
    mpu_hw->mair[0] = 0xFFu;
    mpu_hw->mair[1] = 0;
#endif

    // PRIVDEFENA: where no region matches, privileged code keeps the default
    // memory map. Without it, enabling the MPU would deny the OS access to
    // everything it has not explicitly described — which is all of it.
    mpu_hw->ctrl = (1u << 2) | 1u;         // PRIVDEFENA | ENABLE
    mpu_sync();

    g_ready[c]      = true;
    g_guard_at[c]   = 0;
    g_app_active[c] = false;
    g_regions[c]    = (uint8_t)regions;
}

// --- the stack guard --------------------------------------------------------

extern "C" void task_stack_guard_set(const void *bottom, uint32_t size) {
    unsigned c = this_core();
    if (!g_ready[c]) return;

    // Null means this core's own boot stack: pid 1 runs on core 0's, and the
    // scheduler loop on whichever core it is idling.
    uintptr_t base;
    if (bottom) {
        base = (uintptr_t)bottom;
    } else {
        base = c == 0 ? (uintptr_t)&__StackBottom : (uintptr_t)&__StackOneBottom;
    }
    (void)size;

#if MPU_V6
    // Round UP to the granule, never down. Down would put the guard in the
    // previous heap allocation — protecting a stranger's memory and leaving
    // this task's own bottom bytes wide open, which is the worst of both.
    uint32_t guard = mpu_align_up((uint32_t)base, MPU_V6_MIN_SIZE);
    MpuV6Region r;
    if (!mpu_v6_encode(guard, MPU_V6_MIN_SIZE, MPU_RGN_STACK_GUARD,
                       MPU_NO_ACCESS, &r)) {
        return;                            // unencodable: leave the last one
    }
    mpu_hw->rnr  = MPU_RGN_STACK_GUARD;
    mpu_hw->rbar = r.rbar;
    mpu_hw->rasr = r.rasr;
    mpu_sync();
    g_guard_at[c] = guard;
#else
    // MSPLIM ignores its low three bits, so the limit is rounded up to keep it
    // inside the block rather than one word below it.
    uint32_t limit = mpu_align_up((uint32_t)base, 8);
    __asm volatile ("msr msplim, %0" :: "r" (limit) : "memory");
    __isb();
    g_guard_at[c] = limit;
#endif
}

// --- package regions --------------------------------------------------------

#if !MPU_V6
// What each region was last ASKED for, per core, and what was written for it.
//
// A region is described by two registers, and a fault report can only read what
// the hardware ended up holding. When those two disagree with each other the
// report says a region is 83 kilobytes when 12 were requested, and there is no
// way from that alone to tell whether the caller passed something wrong or the
// programming did — which is a whole debugging round per crash.
//
// So the intent is recorded next to the act. The dump prints both and says
// which of the two is wrong.
struct RegionShadow {
    uint32_t base, size, rbar, rlar;
};
static RegionShadow g_shadow[2][8];
// Region number plus one while a region is mid-write, zero otherwise. A fault
// that lands here says the state the dump is reading is a half-finished write
// rather than something that persisted.
static volatile uint8_t g_prog[2];

static void set_region(uint32_t rgn, const void *base, uint32_t size, MpuPerm perm) {
    MpuV8Region r;
    unsigned c = this_core();
    RegionShadow *sh = &g_shadow[c][rgn & 7u];
    sh->base = (uint32_t)(uintptr_t)base;
    sh->size = size;

    g_prog[c] = (uint8_t)(rgn + 1);
    mpu_hw->rnr = rgn;

    // DISABLE BEFORE CHANGING, always.
    //
    // A region is a base register and a limit register, and the enable bit
    // lives in the limit. Writing the base first therefore leaves the region
    // ENABLED, with the new base and the PREVIOUS limit, for as long as it
    // takes to reach the next store — a span the hardware will happily enforce
    // if an exception arrives inside it.
    //
    // What it enforces then is not a small error. The old limit belongs to a
    // different allocation, so the region stretches from one block to wherever
    // the last one ended: tens of kilobytes, overlapping whatever lies between,
    // and ARMv8-M calls overlapping regions UNPREDICTABLE. An access the real
    // region would have allowed can be refused, and the refusal is reported
    // against an address that is plainly inside a region granting it.
    //
    // Clearing the limit first costs one store and means the intermediate state
    // is a region that does not exist rather than one describing the wrong
    // memory. Where no region matches, privileged code still has the default
    // map (PRIVDEFENA) and unprivileged code is denied — denial being the safe
    // direction, and unreachable in practice because callers mask interrupts.
    mpu_hw->rlar = 0;
    __dsb();

    if (!base || size == 0 || !mpu_v8_encode((uint32_t)(uintptr_t)base, size, perm, &r)) {
        mpu_hw->rbar = 0;                  // EN already clear: no region rather
        sh->rbar = sh->rlar = 0;           // than a wrong one
        g_prog[c] = 0;
        return;
    }
    mpu_hw->rbar = r.rbar;
    mpu_hw->rlar = r.rlar;
    sh->rbar = r.rbar;
    sh->rlar = r.rlar;
    g_prog[c] = 0;
}
#endif

extern "C" void task_app_mem_apply(const TaskAppMem *mem) {
    unsigned c = this_core();
    if (!g_ready[c]) return;

    // Five regions are one description, not five. arm_protection already masks
    // around its call; this covers task_app_mem_set and task_app_mem_clear,
    // which reach here directly. The pairs nest.
    unsigned irq = task_irq_save();

#if MPU_V6
    // Not enforced on ARMv6-M.
    //
    // Its regions are power-of-two sized and aligned to their own size, so a
    // package's 5 KB of code would need an 8 KB region on an 8 KB boundary and
    // the allocation to match — up to 16 KB of heap for 5 KB of package, twice
    // over for the data block. On the part with 264 KB that is a worse trade
    // than the protection is worth, and making it conditional at runtime would
    // mean two behaviours to reason about instead of one.
    //
    // The stack guard above is unaffected: it is 32 bytes, which is the
    // minimum region on both architectures and rounds to nothing.
    (void)mem;
    g_app_active[c] = false;
#else
    // Code read-only, data never executable. The veneers are code the loader
    // wrote, and they are protected for a reason beyond tidiness: they are the
    // only way a package reaches the firmware, so leaving them writable would
    // let a package point one at an address of its choosing.
    //
    // A null `mem` withdraws all five rather than taking a separate path, so
    // there is one sequence to reason about and one place the mask is dropped.
    set_region(MPU_RGN_APP_TEXT,   mem ? mem->text   : nullptr,
                                   mem ? mem->text_size   : 0, MPU_RO_EXEC);
    set_region(MPU_RGN_APP_DATA,   mem ? mem->data   : nullptr,
                                   mem ? mem->data_size   : 0, MPU_RW_NOEXEC);
    set_region(MPU_RGN_APP_VENEER, mem ? mem->veneer : nullptr,
                                   mem ? mem->veneer_size : 0, MPU_RO_EXEC);
    // Its stack and its heap. Both zero for a package running with the OS's own
    // privileges, which reaches them through the default map like everything
    // else — the regions only mean anything once the default map is gone.
    set_region(MPU_RGN_APP_STACK,  mem ? mem->stack  : nullptr,
                                   mem ? mem->stack_size  : 0, MPU_RW_NOEXEC);
    set_region(MPU_RGN_APP_ARENA,  mem ? mem->arena  : nullptr,
                                   mem ? mem->arena_size  : 0, MPU_RW_NOEXEC);
    mpu_sync();
    g_app_active[c] = mem != nullptr;
#endif
    task_irq_restore(irq);
}

// --- reporting --------------------------------------------------------------

extern "C" bool mpu_report(unsigned core, MpuReport *out) {
    if (!out || core > 1) return false;
    out->ready         = g_ready[core];
    out->guard_at      = g_guard_at[core];
    out->app_active    = g_app_active[core];
    out->regions       = g_regions[core];
    out->uses_msplim   = !MPU_V6;
    out->app_supported = !MPU_V6;
    return true;
}

// --- what the hardware actually held, at the moment it faulted ---------------
//
// A protection fault says an access was refused. It does not say by WHICH
// region, or which regions were programmed at the time — and when the answer is
// "not the ones this task was given", every other reading of the fault is
// wrong. Two rounds were spent on stack sizes for a fault that was never about
// size.
//
// So the registers are read straight out of the hardware and printed. No
// interpretation, because interpretation is what kept going wrong.
extern "C" void mpu_dump_live(unsigned sp) {
#if MPU_V6
    (void)sp;
    printf("    mpu: ARMv6-M, app regions not enforced here\n");
#else
    // SNAPSHOT FIRST, print second, with interrupts off across the read.
    //
    // Printing goes over USB, which is serviced by a task, which means another
    // task can be scheduled in the middle of the dump — and reprogram the very
    // registers being read. The first version of this printed as it went and
    // produced a region whose base came from one task and whose limit came from
    // another: eighty-three kilobytes of arena where twelve were allocated, and
    // an overlap that was never really there.
    //
    // A torn read of the thing you are debugging is worse than no read, because
    // it looks like evidence.
    uint32_t ctrl;
    uint32_t rbar[8], rlar[8];
    RegionShadow shadow[8];
    uint8_t prog;
    unsigned c = this_core();
    {
        uint32_t primask;
        __asm volatile ("mrs %0, primask \n cpsid i" : "=r"(primask) :: "memory");
        ctrl = mpu_hw->ctrl;
        prog = g_prog[c];
        for (uint32_t r = 0; r < 8; r++) {
            mpu_hw->rnr = r;
            rbar[r] = mpu_hw->rbar;
            rlar[r] = mpu_hw->rlar;
            shadow[r] = g_shadow[c][r];
        }
        if (!primask) __asm volatile ("cpsie i" ::: "memory");
    }

    printf("    mpu ctrl=0x%08lx  (enabled=%lu privdefena=%lu)\n",
           (unsigned long)ctrl, (unsigned long)(ctrl & 1u),
           (unsigned long)((ctrl >> 2) & 1u));

    // Was a region mid-write when this happened? That single bit separates "the
    // dump caught a half-finished write" from "this state was live while the
    // package ran", and the two have completely different causes.
    if (prog)
        printf("    region %u WAS MID-WRITE — the state below is unfinished\n",
               (unsigned)(prog - 1));
    else
        printf("    no region was mid-write: this state was live\n");

    bool covered = false;
    uint32_t hits = 0;
    for (uint32_t r = 0; r < 8; r++) {
        if (!(rlar[r] & 1u)) continue;                  // not enabled
        uint32_t base  = rbar[r] & ~0x1Fu;
        uint32_t limit = (rlar[r] & ~0x1Fu) | 0x1Fu;    // inclusive last byte
        bool has = sp >= base && sp <= limit;
        if (has) { covered = true; hits++; }
        printf("    region %lu: 0x%08lx..0x%08lx (%lu B) ap=%lu xn=%lu%s\n",
               (unsigned long)r, (unsigned long)base, (unsigned long)limit,
               (unsigned long)(limit - base + 1),
               (unsigned long)((rbar[r] >> 1) & 3u), (unsigned long)(rbar[r] & 1u),
               has ? "   <- sp is here" : "");
        // What was asked for, when the hardware does not match it. This is the
        // line that says whether the caller or the programming is at fault, and
        // it is why the two are recorded separately.
        if (rbar[r] != shadow[r].rbar || rlar[r] != shadow[r].rlar)
            printf("      asked for 0x%08lx + %lu B  (rbar %08lx/%08lx rlar %08lx/%08lx)"
                   "  MISMATCH\n",
                   (unsigned long)shadow[r].base, (unsigned long)shadow[r].size,
                   (unsigned long)shadow[r].rbar, (unsigned long)rbar[r],
                   (unsigned long)shadow[r].rlar, (unsigned long)rlar[r]);
        else if (shadow[r].size && shadow[r].size != limit - base + 1)
            printf("      asked for 0x%08lx + %lu B  — THE CALLER PASSED THIS\n",
                   (unsigned long)shadow[r].base, (unsigned long)shadow[r].size);
    }
    if (!covered)
        printf("    NO ENABLED REGION COVERS SP — that is the fault, not a size\n");
    if (hits > 1)
        printf("    %lu REGIONS OVERLAP AT SP — ARMv8-M calls that UNPREDICTABLE\n",
               (unsigned long)hits);
#endif
}
