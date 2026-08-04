#include "mpu.h"

// Memory attributes.
//
// These say what KIND of memory a region is, and they are set to match what the
// default memory map already says about SRAM rather than to describe anything
// new. That is deliberate. Every region this OS programs lives in SRAM, which
// the rest of the firmware reaches through the default map, and giving the same
// physical memory two different memory types through two different paths is a
// mismatched alias — architecturally undefined, and the sort of thing that
// works until it does not.
//
// Neither part has a data cache (RP2350's XIP cache sits on the bus in front of
// flash, not in front of the core), so cacheability and shareability are inert
// here either way. Matching the default map costs nothing and removes the
// question.
//
// ARMv8-M: MAIR attribute 0 = Normal, Outer and Inner write-back, read and
// write allocate, non-transient — 0xFF. Shareability non-shareable (0), which
// is what the default map says for 0x20000000.
#define V8_ATTR_INDEX  0u
#define V8_SH_NONE     0u

// ARMv6-M: RASR holds TEX[21:19], S[18], C[17], B[16]. TEX=000, S=1, C=1, B=0
// is Normal, shareable, write-through — ARM's recommended encoding for internal
// SRAM.
#define V6_MEM_ATTRS   ((0u << 19) | (1u << 18) | (1u << 17) | (0u << 16))

// --- ARMv8-M ----------------------------------------------------------------
//
// RBAR: BASE[31:5] | SH[4:3] | AP[2:1] | XN[0]
// RLAR: LIMIT[31:5] | ATTRINDX[3:1] | EN[0]
//
// LIMIT is the address of the LAST BYTE in the region, with its low five bits
// taken as 1 rather than stored. So the value written is (base + size - 1)
// with the low bits masked off, and a size that is not a whole number of
// granules would silently round the region's end down — which is why the
// encoder refuses one instead.

// AP[1:0]:  00 RW privileged-only   01 RW any
//           10 RO privileged-only   11 RO any
static bool v8_perm_bits(MpuPerm perm, uint32_t *ap, uint32_t *xn) {
    switch (perm) {
    case MPU_NO_ACCESS:
        // The strictest thing ARMv8-M can say. There is no encoding for
        // "privileged code may not read this" — read-only is the floor — so
        // this catches writes and instruction fetches and lets reads through.
        // A stack overflow is a push, so it is caught.
        *ap = 0b10; *xn = 1; return true;
    case MPU_RO_EXEC:
        // Readable and executable by unprivileged code too. Nothing runs
        // unprivileged yet, which makes this identical to the privileged-only
        // encoding today; it is written this way because it is what package
        // code will need when it does, and a permission that has to be
        // remembered later is one that gets forgotten.
        *ap = 0b11; *xn = 0; return true;
    case MPU_RW_NOEXEC:
        *ap = 0b01; *xn = 1; return true;
    }
    return false;
}

bool mpu_v8_encode(uint32_t base, uint32_t size, MpuPerm perm, MpuV8Region *out) {
    if (!out) return false;
    if (size == 0) return false;
    if (base % MPU_V8_GRAIN) return false;
    if (size % MPU_V8_GRAIN) return false;
    if (base + size < base) return false;              // wraps the address space

    uint32_t ap = 0, xn = 0;
    if (!v8_perm_bits(perm, &ap, &xn)) return false;

    out->rbar = (base & ~(MPU_V8_GRAIN - 1))
              | (V8_SH_NONE << 3)
              | (ap << 1)
              | xn;
    out->rlar = ((base + size - 1) & ~(MPU_V8_GRAIN - 1))
              | (V8_ATTR_INDEX << 1)
              | 1u;                                    // EN
    return true;
}

// --- ARMv6-M ----------------------------------------------------------------
//
// RBAR: ADDR[31:8] | VALID[4] | REGION[3:0]
// RASR: XN[28] | AP[26:24] | attrs[21:16] | SRD[15:8] | SIZE[5:1] | ENABLE[0]
//
// SIZE encodes 2^(SIZE+1) bytes, so 32 bytes is SIZE=4. The base must be
// aligned to the region size, which is the expensive part of this architecture:
// a 700-byte block needs a 1024-byte region on a 1024-byte boundary.

// AP[2:0]:  000 no access   001 RW priv   011 RW full
//           101 RO priv     110 RO full
static bool v6_perm_bits(MpuPerm perm, uint32_t *ap, uint32_t *xn) {
    switch (perm) {
    case MPU_NO_ACCESS:
        // Genuinely no access here, including for privileged code — ARMv6-M can
        // say what ARMv8-M cannot. A read of the guard faults too.
        *ap = 0b000; *xn = 1; return true;
    case MPU_RO_EXEC:   *ap = 0b110; *xn = 0; return true;
    case MPU_RW_NOEXEC: *ap = 0b011; *xn = 1; return true;
    }
    return false;
}

// log2 of a power of two, or 32 if it is not one.
static uint32_t log2_exact(uint32_t v) {
    if (v == 0 || (v & (v - 1))) return 32;
    uint32_t n = 0;
    while ((v >> n) != 1u) n++;
    return n;
}

bool mpu_v6_encode(uint32_t base, uint32_t size, uint8_t region, MpuPerm perm,
                   MpuV6Region *out) {
    if (!out) return false;
    if (region > 7) return false;
    if (size < MPU_V6_MIN_SIZE) return false;

    uint32_t bits = log2_exact(size);
    if (bits == 32) return false;                      // not a power of two
    if (base & (size - 1)) return false;               // not aligned to its size
    if (base + size < base) return false;

    uint32_t ap = 0, xn = 0;
    if (!v6_perm_bits(perm, &ap, &xn)) return false;

    out->rbar = base | (1u << 4) | region;             // VALID, so REGION is used
    out->rasr = 1u                                     // ENABLE
              | ((bits - 1u) << 1)                     // SIZE: 2^(SIZE+1) bytes
              | V6_MEM_ATTRS
              | (ap << 24)
              | (xn << 28);
    return true;
}

// --- sizing -----------------------------------------------------------------

uint32_t mpu_align_up(uint32_t v, uint32_t align) {
    if (align == 0 || (align & (align - 1))) return v;  // not a power of two
    uint32_t r = v + (align - 1);
    if (r < v) return v;                               // would wrap: leave it be
    return r & ~(align - 1);
}

bool mpu_v8_plan_block(uint32_t want, MpuBlockPlan *out) {
    if (!out || want == 0) return false;
    uint32_t size = mpu_align_up(want, MPU_V8_GRAIN);
    if (size < want) return false;                     // rounding overflowed
    if (size > 0xFFFFFFFFu - MPU_V8_GRAIN) return false;
    out->align        = MPU_V8_GRAIN;
    out->region_bytes = size;
    // Worst case malloc returns a pointer one byte past an alignment boundary,
    // so the aligned start can be up to align-1 further in.
    out->alloc_bytes  = size + (MPU_V8_GRAIN - 1u);
    return true;
}

bool mpu_v6_plan_block(uint32_t want, MpuBlockPlan *out) {
    if (!out || want == 0) return false;
    uint32_t size = MPU_V6_MIN_SIZE;
    while (size < want) {
        if (size > 0x40000000u) return false;          // next doubling overflows
        size <<= 1;
    }
    out->align        = size;                          // base must match the size
    out->region_bytes = size;
    out->alloc_bytes  = size + (size - 1u);
    if (out->alloc_bytes < size) return false;
    return true;
}
