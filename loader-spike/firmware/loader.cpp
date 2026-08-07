// Runtime ELF loader for relocatable ARM Thumb objects.
//
// The shape of the problem, measured rather than assumed:
//
//   * GCC 14.2 for Cortex-M33 at -Os emits exactly three relocation types for
//     real C++ — R_ARM_ABS32, R_ARM_THM_CALL and R_ARM_THM_JUMP24. It
//     materialises addresses through literal pools, so the MOVW/MOVT pair that
//     complicates other ARM loaders does not appear. The engine is small.
//
//   * The hard part is RANGE, not encoding. On RP2350 the firmware runs from
//     XIP flash at 0x10000000 and an app is loaded into SRAM at 0x20000000 —
//     a 256 MB gap. A Thumb BL reaches +/-16 MB. So EVERY call from an app into
//     the firmware is out of range and needs a veneer: a few bytes of
//     trampoline, allocated next to the app, that the BL can reach and which
//     performs an indirect branch the whole way.
//
// The veneer is the reason this works at all, and it is the thing a naive
// implementation gets wrong — it links fine, loads fine, and branches into
// nowhere the first time an app calls printf.

#include "loader.h"
#include "elf.h"
#include "api.h"

#include <string.h>
#include <stdlib.h>

// ------------------------------------------------------------- allocation
// Addresses are held as uintptr_t so the same source compiles clean for a
// 64-bit host test, but the loader assumes a 32-bit address space: relocations
// are 32-bit and there is nowhere to put the high half. The host test satisfies
// that by mapping its image low rather than by using malloc.
static LoaderAlloc g_alloc = malloc;
static LoaderFree  g_free  = free;

void loader_set_allocator(LoaderAlloc a, LoaderFree f) {
    g_alloc = a ? a : malloc;
    g_free  = f ? f : free;
}

// Round an address up to a block boundary.
//
// In uintptr_t rather than through the 32-bit region helpers, because this file
// also compiles for the host test where a pointer does not fit in thirty-two
// bits — and a truncated one is not an address at all. Up, never down: rounding
// down would put a protected block's start before the memory that was actually
// allocated for it.
static inline uintptr_t block_align(uintptr_t v) {
    return (v + (APP_BLOCK_ALIGN - 1)) & ~(uintptr_t)(APP_BLOCK_ALIGN - 1);
}

// ---------------------------------------------------------------- utilities

// Read exactly `len` bytes or fail. A partial read has to be an error: a
// truncated file would otherwise leave section headers half-filled with
// whatever was in the buffer, and the loader would dereference them.
static bool read_exact(const AppSource &src, uint32_t off, void *dst, uint32_t len) {
    if (len == 0) return true;
    if (src.size && (off > src.size || off + len > src.size)) return false;
    int n = src.read(src.ctx, off, dst, len);
    return n == (int)len;
}

static void set_detail(LoadedApp *a, const char *s) {
    if (!a) return;
    size_t n = strlen(s);
    if (n >= sizeof(a->detail)) n = sizeof(a->detail) - 1;
    memcpy(a->detail, s, n);
    a->detail[n] = 0;
}

const char *elf_reloc_name(uint32_t type) {
    switch (type) {
        case R_ARM_NONE:            return "R_ARM_NONE";
        case R_ARM_ABS32:           return "R_ARM_ABS32";
        case R_ARM_REL32:           return "R_ARM_REL32";
        case R_ARM_THM_CALL:        return "R_ARM_THM_CALL";
        case R_ARM_ABS16:           return "R_ARM_ABS16";
        case R_ARM_ABS8:            return "R_ARM_ABS8";
        case R_ARM_THM_JUMP24:      return "R_ARM_THM_JUMP24";
        case R_ARM_TARGET1:         return "R_ARM_TARGET1";
        case R_ARM_PREL31:          return "R_ARM_PREL31";
        case R_ARM_THM_MOVW_ABS_NC: return "R_ARM_THM_MOVW_ABS_NC";
        case R_ARM_THM_MOVT_ABS:    return "R_ARM_THM_MOVT_ABS";
        case R_ARM_THM_JUMP11:      return "R_ARM_THM_JUMP11";
        default:                    return "R_ARM_<unknown>";
    }
}

const char *load_result_str(LoadResult r) {
    switch (r) {
        case LOAD_OK:                    return "ok";
        case LOAD_ERR_READ:              return "read error";
        case LOAD_ERR_NOT_ELF:           return "not an ELF file";
        case LOAD_ERR_NOT_REL:           return "not a relocatable object";
        case LOAD_ERR_NOT_ARM:           return "not an ARM object";
        case LOAD_ERR_NO_HEADER:         return "no .rpc_app_header section";
        case LOAD_ERR_BAD_MAGIC:         return "bad app header magic";
        case LOAD_ERR_API_MISMATCH:      return "API version mismatch";
        case LOAD_ERR_OOM:               return "out of memory";
        case LOAD_ERR_NO_ENTRY:          return "no app_main symbol";
        case LOAD_ERR_UNDEF_SYMBOL:      return "unresolved symbol";
        case LOAD_ERR_RELOC_UNSUPPORTED: return "unsupported relocation";
        case LOAD_ERR_RELOC_RANGE:       return "relocation out of range";
        case LOAD_ERR_TOO_MANY_SECTIONS: return "too many sections";
        default:                         return "unknown error";
    }
}

// ------------------------------------------------------------------ veneers
//
// Written in a form valid on both Cortex-M33 and Cortex-M0+, because the whole
// point of the rewrite is that the RP2040 comes back and an M0+-incompatible
// veneer would quietly rule it out again. M33 could use a 6-byte
// `ldr.w pc,[pc]`, but the saving is not worth two code paths in a spike.
//
//   +0  push {r0}          b401
//   +2  ldr  r0, [pc, #8]   4802     ; -> the literal at +12
//   +4  mov  ip, r0         4684
//   +6  pop  {r0}           bc01
//   +8  bx   ip             4760
//   +10 (pad)
//   +12 .word target
//
// The literal offset is not free to choose. A Thumb LDR-literal computes its
// address as Align(PC, 4) + imm8*4 where PC is the instruction address + 4, so
// for the LDR at +2 that is Align(+6, 4) + imm8*4 = +4 + imm8*4. imm8 = 2 puts
// the literal at +12, which is the first 4-aligned slot past the last
// instruction. imm8 = 1 would put it at +8 — on top of the `bx ip` — and the
// veneer would execute its own target word as code.
//
// Laid out with explicit offsets rather than a struct, because a struct of
// {uint16_t[5]; uint32_t; uint16_t} is 20 bytes after the compiler inserts
// alignment padding, not the 16 the pool strides by. Writing 20-byte structs at
// 16-byte intervals overruns the pool and corrupts the previous veneer.
//
// ip (r12) is call-clobbered under AAPCS, so trashing it across a call is
// legal. LR is untouched: the app's BL already set it, so the callee returns
// straight back into the app rather than into the veneer.
#define VENEER_BYTES   16
#define VENEER_LITERAL 12      // byte offset of the target word within a veneer

// --- the second form: a supervisor call --------------------------------------
//
// A SANDBOXED package cannot use the veneer above at all. It runs unprivileged,
// and unprivileged code cannot fetch instructions from flash — that is precisely
// what makes the sandbox a sandbox — so there is nothing for `bx ip` to reach.
//
// Instead it names the function by its position in the firmware's export table
// and asks the supervisor to make the jump:
//
//   +0   ldr.w ip, [pc, #8]   f8df c008   ; -> the index at +12
//   +4   svc  #0              df00
//   +6   b    .               e7fe        ; never runs: the handler redirects
//   +8   (pad)
//   +12  .word index
//
// The index travels in a REGISTER rather than in the SVC immediate, which is
// eight bits wide against an ABI that passed 156 entries some time ago. r12 is
// call-clobbered under AAPCS so using it costs nothing, and r0-r3 — the
// arguments — are never touched.
//
// The literal stays at +12, the same offset the direct form uses, so the
// reuse scan below does not have to know which form it is looking at. For the
// LDR at +0 the address is Align(PC, 4) + imm12 where PC is the instruction
// address + 4, so imm12 = 8 lands on +12.
//
// Two fixed slots come before any of these, because the way OUT of privileged
// code needs an instruction the package is allowed to execute, and flash is not
// it. They are written once at load time and are read-only to the package.
//
//   slot 0, the return gate:  msr CONTROL, r2 ; isb ; bx r3
//     Reached from the firmware, still privileged, with r2 holding the new
//     CONTROL value and r3 the package's return address. The `msr` has to be
//     the second-to-last instruction executed — everything after it runs
//     unprivileged, so everything after it has to live here rather than in
//     flash.
//
//   slot 1, the exit gate:  svc #1
//     Handed to a package as its return address. When app_main or a registered
//     command returns, it lands here and the supervisor takes the OS back.
#define VENEER_GATE_BYTES   48     // three slots, in SVC mode only
#define VENEER_GATE_RETURN  0
#define VENEER_GATE_ENTER   16
#define VENEER_GATE_EXIT    32
#define VENEER_NOT_AN_INDEX 0xFFFFFFFFu

static LoaderVeneerMode g_veneer_mode = LOADER_VENEER_DIRECT;

void loader_set_veneer_mode(LoaderVeneerMode m) { g_veneer_mode = m; }
LoaderVeneerMode loader_veneer_mode(void) { return g_veneer_mode; }

// Write the two fixed gates at the start of the pool. Called once, before any
// call veneer, so the offsets above are stable.
static void veneer_write_gates(LoadedApp *app) {
    uint8_t *base = (uint8_t *)app->veneers;

    // Both privilege gates carry the new CONTROL value in r12 and nothing else,
    // which is the whole reason there are two of them. r0-r3 must survive
    // untouched: a function may return a value in any of them, and
    // __aeabi_uldivmod returns its remainder in r2 and r3 specifically. Using
    // one of those as scratch made every digit of a 64-bit division come back
    // wrong — visible as `calc 2 ^ 3 ^ 2` printing '===' instead of 512, with
    // the fractional part, which needs no such helper, perfectly correct.
    //
    // They differ only in where they go afterwards, and that is why one gate
    // cannot serve both: entering, LR has to be left holding the exit gate so
    // that app_main's own return lands somewhere useful, so the destination
    // travels in r3. Returning, LR is dead and carries the destination itself.
    uint16_t *r = (uint16_t *)(base + VENEER_GATE_RETURN);
    r[0] = 0xf38c; r[1] = 0x8814;   // msr CONTROL, r12
    r[2] = 0xf3bf; r[3] = 0x8f6f;   // isb sy
    r[4] = 0x4770;                  // bx  lr   - back into the package
    r[5] = 0xbe00;
    *(uint32_t *)(base + VENEER_GATE_RETURN + VENEER_LITERAL) = VENEER_NOT_AN_INDEX;

    uint16_t *e = (uint16_t *)(base + VENEER_GATE_ENTER);
    e[0] = 0xf38c; e[1] = 0x8814;   // msr CONTROL, r12
    e[2] = 0xf3bf; e[3] = 0x8f6f;   // isb sy
    e[4] = 0x4718;                  // bx  r3   - into app_main, LR = exit gate
    e[5] = 0xbe00;
    *(uint32_t *)(base + VENEER_GATE_ENTER + VENEER_LITERAL) = VENEER_NOT_AN_INDEX;

    uint16_t *x = (uint16_t *)(base + VENEER_GATE_EXIT);
    x[0] = 0xdf01;                  // svc #1  - the package has returned
    x[1] = 0xe7fe;                  // b . : the handler redirects, so this is
    x[2] = 0xe7fe;                  //       only reached if it did not
    x[3] = 0xe7fe;
    x[4] = 0xe7fe;
    x[5] = 0xe7fe;
    *(uint32_t *)(base + VENEER_GATE_EXIT + VENEER_LITERAL) = VENEER_NOT_AN_INDEX;

    app->veneers_used  = VENEER_GATE_BYTES;
    app->veneer_gates  = VENEER_GATE_BYTES;
}

static uint32_t veneer_emit(LoadedApp *app, uint32_t target) {
    // Reuse an existing veneer for the same target: a typical app calls
    // fw_printf a dozen times and one trampoline serves them all.
    //
    // The scan starts past the fixed gates. Their literal word is zero, and
    // zero is a perfectly ordinary ABI index — scanning from the beginning
    // would hand out the return gate as the veneer for symbol 0, and a package
    // calling that function would drop privilege into nowhere.
    uint8_t *base = (uint8_t *)app->veneers;
    for (uint32_t off = app->veneer_gates; off < app->veneers_used; off += VENEER_BYTES) {
        if (*(uint32_t *)(base + off + VENEER_LITERAL) == target)
            return (uint32_t)(uintptr_t)(base + off) | 1u;      // Thumb bit
    }
    if (app->veneers_used + VENEER_BYTES > app->veneer_size) return 0;
    uint8_t *v = base + app->veneers_used;
    uint16_t *c = (uint16_t *)v;

    if (g_veneer_mode == LOADER_VENEER_SVC) {
        c[0] = 0xf8df; c[1] = 0xc008;   // ldr.w ip, [pc, #8]
        c[2] = 0xdf00;                  // svc  #0
        c[3] = 0xe7fe;                  // b .
        c[4] = 0xbe00;                  // bkpt, in the pad
        c[5] = 0x0000;
        *(uint32_t *)(v + VENEER_LITERAL) = target;   // the index, not an address
        app->veneers_used += VENEER_BYTES;
        return (uint32_t)(uintptr_t)v | 1u;
    }

    c[0] = 0xb401;    // push {r0}
    c[1] = 0x4802;    // ldr  r0, [pc, #8]
    c[2] = 0x4684;    // mov  ip, r0
    c[3] = 0xbc01;    // pop  {r0}
    c[4] = 0x4760;    // bx   ip
    c[5] = 0x0000;    // pad, so the literal is 4-aligned
    *(uint32_t *)(v + VENEER_LITERAL) = target;
    app->veneers_used += VENEER_BYTES;
    return (uint32_t)(uintptr_t)v | 1u;
}

// --------------------------------------------------------- Thumb BL encoding

static bool thumb_bl_in_range(int32_t disp) {
    return disp >= -(1 << 24) && disp < (1 << 24);
}

// Encode a 25-bit signed displacement into a Thumb-2 BL/B.W pair, preserving
// everything in the instruction that is not part of the offset.
static void thumb_encode_branch(uint16_t *p, int32_t disp) {
    uint32_t s    = (disp < 0) ? 1u : 0u;
    uint32_t i1   = ((uint32_t)disp >> 23) & 1u;
    uint32_t i2   = ((uint32_t)disp >> 22) & 1u;
    uint32_t imm10 = ((uint32_t)disp >> 12) & 0x3ffu;
    uint32_t imm11 = ((uint32_t)disp >> 1)  & 0x7ffu;
    uint32_t j1 = (~i1 ^ s) & 1u;
    uint32_t j2 = (~i2 ^ s) & 1u;
    p[0] = (uint16_t)((p[0] & 0xf800) | (s << 10) | imm10);
    p[1] = (uint16_t)((p[1] & 0xd000) | (j1 << 13) | (j2 << 11) | imm11);
}

static int32_t thumb_decode_branch(const uint16_t *p) {
    uint32_t s     = (p[0] >> 10) & 1u;
    uint32_t imm10 = p[0] & 0x3ffu;
    uint32_t j1    = (p[1] >> 13) & 1u;
    uint32_t j2    = (p[1] >> 11) & 1u;
    uint32_t imm11 = p[1] & 0x7ffu;
    uint32_t i1 = (~(j1 ^ s)) & 1u;
    uint32_t i2 = (~(j2 ^ s)) & 1u;
    int32_t v = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                          (imm10 << 12) | (imm11 << 1));
    if (s) v |= (int32_t)0xfe000000;      // sign-extend from bit 24
    return v;
}

// ------------------------------------------------------------------ loading

struct SectionMap {
    uint32_t addr;        // where the section landed in RAM (0 if not loaded)
    uint32_t size;
    bool     loaded;
};

LoadResult app_load(const AppSource &src, LoadedApp *out) {
    memset(out, 0, sizeof(*out));

    Elf32_Ehdr eh;
    if (!read_exact(src, 0, &eh, sizeof(eh))) return LOAD_ERR_READ;
    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 || eh.e_ident[4] != 1)
        return LOAD_ERR_NOT_ELF;
    if (eh.e_type != ET_REL)      return LOAD_ERR_NOT_REL;
    if (eh.e_machine != EM_ARM)   return LOAD_ERR_NOT_ARM;
    if (eh.e_shnum > LOADER_MAX_SECTIONS) return LOAD_ERR_TOO_MANY_SECTIONS;

    // STATIC, not on the stack.
    //
    // These two are sized by LOADER_MAX_SECTIONS, and at 128 that is over six
    // kilobytes between them — a frame that large lands on whoever called
    // app_load, which is the shell during `pkg install` and the boot task
    // before that. Neither has room for it, and neither should have to.
    //
    // Safe because loading is one at a time by construction: packages load
    // sequentially at boot, and installing is a shell command. Nothing in the
    // ABI lets a package load another one.
    static Elf32_Shdr sh[LOADER_MAX_SECTIONS];
    if (!read_exact(src, eh.e_shoff, sh, eh.e_shnum * sizeof(Elf32_Shdr)))
        return LOAD_ERR_READ;

    // Section-name strings, needed to find .rpc_app_header.
    const Elf32_Shdr &shstr = sh[eh.e_shstrndx];
    char *names = (char *)malloc(shstr.sh_size);
    if (!names) return LOAD_ERR_OOM;
    if (!read_exact(src, shstr.sh_offset, names, shstr.sh_size)) {
        free(names);
        return LOAD_ERR_READ;
    }

    // --- the header check happens FIRST, before anything is allocated or
    // relocated. Refusing an incompatible app has to be cheap and has to happen
    // before any of its content is trusted.
    int hdr_idx = -1;
    for (int i = 0; i < eh.e_shnum; i++) {
        if (strcmp(names + sh[i].sh_name, ".rpc_app_header") == 0) { hdr_idx = i; break; }
    }
    if (hdr_idx < 0) { free(names); return LOAD_ERR_NO_HEADER; }
    if (!read_exact(src, sh[hdr_idx].sh_offset, &out->header,
                    sizeof(RpcAppHeader))) {
        free(names);
        return LOAD_ERR_READ;
    }
    if (out->header.magic != RPC_APP_MAGIC) { free(names); return LOAD_ERR_BAD_MAGIC; }
    if (out->header.api_major != RPC_API_MAJOR ||
        out->header.api_minor >  RPC_API_MINOR) {
        free(names);
        return LOAD_ERR_API_MISMATCH;
    }

    // --- lay the allocatable sections out in one block, in two halves.
    //
    // Still one allocation: the heap this runs on is small, and a single free is
    // the only way "unload reclaims everything" stays verifiable. But the block
    // is now split, with everything the app may not write to first and
    // everything it must be able to write to after, because those two want
    // OPPOSITE permissions and a section order that interleaves them can be
    // given neither. Code has to be executable and must not be writable; data
    // has to be writable and must never be executed. Sorted by SHF_WRITE, which
    // is the ELF flag that says exactly which is which.
    //
    // The halves are padded to APP_BLOCK_ALIGN so the protection hardware can
    // cover each one exactly. Without that the region would either stop short —
    // leaving the tail of the block unprotected — or run past the end and apply
    // the app's permissions to whatever the heap handed out next.
    // Static for the same reason as `sh` above, and cleared on every entry
    // because a static keeps the last load's answers.
    static SectionMap map[LOADER_MAX_SECTIONS];
    memset(map, 0, sizeof(map));

    uint32_t total = 0;
    uint32_t text_end = 0;                 // where the read-only half stops
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < eh.e_shnum; i++) {
            if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
            bool writable = (sh[i].sh_flags & SHF_WRITE) != 0;
            if (writable != (pass == 1)) continue;
            uint32_t align = sh[i].sh_addralign ? sh[i].sh_addralign : 4;
            if (align < 4) align = 4;
            total = (total + align - 1) & ~(align - 1);
            map[i].addr = total;           // offset for now; rebased below
            map[i].size = sh[i].sh_size;
            map[i].loaded = true;
            total += sh[i].sh_size;
        }
        if (pass == 0) {
            total = (total + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);
            text_end = total;              // the writable half starts here
        }
    }
    total = (total + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);

    // Veneer pool. Sized from the number of relocations rather than guessed:
    // worst case is one veneer per distinct out-of-range target.
    //
    // But bounded by the ABI as well, because "one per relocation" is a wildly
    // loose bound on a large package. veneer_emit reuses a trampoline for a
    // target it has already seen, and BOTH call sites only ever emit for a
    // firmware target — an ABI index when sandboxed, a firmware address when
    // not. Everything else is inside the image and reachable by an ordinary
    // branch. So the number of DISTINCT targets can never exceed the number of
    // symbols the firmware exports, however many times they are called.
    //
    // Without this a package with five thousand relocations reserves eighty
    // kilobytes of trampolines and uses under three. That is most of a Nova
    // D1-sized image, spent on nothing. If the bound were ever wrong the cost
    // is a clean LOAD_ERR_RELOC_RANGE from veneer_emit, not corruption.
    uint32_t reloc_count = 0;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_REL) reloc_count += sh[i].sh_size / sizeof(Elf32_Rel);
    // Zero means a caller that does not keep a real table (a host test with a
    // stubbed ABI), and clamping to nothing would refuse an app that resolves
    // fine. Only a real count is allowed to tighten the bound.
    uint32_t max_targets = api_symbol_count();
    if (max_targets && reloc_count > max_targets) reloc_count = max_targets;
    uint32_t veneer_bytes = (reloc_count + 1) * VENEER_BYTES;
    if (g_veneer_mode == LOADER_VENEER_SVC) veneer_bytes += VENEER_GATE_BYTES;
    veneer_bytes = (veneer_bytes + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);

    // Both blocks are over-allocated by one alignment and the useful part is
    // placed on the first boundary inside. The raw pointers are kept because
    // they, not the aligned ones, are what the allocator will take back.
    uint8_t *image_raw = (uint8_t *)g_alloc((total ? total : APP_BLOCK_ALIGN) + APP_BLOCK_ALIGN);
    if (!image_raw) { free(names); return LOAD_ERR_OOM; }
    uint8_t *veneers_raw = (uint8_t *)g_alloc(veneer_bytes + APP_BLOCK_ALIGN);
    if (!veneers_raw) { g_free(image_raw); free(names); return LOAD_ERR_OOM; }

    uint8_t *image   = (uint8_t *)block_align((uintptr_t)image_raw);
    uint8_t *veneers = (uint8_t *)block_align((uintptr_t)veneers_raw);

    out->image_raw   = image_raw;
    out->veneers_raw = veneers_raw;
    out->text_size   = text_end;
    out->data        = text_end < total ? image + text_end : nullptr;
    out->data_size   = total - text_end;
    out->image = image;
    out->image_size = total;
    out->veneers = veneers;
    out->veneer_size = veneer_bytes;
    out->veneers_used = 0;
    out->veneer_gates = 0;
    // The gates go in before anything else, so their offsets are fixed and the
    // reuse scan has a definite place to start.
    if (g_veneer_mode == LOADER_VENEER_SVC) veneer_write_gates(out);
    // What was actually taken from the heap, alignment slack included — so the
    // "did not release N bytes" report after a package runs stays truthful.
    out->bytes_allocated = total + veneer_bytes + 2 * APP_BLOCK_ALIGN;

    for (int i = 0; i < eh.e_shnum; i++) {
        if (!map[i].loaded) continue;
        map[i].addr += (uint32_t)(uintptr_t)image;                 // rebase to real memory
        if (sh[i].sh_type == SHT_NOBITS) {
            memset((void *)(uintptr_t)map[i].addr, 0, sh[i].sh_size);   // .bss
        } else if (!read_exact(src, sh[i].sh_offset,
                               (void *)(uintptr_t)map[i].addr, sh[i].sh_size)) {
            free(names);
            app_unload(out);
            return LOAD_ERR_READ;
        }
    }

    // --- symbols
    int symtab_idx = -1;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB) { symtab_idx = i; break; }
    if (symtab_idx < 0) { free(names); app_unload(out); return LOAD_ERR_NO_ENTRY; }

    uint32_t nsyms = sh[symtab_idx].sh_size / sizeof(Elf32_Sym);
    Elf32_Sym *syms = (Elf32_Sym *)malloc(sh[symtab_idx].sh_size);
    if (!syms) { free(names); app_unload(out); return LOAD_ERR_OOM; }
    if (!read_exact(src, sh[symtab_idx].sh_offset, syms,
                    sh[symtab_idx].sh_size)) {
        free(syms); free(names); app_unload(out); return LOAD_ERR_READ;
    }
    uint32_t strtab_idx = sh[symtab_idx].sh_link;
    if (strtab_idx >= eh.e_shnum) {
        free(syms); free(names); app_unload(out); return LOAD_ERR_READ;
    }
    char *strs = (char *)malloc(sh[strtab_idx].sh_size);
    if (!strs) { free(syms); free(names); app_unload(out); return LOAD_ERR_OOM; }
    if (!read_exact(src, sh[strtab_idx].sh_offset, strs,
                    sh[strtab_idx].sh_size)) {
        free(strs); free(syms); free(names); app_unload(out); return LOAD_ERR_READ;
    }

    LoadResult rc = LOAD_OK;

    // Resolve one symbol to an absolute address. Defined symbols come from the
    // loaded image; undefined ones must be exported by the firmware.
    auto resolve = [&](uint32_t idx, uint32_t *addr, bool *is_func) -> LoadResult {
        const Elf32_Sym &s = syms[idx];
        *is_func = (ELF32_ST_TYPE(s.st_info) == STT_FUNC);
        if (s.st_shndx == SHN_UNDEF) {
            const char *nm = strs + s.st_name;
            if (g_veneer_mode == LOADER_VENEER_SVC) {
                // A sandboxed package never learns a firmware address at all.
                // The symbol resolves to its own supervisor-call veneer, which
                // means EVERY route to the firmware goes through the gateway —
                // including the one a direct address would otherwise open.
                //
                // A package that stores a function pointer rather than calling
                // through it is the case this covers: an ABS32 relocation to a
                // real firmware address would produce a pointer that works
                // perfectly while packages are privileged and faults the moment
                // they are not, which is the worst possible time to find out.
                int ix = api_index_of(nm);
                if (ix < 0) { set_detail(out, nm); return LOAD_ERR_UNDEF_SYMBOL; }
                uint32_t v = veneer_emit(out, (uint32_t)ix);
                if (!v) { set_detail(out, nm); return LOAD_ERR_RELOC_RANGE; }
                *addr = v;                 // already carries the Thumb bit
                *is_func = true;
                return LOAD_OK;
            }
            uint32_t a = api_lookup(nm);
            if (!a) { set_detail(out, nm); return LOAD_ERR_UNDEF_SYMBOL; }
            *addr = a;
            *is_func = true;      // everything the firmware exports is callable
            return LOAD_OK;
        }
        if (s.st_shndx == SHN_ABS) { *addr = s.st_value; return LOAD_OK; }
        if (s.st_shndx >= eh.e_shnum || !map[s.st_shndx].loaded) {
            set_detail(out, strs + s.st_name);
            return LOAD_ERR_UNDEF_SYMBOL;
        }
        *addr = map[s.st_shndx].addr + s.st_value;
        return LOAD_OK;
    };

    for (int i = 0; i < eh.e_shnum && rc == LOAD_OK; i++) {
        if (sh[i].sh_type != SHT_REL) continue;
        uint32_t target_sec = sh[i].sh_info;
        if (target_sec >= eh.e_shnum || !map[target_sec].loaded) continue;

        uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel);
        Elf32_Rel *rels = (Elf32_Rel *)malloc(sh[i].sh_size);
        if (!rels) { rc = LOAD_ERR_OOM; break; }
        if (!read_exact(src, sh[i].sh_offset, rels, sh[i].sh_size)) {
            free(rels); rc = LOAD_ERR_READ; break;
        }

        for (uint32_t r = 0; r < n && rc == LOAD_OK; r++) {
            uint32_t type = ELF32_R_TYPE(rels[r].r_info);
            uint32_t sidx = ELF32_R_SYM(rels[r].r_info);
            uint32_t P = map[target_sec].addr + rels[r].r_offset;   // patch site
            uint32_t S = 0;
            bool is_func = false;

            if (type != R_ARM_NONE) {
                rc = resolve(sidx, &S, &is_func);
                if (rc != LOAD_OK) break;
            }

            switch (type) {
            case R_ARM_NONE:
                break;

            case R_ARM_ABS32:
            case R_ARM_TARGET1: {
                uint32_t *p = (uint32_t *)(uintptr_t)P;
                // S ALREADY carries the Thumb bit. AAELF stores it in st_value
                // itself: a symbol referring to Thumb code has bit 0 set, which
                // readelf shows as st_value = 1 for a function at the start of
                // its section. Adding another one here produced a pointer to
                // function+2 with the Thumb bit CLEAR, and calling it faulted
                // with INVSTATE the instant a package command was invoked.
                //
                // app_main escaped it because the entry point is resolved
                // through the symbol lookup rather than a relocation — which is
                // exactly why loading a package always worked and running one
                // never did.
                *p = *p + S;
                break;
            }

            case R_ARM_REL32: {
                uint32_t *p = (uint32_t *)(uintptr_t)P;
                *p = *p + S - P;
                break;
            }

            case R_ARM_PREL31: {
                uint32_t *p = (uint32_t *)(uintptr_t)P;
                int32_t a = (int32_t)(*p << 1) >> 1;          // sign-extend 31
                int32_t v = (int32_t)(S + a - P);
                *p = (*p & 0x80000000u) | ((uint32_t)v & 0x7fffffffu);
                break;
            }

            case R_ARM_ABS16: {
                uint16_t *p = (uint16_t *)(uintptr_t)P;
                *p = (uint16_t)(*p + S);
                break;
            }

            case R_ARM_ABS8: {
                uint8_t *p = (uint8_t *)(uintptr_t)P;
                *p = (uint8_t)(*p + S);
                break;
            }

            case R_ARM_THM_MOVW_ABS_NC:
            case R_ARM_THM_MOVT_ABS: {
                uint16_t *p = (uint16_t *)(uintptr_t)P;
                uint32_t val = S + (is_func ? 1u : 0u);
                if (type == R_ARM_THM_MOVT_ABS) val >>= 16;
                val &= 0xffffu;
                uint32_t imm4 = (val >> 12) & 0xf, i = (val >> 11) & 1;
                uint32_t imm3 = (val >> 8) & 0x7, imm8 = val & 0xff;
                p[0] = (uint16_t)((p[0] & 0xfbf0) | (i << 10) | imm4);
                p[1] = (uint16_t)((p[1] & 0x8f00) | (imm3 << 12) | imm8);
                break;
            }

            case R_ARM_THM_CALL:
            case R_ARM_THM_JUMP24: {
                uint16_t *p = (uint16_t *)(uintptr_t)P;
                // The addend encoded in an unresolved BL is -4, not 0: the ARM
                // ELF convention folds the Thumb pipeline offset into it, so
                // the ABI's result is (S + A) - P with no separate +4. Adding
                // the +4 back here recovers the ACTUAL target address, which is
                // what both the direct branch and the veneer need. Treating the
                // raw addend as part of the target instead lands every call
                // four bytes early — it links, it loads, and it jumps into the
                // middle of the previous instruction.
                int32_t addend = thumb_decode_branch(p);
                // The Thumb bit is masked OFF for the arithmetic. S carries it
                // (see R_ARM_ABS32 above), and a branch displacement computed
                // from an odd address is wrong by one — Thumb branch offsets are
                // even by construction. It goes back on for the veneer target
                // below, where an address is what is wanted rather than an
                // offset.
                uint32_t real_target = (S & ~1u) + (uint32_t)addend + 4;
                int32_t disp = (int32_t)(real_target - (P + 4));
                if (!thumb_bl_in_range(disp)) {
                    // In SVC mode this cannot legitimately happen and must not
                    // be papered over. Firmware symbols already resolved to a
                    // veneer next door, and anything else is inside the image —
                    // so an out-of-range branch here means a target that is
                    // neither, and emitting a veneer for it would build a
                    // trampoline holding a raw address where the supervisor
                    // expects an index.
                    if (g_veneer_mode == LOADER_VENEER_SVC) {
                        rc = LOAD_ERR_RELOC_RANGE;
                        break;
                    }
                    // This is the normal case, not an edge case: firmware lives
                    // in XIP flash at 0x10000000 and the app in SRAM at
                    // 0x20000000, so every call into the firmware is 256 MB
                    // away and no BL can reach it. Route it through a veneer.
                    uint32_t v = veneer_emit(out, real_target | 1u);
                    if (!v) { rc = LOAD_ERR_RELOC_RANGE; break; }
                    disp = (int32_t)((v & ~1u) - (P + 4));
                    if (!thumb_bl_in_range(disp)) { rc = LOAD_ERR_RELOC_RANGE; break; }
                }
                thumb_encode_branch(p, disp);
                break;
            }

            default:
                set_detail(out, elf_reloc_name(type));
                rc = LOAD_ERR_RELOC_UNSUPPORTED;
                break;
            }
        }
        free(rels);
    }

    // --- entry point
    if (rc == LOAD_OK) {
        rc = LOAD_ERR_NO_ENTRY;
        for (uint32_t i = 0; i < nsyms; i++) {
            if (syms[i].st_shndx == SHN_UNDEF) continue;
            if (strcmp(strs + syms[i].st_name, "app_main") != 0) continue;
            if (syms[i].st_shndx >= eh.e_shnum || !map[syms[i].st_shndx].loaded) break;
            uint32_t a = map[syms[i].st_shndx].addr + syms[i].st_value;
            out->entry = (int (*)(int))(uintptr_t)(a | 1u);       // Thumb
            rc = LOAD_OK;
            break;
        }
    }

    free(strs);
    free(syms);
    free(names);
    if (rc != LOAD_OK) app_unload(out);
    return rc;
}

uint32_t app_return_gate(const LoadedApp *app) {
    if (!app || !app->veneer_gates) return 0;
    return ((uint32_t)(uintptr_t)app->veneers + VENEER_GATE_RETURN) | 1u;
}

uint32_t app_enter_gate(const LoadedApp *app) {
    if (!app || !app->veneer_gates) return 0;
    return ((uint32_t)(uintptr_t)app->veneers + VENEER_GATE_ENTER) | 1u;
}

uint32_t app_exit_gate(const LoadedApp *app) {
    if (!app || !app->veneer_gates) return 0;
    return ((uint32_t)(uintptr_t)app->veneers + VENEER_GATE_EXIT) | 1u;
}

void app_unload(LoadedApp *app) {
    if (!app) return;
    // The RAW pointers, not the aligned ones. They are usually the same address
    // and occasionally are not, which is the worst possible shape for a bug:
    // it works on the bench and corrupts the heap in the field.
    if (app->image_raw)   g_free(app->image_raw);
    if (app->veneers_raw) g_free(app->veneers_raw);
    app->image = app->image_raw = nullptr;
    app->veneers = app->veneers_raw = nullptr;
    app->data = nullptr;
    app->entry = nullptr;
    app->image_size = app->text_size = app->data_size = 0;
    app->veneer_size = app->veneers_used = app->veneer_gates = 0;
    app->bytes_allocated = 0;
}
