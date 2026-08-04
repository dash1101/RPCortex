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
#include "core/mpu.h"
#include "core/task.h"

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
static void set_region(uint32_t rgn, const void *base, uint32_t size, MpuPerm perm) {
    MpuV8Region r;
    mpu_hw->rnr = rgn;
    if (!base || size == 0 || !mpu_v8_encode((uint32_t)(uintptr_t)base, size, perm, &r)) {
        mpu_hw->rbar = 0;
        mpu_hw->rlar = 0;                  // EN clear: no region rather than a wrong one
        return;
    }
    mpu_hw->rbar = r.rbar;
    mpu_hw->rlar = r.rlar;
}
#endif

extern "C" void task_app_mem_apply(const TaskAppMem *mem) {
    unsigned c = this_core();
    if (!g_ready[c]) return;

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
    if (!mem) {
        set_region(MPU_RGN_APP_TEXT,   nullptr, 0, MPU_RO_EXEC);
        set_region(MPU_RGN_APP_DATA,   nullptr, 0, MPU_RW_NOEXEC);
        set_region(MPU_RGN_APP_VENEER, nullptr, 0, MPU_RO_EXEC);
        mpu_sync();
        g_app_active[c] = false;
        return;
    }
    // Code read-only, data never executable. The veneers are code the loader
    // wrote, and they are protected for a reason beyond tidiness: they are the
    // only way a package reaches the firmware, so leaving them writable would
    // let a package point one at an address of its choosing.
    set_region(MPU_RGN_APP_TEXT,   mem->text,   mem->text_size,   MPU_RO_EXEC);
    set_region(MPU_RGN_APP_DATA,   mem->data,   mem->data_size,   MPU_RW_NOEXEC);
    set_region(MPU_RGN_APP_VENEER, mem->veneer, mem->veneer_size, MPU_RO_EXEC);
    mpu_sync();
    g_app_active[c] = true;
#endif
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
