// Memory protection: turning silent corruption into a named fault.
//
// Two things on this device write where they should not, and neither one says
// so at the time:
//
//   * A task runs off the end of its stack. The stack is a malloc'd block, so
//     what is below it is whatever the heap handed out last — on a real board
//     that was littlefs's cache, which then got written to flash and left
//     something that would not boot until it was erased. The painted-word
//     tripwire in task.cpp catches this, but only at the NEXT yield, by which
//     point the damage is done and the code that did it is long gone.
//
//   * A package writes through a bad pointer. It runs with the same rights the
//     OS has, so it can scribble on the command table, the task table, or its
//     own machine code, and the first sign of it is a fault somewhere else
//     entirely.
//
// The MPU makes both of those faults happen AT THE INSTRUCTION, with a fault
// address, which is the difference between a bug that takes an afternoon and
// one that takes a week.
//
// This file is the encoding, and nothing else. It has no hardware headers and
// touches no registers, so the arithmetic — which is where these get wrong, and
// which is invisible on a board because a mis-encoded region simply does
// nothing — is tested on the host. mpu_rp2.cpp is the part that writes to the
// peripheral.
//
// --- the two architectures do not agree -------------------------------------
//
// ARMv8-M (Cortex-M33, RP2350) describes a region as base+limit with a 32-byte
// granule. Any size that is a multiple of 32 works, so a region fits its
// subject exactly.
//
// ARMv6-M (Cortex-M0+, RP2040) describes it as a base and a power-of-two size,
// with the base aligned to that size. A 700-byte block therefore needs a
// 1024-byte region on a 1024-byte boundary, and the alignment is paid for in
// wasted heap. That is why package W^X is enabled on RP2350 and not on RP2040:
// the same protection there would cost more RAM than the part has to spare.
// The stack guard is exempt because it is 32 bytes, which is the minimum on
// both and rounds to nothing.
#ifndef RPC_MPU_H
#define RPC_MPU_H

#include <stdint.h>

// What a region permits. Deliberately short: these are the only three shapes
// this OS has a use for, and an enum that mirrors the hardware's full AP field
// would invite encoding a combination nobody has thought about.
enum MpuPerm {
    // The guard. On ARMv6-M this really is no access at all. On ARMv8-M the AP
    // field cannot express "privileged code may not read this" — the weakest
    // it goes is read-only — so a guard there is read-only and non-executable.
    // That still catches what matters, because a stack overflow is a WRITE:
    // the push is what runs off the end, and the push is what faults.
    MPU_NO_ACCESS = 0,
    // Package code and its veneers. Readable and executable, never writable —
    // so a package with a wild pointer cannot rewrite its own instructions, and
    // neither can anything else.
    MPU_RO_EXEC,
    // Package data and bss. Writable, never executable — so a jump into a data
    // buffer faults instead of running whatever bytes are in it.
    MPU_RW_NOEXEC,
};

// --- ARMv8-M (Cortex-M33, RP2350) -------------------------------------------

// The granule. Base and limit are both held to this, which is why an allocation
// meant for a region has to be rounded to it at BOTH ends: a region whose limit
// runs past the end of the block would apply the block's permissions to
// whatever malloc put next, and marking a stranger's memory read-only is a
// fault in code that did nothing wrong.
#define MPU_V8_GRAIN 32u

struct MpuV8Region {
    uint32_t rbar;
    uint32_t rlar;
};

// Encode one region. Returns false, and writes nothing, if the request cannot
// be expressed: base not on the granule, size zero or not a multiple of it, or
// the range wrapping the address space. Refusing is the whole point — a region
// silently rounded to something else protects the wrong memory and every test
// still passes.
bool mpu_v8_encode(uint32_t base, uint32_t size, MpuPerm perm, MpuV8Region *out);

// --- ARMv6-M (Cortex-M0+, RP2040) -------------------------------------------

#define MPU_V6_MIN_SIZE 32u

struct MpuV6Region {
    uint32_t rbar;
    uint32_t rasr;
};

// Encode one region, including the region number — ARMv6-M carries it in RBAR
// alongside a VALID bit rather than in a separate select register, so the
// number is part of the encoding here and not part of the write.
//
// `size` must be a power of two of at least 32, and `base` must be aligned to
// it. Both are the architecture's rules, not this code's.
bool mpu_v6_encode(uint32_t base, uint32_t size, uint8_t region, MpuPerm perm,
                   MpuV6Region *out);

// --- sizing an allocation that is going to become a region ------------------
//
// A block that is going to be protected cannot just be malloc'd: the region has
// to start on an aligned address and cover a whole number of granules, and
// malloc promises 8-byte alignment and nothing else. So the slack is asked for
// up front and the useful part is aligned inside it.
//
// This is one function per architecture rather than three loose helpers because
// the three numbers have to agree with each other. Working them out separately
// at each call site is how a region ends up 32 bytes longer than its block.

struct MpuBlockPlan {
    uint32_t alloc_bytes;    // hand this to malloc
    uint32_t align;          // round the pointer it returns up to this
    uint32_t region_bytes;   // the region then covers this much, from there
};

// Round up to the next multiple of `align`, which must be a power of two.
// Saturates rather than wrapping: a rounded-DOWN address would put a region
// somewhere it was never meant to be.
uint32_t mpu_align_up(uint32_t v, uint32_t align);

// False if `want` is zero or so large that the rounding overflows.
bool mpu_v8_plan_block(uint32_t want, MpuBlockPlan *out);
bool mpu_v6_plan_block(uint32_t want, MpuBlockPlan *out);

// --- region numbering -------------------------------------------------------
//
// Fixed, not allocated. Eight regions is few enough that a first-fit allocator
// would be more code than the thing it manages, and a fixed map means a region
// programmed by mistake collides loudly with a known owner instead of quietly
// with whatever happened to be free.
enum {
    MPU_RGN_STACK_GUARD = 0,   // ARMv6-M only; ARMv8-M uses MSPLIM instead
    MPU_RGN_APP_TEXT    = 1,   // package code + rodata: read-only, executable
    MPU_RGN_APP_DATA    = 2,   // package data + bss:    writable, never executed
    MPU_RGN_APP_VENEER  = 3,   // the loader's trampolines: read-only, executable
    // The two more a SANDBOXED package needs. Without the default memory map —
    // and unprivileged code never gets it — a package cannot reach its own
    // stack or its own heap unless they are described here.
    MPU_RGN_APP_STACK   = 4,   // its own stack:  writable, never executed
    MPU_RGN_APP_ARENA   = 5,   // what fw_malloc gives it: the same
    MPU_RGN_FIRST_FREE  = 6,   // 6 and 7 are unspoken for
};

// --- the hardware side, which lives in mpu_rp2.cpp --------------------------
//
// Declared here so there is one header rather than two, but implemented only
// on the device. Nothing in the host tests calls these.

// Bring this core's protection hardware up. Called once per core, and once per
// core only — the registers are not shared between them.
extern "C" void mpu_platform_init(void);

// What is actually programmed right now, for `mpu` to print. Read from what
// was recorded at configuration time rather than from the registers, because a
// core can only read its own.
struct MpuReport {
    bool     ready;           // the hardware came up and is enabled
    uint32_t regions;         // how many the part has
    uint32_t guard_at;        // the address the stack guard is set to
    bool     app_active;      // package regions are programmed right now
    bool     uses_msplim;     // stack guard is the limit register, not a region
    bool     app_supported;   // package regions are enforced on this part
};
extern "C" bool mpu_report(unsigned core, MpuReport *out);

#endif  // RPC_MPU_H
