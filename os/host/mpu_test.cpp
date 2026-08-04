// Encoding an MPU region.
//
// This is tested here rather than on a board because a mis-encoded region is
// invisible from the device's side. It does not fault, it does not warn, it
// simply protects the wrong bytes — or none — and everything appears to work
// until the day something writes where it should not and nothing stops it.
//
// The checks below are about the two things that actually go wrong: the field
// arithmetic, and the refusals. A region silently rounded to fit is worse than
// no region at all, because it looks like protection.
#include "../core/mpu.h"

#include <stdio.h>

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

// Field extraction, written out rather than shared with the encoder so a wrong
// shift in one is not cancelled by the same wrong shift in the other.
static uint32_t v8_base(const MpuV8Region &r)  { return r.rbar & 0xFFFFFFE0u; }
static uint32_t v8_sh(const MpuV8Region &r)    { return (r.rbar >> 3) & 0x3u; }
static uint32_t v8_ap(const MpuV8Region &r)    { return (r.rbar >> 1) & 0x3u; }
static uint32_t v8_xn(const MpuV8Region &r)    { return r.rbar & 1u; }
static uint32_t v8_limit(const MpuV8Region &r) { return r.rlar & 0xFFFFFFE0u; }
static uint32_t v8_en(const MpuV8Region &r)    { return r.rlar & 1u; }

static uint32_t v6_addr(const MpuV6Region &r)   { return r.rbar & 0xFFFFFFE0u; }
static uint32_t v6_region(const MpuV6Region &r) { return r.rbar & 0xFu; }
static uint32_t v6_valid(const MpuV6Region &r)  { return (r.rbar >> 4) & 1u; }
static uint32_t v6_enable(const MpuV6Region &r) { return r.rasr & 1u; }
static uint32_t v6_size(const MpuV6Region &r)   { return (r.rasr >> 1) & 0x1Fu; }
static uint32_t v6_ap(const MpuV6Region &r)     { return (r.rasr >> 24) & 0x7u; }
static uint32_t v6_xn(const MpuV6Region &r)     { return (r.rasr >> 28) & 1u; }

int main(void) {
    printf("mpu_test - encoding a protection region\n");

    // ---------------------------------------------------------------- ARMv8-M
    MpuV8Region r8;

    // The stack guard, which is the case that has to be right: 32 bytes at the
    // bottom of a task's stack.
    ck(mpu_v8_encode(0x20001000u, 32, MPU_NO_ACCESS, &r8), "a 32-byte guard encodes");
    ck(v8_base(r8) == 0x20001000u, "with the base it was given");
    ck(v8_en(r8) == 1, "and the region enabled");
    ck(v8_xn(r8) == 1, "never executable");
    ck(v8_ap(r8) == 0b10, "and read-only to privileged code, which is the floor here");

    // LIMIT is the address of the LAST BYTE, not the first byte past the end.
    // Off by one here makes every region 32 bytes longer than its subject, so
    // the guard also covers the first word the task is allowed to use — and the
    // very first push faults, on a stack that is nowhere near full.
    ck(v8_limit(r8) == 0x20001000u,
       "the limit names the last byte, so a 32-byte region ends where it starts");

    ck(mpu_v8_encode(0x20001000u, 64, MPU_NO_ACCESS, &r8) && v8_limit(r8) == 0x20001020u,
       "a 64-byte region reaches one granule further");

    // Package code: readable, executable, not writable.
    ck(mpu_v8_encode(0x20002000u, 1024, MPU_RO_EXEC, &r8), "package code encodes");
    ck(v8_xn(r8) == 0, "and stays executable");
    ck(v8_ap(r8) == 0b11, "read-only");
    ck(v8_limit(r8) == 0x200023E0u, "covering the whole block and no more");

    // Package data: writable, never executable. A package that jumps into a
    // buffer should fault rather than run whatever bytes are in it.
    ck(mpu_v8_encode(0x20003000u, 256, MPU_RW_NOEXEC, &r8), "package data encodes");
    ck(v8_xn(r8) == 1, "and cannot be executed");
    ck(v8_ap(r8) == 0b01, "but is writable");

    // Shareability and the attribute index have to match what the default map
    // already says about SRAM. The same memory described two ways through two
    // paths is a mismatched alias.
    ck(v8_sh(r8) == 0, "SRAM stays non-shareable, as the default map has it");
    ck(((r8.rlar >> 1) & 0x7u) == 0, "and uses attribute 0, which is set to match");

    // --- what it refuses. Each of these is a request that CANNOT be encoded,
    // and rounding it to something legal would protect memory the caller never
    // asked about.
    ck(!mpu_v8_encode(0x20001004u, 32, MPU_NO_ACCESS, &r8),
       "a base off the granule is refused, not rounded");
    ck(!mpu_v8_encode(0x20001000u, 48, MPU_NO_ACCESS, &r8),
       "a size that is not a whole number of granules is refused");
    ck(!mpu_v8_encode(0x20001000u, 0, MPU_NO_ACCESS, &r8), "and a zero-length region");
    ck(!mpu_v8_encode(0xFFFFFF00u, 0x200, MPU_NO_ACCESS, &r8),
       "and one that runs off the end of the address space");

    // ---------------------------------------------------------------- ARMv6-M
    MpuV6Region r6;

    ck(mpu_v6_encode(0x20001000u, 32, MPU_RGN_STACK_GUARD, MPU_NO_ACCESS, &r6),
       "the same 32-byte guard encodes on ARMv6-M");
    ck(v6_addr(r6) == 0x20001000u, "with the base it was given");
    ck(v6_valid(r6) == 1, "VALID set, or the region number in RBAR is ignored");
    ck(v6_region(r6) == MPU_RGN_STACK_GUARD, "and the region number carried in RBAR");
    ck(v6_enable(r6) == 1, "enabled");
    ck(v6_xn(r6) == 1, "not executable");
    // SIZE encodes 2^(SIZE+1) bytes, so 32 bytes is 4 and not 5. Off by one
    // makes the guard 64 bytes and it eats the stack below it.
    ck(v6_size(r6) == 4, "and a size field of 4, which is 2^5 = 32 bytes");
    // ARMv6-M can express what ARMv8-M cannot: privileged code gets nothing.
    ck(v6_ap(r6) == 0b000,
       "no access at all here, including for the OS — ARMv6-M can say that");

    ck(mpu_v6_encode(0x20002000u, 0x2000, 1, MPU_RO_EXEC, &r6) && v6_size(r6) == 12,
       "an 8 KB region is size field 12");
    ck(v6_ap(r6) == 0b110 && v6_xn(r6) == 0, "read-only and executable");

    ck(mpu_v6_encode(0x20004000u, 0x1000, 2, MPU_RW_NOEXEC, &r6), "and a data region");
    ck(v6_ap(r6) == 0b011 && v6_xn(r6) == 1, "writable and never executed");

    // --- what it refuses. ARMv6-M's rules are stricter and the refusals matter
    // more, because an unaligned base here does not fault — the hardware simply
    // ignores the low bits and protects a DIFFERENT block.
    ck(!mpu_v6_encode(0x20001010u, 32, 0, MPU_NO_ACCESS, &r6),
       "a base not aligned to its own size is refused");
    ck(!mpu_v6_encode(0x20001000u, 96, 0, MPU_NO_ACCESS, &r6),
       "a size that is not a power of two is refused");
    ck(!mpu_v6_encode(0x20001000u, 16, 0, MPU_NO_ACCESS, &r6),
       "and one below the 32-byte minimum");
    ck(!mpu_v6_encode(0x20001000u, 32, 8, MPU_NO_ACCESS, &r6),
       "and a region number the hardware does not have");

    // --------------------------------------------------------------- planning
    //
    // The three numbers a caller needs have to agree: ask malloc for
    // alloc_bytes, round what it returns up to align, and the region covers
    // region_bytes from there. If they disagree the region runs past the end of
    // the block, and the permissions land on whatever malloc handed out next.
    MpuBlockPlan p;

    ck(mpu_v8_plan_block(700, &p), "planning a 700-byte block on ARMv8-M");
    ck(p.align == 32, "aligns to the granule");
    ck(p.region_bytes == 704, "and rounds the region up to a whole number of them");
    ck(p.alloc_bytes >= p.region_bytes + p.align - 1,
       "asking malloc for enough that an aligned block still fits");

    ck(mpu_v6_plan_block(700, &p), "planning the same block on ARMv6-M");
    ck(p.region_bytes == 1024, "costs a whole power of two");
    ck(p.align == 1024, "with the base aligned to it, which is where the RAM goes");

    ck(mpu_v6_plan_block(1024, &p) && p.region_bytes == 1024,
       "an exact power of two is not rounded up again");
    ck(mpu_v6_plan_block(1, &p) && p.region_bytes == 32,
       "and nothing is smaller than the 32-byte minimum");
    ck(mpu_v8_plan_block(1, &p) && p.region_bytes == 32, "on either architecture");

    ck(!mpu_v8_plan_block(0, &p) && !mpu_v6_plan_block(0, &p),
       "a zero-byte block is not a region");

    // The top of the range, which matters only because getting it wrong wraps
    // silently. 2 GB is the largest power of two that still fits, and one byte
    // more has no answer — the doubling would overflow to zero and produce a
    // "region" of nothing at all.
    ck(mpu_v6_plan_block(0x80000000u, &p) && p.region_bytes == 0x80000000u,
       "the largest power of two that fits is still planned");
    ck(!mpu_v6_plan_block(0x80000001u, &p),
       "and one byte past it is refused rather than wrapped to zero");

    // Every plan a real allocation could ask for must actually encode. This is
    // the join between the two halves of this file, and it is the one that
    // would not be noticed: planning that produces something the encoder then
    // refuses means no protection at all, silently.
    bool all_encode = true;
    for (uint32_t want = 1; want <= 9000; want += 7) {
        MpuBlockPlan q;
        MpuV8Region e8;
        MpuV6Region e6;
        if (!mpu_v8_plan_block(want, &q)) { all_encode = false; break; }
        uint32_t base8 = mpu_align_up(0x20000004u, q.align);
        if (!mpu_v8_encode(base8, q.region_bytes, MPU_RW_NOEXEC, &e8)) {
            all_encode = false; break;
        }
        if (!mpu_v6_plan_block(want, &q)) { all_encode = false; break; }
        uint32_t base6 = mpu_align_up(0x20000004u, q.align);
        if (!mpu_v6_encode(base6, q.region_bytes, 1, MPU_RW_NOEXEC, &e6)) {
            all_encode = false; break;
        }
    }
    ck(all_encode, "every plan the loader could ask for encodes on both architectures");

    // mpu_align_up must never go DOWN. A rounded-down base puts the region
    // before the block, which protects the previous allocation and leaves the
    // real one open.
    ck(mpu_align_up(0x20000001u, 32) == 0x20000020u, "aligning up moves up");
    ck(mpu_align_up(0x20000020u, 32) == 0x20000020u, "and leaves an aligned value alone");
    ck(mpu_align_up(0xFFFFFFF1u, 32) >= 0xFFFFFFF1u,
       "and never wraps past the top of memory");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
