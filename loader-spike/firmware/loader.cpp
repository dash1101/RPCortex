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
#include <stdio.h>

// Relocations processed per read. 256 of them is two kilobytes of bss and one
// read per 256 patches — small enough to be free, large enough that the read
// count never matters.
#define REL_CHUNK 256
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

// Windows into the ELF, shared by the two functions that parse one.
//
// app_load and app_pic_install are never in flight at the same time — a package
// is being installed or it is being loaded — and a private copy of each in both
// would be six kilobytes of bss for nothing. Named here rather than declared
// static inside each function so the sharing is a decision instead of an
// accident, and so the consequence is written down once: NOTHING may call one of
// those two from inside the other.
static Elf32_Rel g_relbuf[REL_CHUNK];
static Elf32_Sym g_symwin[REL_CHUNK];
// The section table, likewise shared. Five kilobytes each, and app_peek,
// app_load, app_pic_measure and app_pic_install all want one — four private
// copies would be twenty kilobytes of bss to hold the same table one function at
// a time. Same rule as above, and it is the same rule for the same reason.
static Elf32_Shdr g_shdr[LOADER_MAX_SECTIONS];

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
        case R_ARM_BASE_PREL:       return "R_ARM_BASE_PREL";
        case R_ARM_GOT_BREL:        return "R_ARM_GOT_BREL";
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
        case LOAD_ERR_SLOT_ALIGN:        return "the slot is not block-aligned";
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

// The header, and nothing else.
//
// Reading a package's name and version used to require LOADING it — relocating
// the whole image into RAM just to find out what it was called. That is fine
// until the package being asked about is the one already running, because then
// installing an upgrade needs two copies of the image resident at once. On a
// device with 96 KB free and a 53 KB image it simply cannot be done, and the
// message is "out of memory" from a step nobody expected to allocate.
//
// This reads the section table and the header section. The only allocation is
// the section-name strings, a few hundred bytes, freed before returning.
LoadResult app_peek(const AppSource &src, RpcAppHeader *out) {
    memset(out, 0, sizeof(*out));

    Elf32_Ehdr eh;
    if (!read_exact(src, 0, &eh, sizeof(eh))) return LOAD_ERR_READ;
    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 || eh.e_ident[4] != 1)
        return LOAD_ERR_NOT_ELF;
    if (eh.e_type != ET_REL)      return LOAD_ERR_NOT_REL;
    if (eh.e_machine != EM_ARM)   return LOAD_ERR_NOT_ARM;
    if (eh.e_shnum > LOADER_MAX_SECTIONS) return LOAD_ERR_TOO_MANY_SECTIONS;

    Elf32_Shdr *const sh = g_shdr;                 // shared; see the note above
    if (!read_exact(src, eh.e_shoff, sh, eh.e_shnum * sizeof(Elf32_Shdr)))
        return LOAD_ERR_READ;

    const Elf32_Shdr &shstr = sh[eh.e_shstrndx];
    char *names = (char *)malloc(shstr.sh_size);
    if (!names) return LOAD_ERR_OOM;
    if (!read_exact(src, shstr.sh_offset, names, shstr.sh_size)) {
        free(names);
        return LOAD_ERR_READ;
    }

    int hdr_idx = -1;
    for (int i = 0; i < eh.e_shnum; i++)
        if (strcmp(names + sh[i].sh_name, ".rpc_app_header") == 0) { hdr_idx = i; break; }
    if (hdr_idx < 0) { free(names); return LOAD_ERR_NO_HEADER; }

    bool ok = read_exact(src, sh[hdr_idx].sh_offset, out, sizeof(RpcAppHeader));
    free(names);
    if (!ok) return LOAD_ERR_READ;
    if (out->magic != RPC_APP_MAGIC) return LOAD_ERR_BAD_MAGIC;
    if (out->api_major != RPC_API_MAJOR || out->api_minor > RPC_API_MINOR)
        return LOAD_ERR_API_MISMATCH;
    return LOAD_OK;
}

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
    Elf32_Shdr *const sh = g_shdr;                 // shared; see the note above
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

    // Streamed the same way in the pre-scan below and the relocation pass proper,
    // so it is declared once here. Static rather than on the stack for the reason
    // the whole file is: this runs on the shell's 8 KB stack, and two kilobytes
    // of it is a third of what a command has to play with.

    // --- size the GOT (position-independent packages only) -------------------
    //
    // A package built -fPIC -msingle-pic-base reaches every global through a GOT
    // indexed off r9. A relocatable object carries no GOT — the loader
    // synthesises one at the base of the writable block — so its size has to be
    // known before that block is allocated, which is why this runs first.
    //
    // Counted by DISTINCT symbol index, which is an upper bound on distinct by
    // address (two names can resolve to one place); the relocation pass dedups on
    // the resolved address and never needs more than this many slots. A bare
    // bitmap of the symbol table counts them — the mapping itself is rebuilt on
    // the real pass, so none of this has to survive the function. A non-PIC
    // package has no GOT_BREL, falls straight through with got_bytes = 0, and
    // takes exactly the path it did before any of this existed.
    uint32_t got_bytes = 0;
    {
        int symtab_i = -1;
        for (int i = 0; i < eh.e_shnum; i++)
            if (sh[i].sh_type == SHT_SYMTAB) { symtab_i = i; break; }
        uint32_t nsyms = symtab_i < 0 ? 0 : sh[symtab_i].sh_size / sizeof(Elf32_Sym);
        if (nsyms) {
            uint8_t *seen = (uint8_t *)calloc((nsyms + 7) / 8, 1);
            if (!seen) { free(names); return LOAD_ERR_OOM; }
            uint32_t got_slots = 0;
            for (int i = 0; i < eh.e_shnum; i++) {
                if (sh[i].sh_type != SHT_REL) continue;
                uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel);
                uint32_t have = 0, first = 0;
                for (uint32_t r = 0; r < n; r++) {
                    if (r >= first + have) {
                        first = r;
                        have  = n - r < REL_CHUNK ? n - r : REL_CHUNK;
                        if (!read_exact(src, sh[i].sh_offset + first * sizeof(Elf32_Rel),
                                        g_relbuf, have * sizeof(Elf32_Rel))) {
                            free(seen); free(names); return LOAD_ERR_READ;
                        }
                    }
                    const Elf32_Rel *rels = g_relbuf - first;
                    if (ELF32_R_TYPE(rels[r].r_info) != R_ARM_GOT_BREL) continue;
                    uint32_t sidx = ELF32_R_SYM(rels[r].r_info);
                    if (sidx >= nsyms) continue;
                    if (!(seen[sidx >> 3] & (1u << (sidx & 7)))) {
                        seen[sidx >> 3] |= (1u << (sidx & 7));
                        got_slots++;
                    }
                }
            }
            free(seen);
            got_bytes = (got_slots * 4u + 3u) & ~3u;
        }
    }

    // --- lay the allocatable sections out in two halves, and TWO allocations.
    //
    // Everything the app may not write to first, everything it must be able to
    // write to after, because those two want OPPOSITE permissions and a section
    // order that interleaves them can be given neither. Code has to be
    // executable and must not be writable; data has to be writable and must
    // never be executed. Sorted by SHF_WRITE, which is the ELF flag that says
    // exactly which is which.
    //
    // The halves are padded to APP_BLOCK_ALIGN so the protection hardware can
    // cover each one exactly. Without that a region would either stop short —
    // leaving the tail unprotected — or run past the end and apply the app's
    // permissions to whatever the heap handed out next.
    //
    // TWO ALLOCATIONS rather than one, since 2026-08-08. The two halves were
    // always separate regions to everything that consumes them — the MPU, the
    // pointer checker, the fault reporter — and only the heap thought they were
    // one thing. Asking for them together meant a 123 KB package needed 123 KB
    // in a single piece, and upgrading it on a device already running it could
    // not find that: the copy being replaced leaves a hole its own old size,
    // which the larger new image does not fit in. The offsets below are still
    // computed as though it were one block, because the writable half's
    // offsets already include text_end and rebasing is where that is undone.
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
            // The GOT goes at the very base of the writable half, so its origin
            // — the value r9 holds — is the block base. Reserve it before any
            // writable section is placed, so those sections land above it and
            // `data` itself is the GOT. Zero for a non-PIC package, so the split
            // is exactly where it was.
            total += got_bytes;
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

    // Every block is over-allocated by one alignment and the useful part is
    // placed on the first boundary inside. The raw pointers are kept because
    // they, not the aligned ones, are what the allocator will take back.
    //
    // The read-only half is asked for FIRST and is the larger of the two, so
    // the request most likely to fail is made while the heap is least picked
    // over. An app with no writable sections asks for nothing at all rather
    // than a token block nobody uses.
    uint32_t text_bytes = text_end;
    uint32_t data_bytes = total - text_end;

    // WHICH allocation failed, named in the detail. Three requests of very
    // different sizes fail with one error code, and "out of memory" without the
    // number is a message nobody can act on — the whole point of splitting the
    // image was to make the binding request smaller, and there is no way to
    // tell whether that worked without knowing which one ran out.
    uint8_t *image_raw = (uint8_t *)g_alloc((text_bytes ? text_bytes : APP_BLOCK_ALIGN) + APP_BLOCK_ALIGN);
    if (!image_raw) {
        snprintf(out->detail, sizeof(out->detail), "%u bytes for the code half",
                 (unsigned)text_bytes);
        free(names);
        return LOAD_ERR_OOM;
    }

    uint8_t *data_raw = nullptr;
    if (data_bytes) {
        data_raw = (uint8_t *)g_alloc(data_bytes + APP_BLOCK_ALIGN);
        if (!data_raw) {
            snprintf(out->detail, sizeof(out->detail), "%u bytes for the data half",
                     (unsigned)data_bytes);
            g_free(image_raw); free(names);
            return LOAD_ERR_OOM;
        }
    }

    uint8_t *veneers_raw = (uint8_t *)g_alloc(veneer_bytes + APP_BLOCK_ALIGN);
    if (!veneers_raw) {
        snprintf(out->detail, sizeof(out->detail), "%u bytes for the veneers",
                 (unsigned)veneer_bytes);
        g_free(image_raw);
        if (data_raw) g_free(data_raw);
        free(names);
        return LOAD_ERR_OOM;
    }

    uint8_t *image   = (uint8_t *)block_align((uintptr_t)image_raw);
    uint8_t *data    = data_raw ? (uint8_t *)block_align((uintptr_t)data_raw) : nullptr;
    uint8_t *veneers = (uint8_t *)block_align((uintptr_t)veneers_raw);

    out->image_raw   = image_raw;
    out->data_raw    = data_raw;
    out->veneers_raw = veneers_raw;
    out->text_size   = text_bytes;
    out->data        = data;
    out->data_size   = data_bytes;
    // The GOT occupies the first got_bytes of the writable block. It is not an
    // ELF section, so the section-load loop below never touches it — clear it
    // here so its alignment tail is zero rather than whatever the heap held, and
    // record its size, which is the one flag the entry points read to know this
    // package needs r9 pointed at it.
    out->got_size    = got_bytes;
    out->got_count   = 0;              // filled as GOT_BREL relocations resolve
    if (data && got_bytes) memset(data, 0, got_bytes);
    out->image = image;
    // A SUM, not a span. The two halves are separate allocations and there is
    // no address at image + image_size.
    out->image_size = text_bytes + data_bytes;
    out->veneers = veneers;
    out->veneer_size = veneer_bytes;
    out->veneers_used = 0;
    out->veneer_gates = 0;
    // The gates go in before anything else, so their offsets are fixed and the
    // reuse scan has a definite place to start.
    if (g_veneer_mode == LOADER_VENEER_SVC) veneer_write_gates(out);
    // What was actually taken from the heap, alignment slack included — so the
    // "did not release N bytes" report after a package runs stays truthful.
    out->bytes_allocated = (text_bytes ? text_bytes : APP_BLOCK_ALIGN) + data_bytes +
                           veneer_bytes + (data_raw ? 3 : 2) * APP_BLOCK_ALIGN;

    for (int i = 0; i < eh.e_shnum; i++) {
        if (!map[i].loaded) continue;
        // REBASE PER HALF. The offsets were laid out as though the two were one
        // block, so a writable section's offset already counts text_end — which
        // is exactly the amount to take back off before adding its own base.
        // Getting this wrong loads the app successfully and faults later at an
        // address that looks plausible.
        bool writable = (sh[i].sh_flags & SHF_WRITE) != 0;
        map[i].addr += writable ? (uint32_t)(uintptr_t)data - text_end
                                : (uint32_t)(uintptr_t)image;
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

    // THE SYMBOL TABLE IS NOT READ INTO RAM EITHER.
    //
    // At ~48 KB for a Nova D1-sized package it was the FOURTH large block the
    // loader asked for, after the code half, the data half and the veneers —
    // and it was the one that failed. A device with 263 KB free but no 48 KB
    // hole left once those three had carved up the biggest region could not
    // install its own update, with an "out of memory" that named nothing:
    // this was the one OOM path that set no detail.
    //
    // A symbol is read through a small window instead, the way .rel.* and the
    // string table already are. The relocation loop touches indices in bursts
    // that cluster — a section's own symbols are contiguous — and the app_main
    // scan walks them in order, so a 256-entry window turns what would be a
    // read per lookup into a handful of refills, and the 48 KB stays on flash.
    const uint32_t symtab_off = sh[symtab_idx].sh_offset;
    static const Elf32_Sym sym_zero = {};
    uint32_t symwin_first = 0, symwin_have = 0;
    bool sym_io = true;
    auto sym_at = [&](uint32_t idx) -> const Elf32_Sym & {
        if (idx >= nsyms) { sym_io = false; return sym_zero; }
        if (idx < symwin_first || idx >= symwin_first + symwin_have) {
            uint32_t base = idx - (idx % REL_CHUNK);
            uint32_t cnt  = nsyms - base < REL_CHUNK ? nsyms - base : REL_CHUNK;
            if (!read_exact(src, symtab_off + base * sizeof(Elf32_Sym),
                            g_symwin, cnt * sizeof(Elf32_Sym))) {
                sym_io = false;
                return sym_zero;
            }
            symwin_first = base; symwin_have = cnt;
        }
        return g_symwin[idx - symwin_first];
    };

    uint32_t strtab_idx = sh[symtab_idx].sh_link;
    if (strtab_idx >= eh.e_shnum) {
        free(names); app_unload(out); return LOAD_ERR_READ;
    }
    // THE STRING TABLE IS NOT READ INTO RAM.
    //
    // It is 32 KB for a Nova D1-sized package and was held for the whole of
    // relocation, alongside the symbol table and the image itself — a peak of
    // over 200 KB for a package whose image is 123. That is what made an
    // in-place upgrade fail on a device reporting 277 KB free.
    //
    // Almost none of it is ever looked at. A name is needed only for a symbol
    // the firmware has to resolve — sixty-odd imports — and for the text of an
    // error. Every other relocation resolves through the section map and never
    // touches a character. So the names are read one at a time, from wherever
    // the package is, and the 32 KB stays on flash.
    const uint32_t strtab_off  = sh[strtab_idx].sh_offset;
    const uint32_t strtab_size = sh[strtab_idx].sh_size;
    static char namebuf[96];

    auto sym_name = [&](uint32_t st_name) -> const char * {
        namebuf[0] = 0;
        if (st_name >= strtab_size) return namebuf;
        uint32_t want = strtab_size - st_name;
        if (want > sizeof(namebuf) - 1) want = sizeof(namebuf) - 1;
        if (!read_exact(src, strtab_off + st_name, namebuf, want)) {
            namebuf[0] = 0;
            return namebuf;
        }
        // A name longer than the buffer is truncated, and truncation is safe:
        // api_index_of will not match a partial name, so it becomes a clean
        // "undefined symbol" naming most of what was wanted rather than a wrong
        // match. Every symbol the firmware exports is a short C name.
        namebuf[sizeof(namebuf) - 1] = 0;
        return namebuf;
    };

    LoadResult rc = LOAD_OK;

    // Resolve one symbol to an absolute address. Defined symbols come from the
    // loaded image; undefined ones must be exported by the firmware.
    auto resolve = [&](uint32_t idx, uint32_t *addr, bool *is_func) -> LoadResult {
        const Elf32_Sym &s = sym_at(idx);
        if (!sym_io) return LOAD_ERR_READ;
        *is_func = (ELF32_ST_TYPE(s.st_info) == STT_FUNC);
        if (s.st_shndx == SHN_UNDEF) {
            const char *nm = sym_name(s.st_name);
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
            set_detail(out, sym_name(s.st_name));
            return LOAD_ERR_UNDEF_SYMBOL;
        }
        *addr = map[s.st_shndx].addr + s.st_value;
        return LOAD_OK;
    };

    // The synthesised GOT: an array of resolved addresses at the base of the
    // writable block, which the package reaches through r9. A GOT_BREL stores the
    // byte offset of a symbol's slot; the code adds it to r9 and loads the slot.
    //
    // Deduped on the RESOLVED address, so two names for one place — a section
    // symbol and a label at its start, say — share a slot. The sizing pass
    // counted by symbol index, which is an upper bound on this, so the append
    // can only run out if that pass and this one disagree, and then it is a clean
    // refusal rather than a write past the block. Slot addresses carry the Thumb
    // bit for functions exactly as resolve() returns them; realapp_test walks the
    // writable half and would catch a code slot that lost it.
    uint32_t *got = (uint32_t *)out->data;      // valid only when got_bytes != 0
    uint32_t got_used = 0;
    auto got_offset_for = [&](uint32_t S, uint32_t *off) -> bool {
        for (uint32_t k = 0; k < got_used; k++)
            if (got[k] == S) { *off = k * 4u; return true; }
        if ((got_used + 1u) * 4u > got_bytes) return false;
        got[got_used] = S;
        *off = got_used * 4u;
        got_used++;
        return true;
    };

    for (int i = 0; i < eh.e_shnum && rc == LOAD_OK; i++) {
        if (sh[i].sh_type != SHT_REL) continue;
        uint32_t target_sec = sh[i].sh_info;
        if (target_sec >= eh.e_shnum || !map[target_sec].loaded) continue;

        uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel);

        // STREAMED, not read whole, through the shared window at the top of the
        // file. .rel.text alone is 29 KB on a Nova D1, and a
        // relocation is looked at exactly once in order — there is no reason for
        // the whole section to be resident, and holding it was 29 KB of the peak
        // that stopped a device upgrading its own package.
        uint32_t have = 0, first = 0;

        for (uint32_t r = 0; r < n && rc == LOAD_OK; r++) {
            if (r >= first + have) {
                first = r;
                have  = n - r < REL_CHUNK ? n - r : REL_CHUNK;
                if (!read_exact(src, sh[i].sh_offset + first * sizeof(Elf32_Rel),
                                g_relbuf, have * sizeof(Elf32_Rel))) {
                    rc = LOAD_ERR_READ;
                    break;
                }
            }
            const Elf32_Rel *rels = g_relbuf - first;   // so rels[r] still reads
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

            case R_ARM_GOT_BREL: {
                // The value the code needs is the byte offset of S's GOT slot
                // from the GOT origin (r9). S was resolved above and already
                // carries the Thumb bit if it names a function, so the slot is
                // stored verbatim.
                //
                // The addend rides in the patch-site word and is 0 for every
                // GOT_BREL GCC emits with these flags (measured across every
                // package). A nonzero one would mean "S's slot, then N bytes on",
                // which an r9-relative load cannot express — so it is refused
                // rather than made to point one slot away and fault far from here.
                uint32_t *p = (uint32_t *)(uintptr_t)P;
                if (*p != 0) {
                    set_detail(out, "GOT_BREL addend");
                    rc = LOAD_ERR_RELOC_UNSUPPORTED;
                    break;
                }
                uint32_t off;
                if (!got_offset_for(S, &off)) { rc = LOAD_ERR_RELOC_RANGE; break; }
                *p = off;
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
    }

    // What the GOT actually cost, for the same accounting the veneer counts get.
    out->got_count = got_used;

    // --- entry point
    if (rc == LOAD_OK) {
        rc = LOAD_ERR_NO_ENTRY;
        for (uint32_t i = 0; i < nsyms; i++) {
            const Elf32_Sym &s = sym_at(i);
            if (!sym_io) { rc = LOAD_ERR_READ; break; }
            if (s.st_shndx == SHN_UNDEF) continue;
            if (strcmp(sym_name(s.st_name), "app_main") != 0) continue;
            if (s.st_shndx >= eh.e_shnum || !map[s.st_shndx].loaded) break;
            uint32_t a = map[s.st_shndx].addr + s.st_value;
            out->entry = (int (*)(int))(uintptr_t)(a | 1u);       // Thumb
            rc = LOAD_OK;
            break;
        }
    }

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

uint32_t app_pic_base(const LoadedApp *app) {
    // The GOT origin, which is the base of the writable block. Zero for a
    // non-PIC package (no GOT), which every entry point reads as "leave r9".
    if (!app || !app->got_size) return 0;
    return (uint32_t)(uintptr_t)app->data;
}

// ================================================================= PIC slot
//
// Task #93 stage 3. app_pic_install assembles the position-independent read-only
// blob (.text + .rodata + firmware veneers) and records a manifest; app_pic_load
// instantiates it from the blob + manifest with the ELF nowhere in sight. See the
// header for the shape of it. What is here is the two functions and the one thing
// that decides whether any of it is safe: the blob carries NO absolute address,
// so it is byte-identical wherever it lands and realapp_test proves that by
// building it twice at two addresses and comparing.
//
// SVC veneer form only. The blob holds `svc #0` trampolines that name a firmware
// function by its ABI INDEX, which is stable across a firmware rebuild — a raw
// address (the DIRECT form) is not, and baking one into flash would call the
// wrong function after any update. A package that would run privileged takes the
// ordinary copy-to-RAM app_load path; it never reaches here.

// What an install COSTS, measured by the loader rather than reasoned about from
// section sizes.
//
// An install is the memory-hungriest thing the OS does, and every previous
// failure of it was an allocation nobody had counted: the read-only half, then
// the string table, then the symbol table. Section sums do not find those,
// because the thing that runs out is the largest free BLOCK, and what matters is
// how much is held at once and how big the biggest single request was. Both are
// recorded here so realapp_test can assert them and a device can print them.
//
// The size rides in an eight-byte header ahead of the block, which is also what
// keeps the returned pointer eight-aligned on both a device and a 64-bit host.
static uint32_t g_pic_live, g_pic_peak, g_pic_biggest;
// How many times the page loop had to stop short of a page boundary because a
// patch site straddled it. Reported so that "the straddle path is handled" can
// be told apart from "the straddle path never ran" — an untaken branch that
// corrupts a package when it is finally taken is the worst of both.
static uint32_t g_pic_cuts;

static void *pic_alloc(uint32_t n) {
    uint64_t *p = (uint64_t *)malloc((size_t)n + 8);
    if (!p) return nullptr;
    p[0] = n;
    g_pic_live += n;
    if (g_pic_live > g_pic_peak)    g_pic_peak = g_pic_live;
    if (n > g_pic_biggest)          g_pic_biggest = n;
    return p + 1;
}
static void *pic_calloc(uint32_t n) {
    void *p = pic_alloc(n);
    if (p) memset(p, 0, n);
    return p;
}
static void pic_free(void *v) {
    if (!v) return;
    uint64_t *p = (uint64_t *)v - 1;
    g_pic_live -= (uint32_t)p[0];
    free(p);
}

void app_pic_install_cost(uint32_t *peak, uint32_t *biggest, uint32_t *page_cuts) {
    if (peak)      *peak      = g_pic_peak;
    if (biggest)   *biggest   = g_pic_biggest;
    if (page_cuts) *page_cuts = g_pic_cuts;
}

// The SVC firmware veneer, written into the blob's veneer region. Deduped by ABI
// index, the same trampoline serving every call to one function. Returns the byte
// offset within `vbuf`, or -1 if the region is full — a clean refusal, never a
// write past it. Mirrors the SVC arm of veneer_emit; kept separate because that
// one appends to a live LoadedApp pool and this one fills a plain buffer.
static int32_t pic_veneer(uint8_t *vbuf, uint32_t cap, uint32_t *used, uint32_t index) {
    for (uint32_t off = 0; off < *used; off += VENEER_BYTES)
        if (*(uint32_t *)(vbuf + off + VENEER_LITERAL) == index)
            return (int32_t)off;
    if (*used + VENEER_BYTES > cap) return -1;
    uint16_t *c = (uint16_t *)(vbuf + *used);
    c[0] = 0xf8df; c[1] = 0xc008;   // ldr.w ip, [pc, #8]
    c[2] = 0xdf00;                  // svc  #0
    c[3] = 0xe7fe;                  // b .
    c[4] = 0xbe00;                  // bkpt, in the pad
    c[5] = 0x0000;
    *(uint32_t *)(vbuf + *used + VENEER_LITERAL) = index;   // the index, not an address
    int32_t off = (int32_t)*used;
    *used += VENEER_BYTES;
    return off;
}

// The blob is emitted a PAGE at a time, never assembled.
//
// The first version of this built all 123 KB of novad1's read-only half in RAM
// and handed it to the sink in one call. No board can do that: a booted device
// has around 89 KB in its largest free block, whatever the free total says. A
// page buffer and a forward walk cost four kilobytes instead.
//
// The price is that the relocation table is re-read once per page rather than
// once, because a patch site is only known to belong to a page when its offset
// is compared against that page. For novad1 that is about a megabyte of streamed
// reads against an install whose flash erase alone takes seconds — and it is
// measured in realapp_test rather than assumed, so if it ever stops being true
// the number says so.
#define PIC_PAGE 4096u

// --- the pre-flight ----------------------------------------------------------
//
// Everything app_pic_install works out from the SECTION HEADERS, worked out
// again without writing a byte. See the header for why it is worth having a
// second function that agrees with the first: the erase comes before the proof,
// so the refusals have to come before the erase.
//
// The relocation scan is one pass and it answers three questions at once — how
// many GOT slots, how many ABS32 fixups the recipe could need, and whether every
// relocation is one the producer can actually emit. That last one is the reason
// this is a scan rather than arithmetic over section sizes: an unsupported
// relocation found by the producer is a package destroyed and not replaced,
// and it costs nothing to see it coming.
LoadResult app_pic_measure(const AppSource &src, PicMeasure *out) {
    memset(out, 0, sizeof(*out));

    Elf32_Ehdr eh;
    if (!read_exact(src, 0, &eh, sizeof(eh))) return LOAD_ERR_READ;
    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 || eh.e_ident[4] != 1)
        return LOAD_ERR_NOT_ELF;
    if (eh.e_type != ET_REL)      return LOAD_ERR_NOT_REL;
    if (eh.e_machine != EM_ARM)   return LOAD_ERR_NOT_ARM;
    if (eh.e_shnum > LOADER_MAX_SECTIONS) return LOAD_ERR_TOO_MANY_SECTIONS;

    Elf32_Shdr *const sh = g_shdr;                 // shared; see the note above
    if (!read_exact(src, eh.e_shoff, sh, eh.e_shnum * sizeof(Elf32_Shdr)))
        return LOAD_ERR_READ;

    // `placed` is not an array here. app_pic_install keeps one because it needs
    // the offsets too; the predicate itself is just these two tests, and writing
    // it once keeps the two functions from disagreeing about what a section is.
    auto placed = [&](uint32_t i) -> bool {
        return i < (uint32_t)eh.e_shnum &&
               (sh[i].sh_flags & SHF_ALLOC) && sh[i].sh_size != 0;
    };

    int symtab_idx = -1;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB) { symtab_idx = i; break; }
    const uint32_t nsyms = symtab_idx < 0 ? 0 : sh[symtab_idx].sh_size / sizeof(Elf32_Sym);

    // --- the one relocation pass.
    uint32_t got_slots = 0, abs_cap = 0;
    bool     supported = true;
    if (nsyms) {
        uint8_t *seen = (uint8_t *)pic_calloc((nsyms + 7) / 8);
        if (!seen) return LOAD_ERR_OOM;
        for (int i = 0; i < eh.e_shnum && supported; i++) {
            if (sh[i].sh_type != SHT_REL) continue;
            const uint32_t tgt = sh[i].sh_info;
            // A relocation section whose target is not loaded relocates nothing
            // that ends up in the blob or the RAM block — .rel.debug_* and the
            // like. app_pic_install skips them in both of its passes.
            if (!placed(tgt)) continue;
            const bool writable = (sh[tgt].sh_flags & SHF_WRITE) != 0;
            if (writable) abs_cap += sh[i].sh_size / sizeof(Elf32_Rel);

            uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel), have = 0, first = 0;
            for (uint32_t r = 0; r < n; r++) {
                if (r >= first + have) {
                    first = r; have = n - r < REL_CHUNK ? n - r : REL_CHUNK;
                    if (!read_exact(src, sh[i].sh_offset + first * sizeof(Elf32_Rel),
                                    g_relbuf, have * sizeof(Elf32_Rel))) {
                        pic_free(seen); return LOAD_ERR_READ;
                    }
                }
                const Elf32_Rel *rels = g_relbuf - first;
                const uint32_t type = ELF32_R_TYPE(rels[r].r_info);
                if (type == R_ARM_NONE) continue;
                // EXACTLY the two switches in app_pic_install: the blob accepts a
                // GOT load or a branch, the RAM block accepts a pointer. Anything
                // else is a package the producer would refuse halfway through.
                if (writable) {
                    if (type != R_ARM_ABS32 && type != R_ARM_TARGET1) supported = false;
                } else if (type != R_ARM_GOT_BREL && type != R_ARM_THM_CALL &&
                           type != R_ARM_THM_JUMP24) {
                    supported = false;
                }
                if (!supported) break;
                if (type != R_ARM_GOT_BREL) continue;
                uint32_t s = ELF32_R_SYM(rels[r].r_info);
                if (s < nsyms && !(seen[s >> 3] & (1u << (s & 7)))) {
                    seen[s >> 3] |= (1u << (s & 7)); got_slots++;
                }
            }
        }
        pic_free(seen);
    }
    out->got_bytes = (got_slots * 4u + 3u) & ~3u;
    // No GOT means it was not built -fPIC — a plain package, which app_load
    // handles exactly as it always did. An unsupported relocation means the same
    // answer for a different reason, and both are "take the copy path", not
    // "refuse to install".
    out->pic = supported && got_slots != 0;

    // --- the same two-half layout, so the two agree on where things land.
    uint32_t bpos = 0, rpos = out->got_bytes;
    uint32_t data_lo = 0xffffffffu, data_hi = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        if (!placed(i) || (sh[i].sh_flags & SHF_WRITE)) continue;
        uint32_t al = sh[i].sh_addralign ? sh[i].sh_addralign : 4; if (al < 4) al = 4;
        bpos = (bpos + al - 1) & ~(al - 1);
        bpos += sh[i].sh_size;
    }
    for (int i = 0; i < eh.e_shnum; i++) {
        if (!placed(i) || !(sh[i].sh_flags & SHF_WRITE)) continue;
        uint32_t al = sh[i].sh_addralign ? sh[i].sh_addralign : 4; if (al < 4) al = 4;
        rpos = (rpos + al - 1) & ~(al - 1);
        if (sh[i].sh_type != SHT_NOBITS) {
            if (rpos < data_lo) data_lo = rpos;
            if (rpos + sh[i].sh_size > data_hi) data_hi = rpos + sh[i].sh_size;
        }
        rpos += sh[i].sh_size;
    }
    if (data_lo == 0xffffffffu) { data_lo = out->got_bytes; data_hi = out->got_bytes; }
    const uint32_t data_size = data_hi - data_lo;
    out->ram_size = (rpos + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);

    // The veneer pool's CEILING, which is where nearly all the slack in the bound
    // comes from: one veneer per distinct firmware function, bounded by the ABI's
    // own size. For a Nova D1 that is 2.5 KB reserved against 1.2 KB used.
    uint32_t reloc_count = 0;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_REL) reloc_count += sh[i].sh_size / sizeof(Elf32_Rel);
    uint32_t veneer_bound = reloc_count;
    uint32_t max_targets  = api_symbol_count();
    if (max_targets && veneer_bound > max_targets) veneer_bound = max_targets;

    const uint32_t veneer_off = (bpos + 3u) & ~3u;
    out->ro_bound = veneer_off + (veneer_bound + 1) * VENEER_BYTES;

    // And the body, in pkgslot_commit's order: blob, GOT recipe, ABS32 recipe,
    // .data initialisers, each on a four-byte boundary. The final round up to a
    // program page is pkgslot's, so it is added there and not here.
    uint32_t b = out->ro_bound;
    b = ((b + 3u) & ~3u) + got_slots * (uint32_t)sizeof(PicGotEntry);
    b = ((b + 3u) & ~3u) + abs_cap   * (uint32_t)sizeof(PicAbs32);
    b = ((b + 3u) & ~3u) + data_size;
    out->body_bound = b;
    return LOAD_OK;
}

LoadResult app_pic_install(const AppSource &src, SlotWrite sink, void *sink_ctx,
                           PicManifest *m) {
    memset(m, 0, sizeof(*m));
    g_pic_peak = g_pic_biggest = g_pic_cuts = 0;   // this install's cost, not the last one's

    Elf32_Ehdr eh;
    if (!read_exact(src, 0, &eh, sizeof(eh))) return LOAD_ERR_READ;
    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) != 0 || eh.e_ident[4] != 1)
        return LOAD_ERR_NOT_ELF;
    if (eh.e_type != ET_REL)      return LOAD_ERR_NOT_REL;
    if (eh.e_machine != EM_ARM)   return LOAD_ERR_NOT_ARM;
    if (eh.e_shnum > LOADER_MAX_SECTIONS) return LOAD_ERR_TOO_MANY_SECTIONS;

    Elf32_Shdr *const sh = g_shdr;                 // shared; see the note above
    if (!read_exact(src, eh.e_shoff, sh, eh.e_shnum * sizeof(Elf32_Shdr)))
        return LOAD_ERR_READ;
    const Elf32_Shdr &shstr = sh[eh.e_shstrndx];
    char *names = (char *)pic_alloc(shstr.sh_size);
    if (!names) return LOAD_ERR_OOM;
    if (!read_exact(src, shstr.sh_offset, names, shstr.sh_size)) {
        pic_free(names); return LOAD_ERR_READ;
    }

    // Header: the ABI it was built against goes into the manifest so a slot built
    // for one firmware is refused by another rather than calling stale indices.
    int hdr_idx = -1;
    for (int i = 0; i < eh.e_shnum; i++)
        if (strcmp(names + sh[i].sh_name, ".rpc_app_header") == 0) { hdr_idx = i; break; }
    if (hdr_idx < 0) { pic_free(names); return LOAD_ERR_NO_HEADER; }
    RpcAppHeader hdr;
    if (!read_exact(src, sh[hdr_idx].sh_offset, &hdr, sizeof(hdr))) {
        pic_free(names); return LOAD_ERR_READ;
    }
    if (hdr.magic != RPC_APP_MAGIC) { pic_free(names); return LOAD_ERR_BAD_MAGIC; }
    if (hdr.api_major != RPC_API_MAJOR || hdr.api_minor > RPC_API_MINOR) {
        pic_free(names); return LOAD_ERR_API_MISMATCH;
    }
    m->header    = hdr;              // carried whole: name and version, not just the ABI
    m->api_major = hdr.api_major;
    m->api_minor = hdr.api_minor;

    // --- the symbol table, through a window rather than in RAM.
    //
    // 71 KB for a Nova D1-sized package, and it was the largest thing this
    // function held after the blob. Read a window at a time exactly as app_load
    // does — a section's own symbols are contiguous and the relocation loop
    // touches them in bursts, so a 256-entry window turns a read per lookup into
    // a handful of refills.
    int symtab_idx = -1;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB) { symtab_idx = i; break; }
    if (symtab_idx < 0) { pic_free(names); return LOAD_ERR_NO_ENTRY; }
    const uint32_t nsyms      = sh[symtab_idx].sh_size / sizeof(Elf32_Sym);
    const uint32_t symtab_off = sh[symtab_idx].sh_offset;
    static const Elf32_Sym sym_zero = {};
    uint32_t symwin_first = 0, symwin_have = 0;
    bool sym_io = true;
    auto sym_at = [&](uint32_t idx) -> const Elf32_Sym & {
        if (idx >= nsyms) { sym_io = false; return sym_zero; }
        if (idx < symwin_first || idx >= symwin_first + symwin_have) {
            uint32_t base = idx - (idx % REL_CHUNK);
            uint32_t cnt  = nsyms - base < REL_CHUNK ? nsyms - base : REL_CHUNK;
            if (!read_exact(src, symtab_off + base * sizeof(Elf32_Sym),
                            g_symwin, cnt * sizeof(Elf32_Sym))) {
                sym_io = false;
                return sym_zero;
            }
            symwin_first = base; symwin_have = cnt;
        }
        return g_symwin[idx - symwin_first];
    };
    // The string table is streamed a name at a time, as app_load does: 32 KB for
    // a Nova D1 and it is only ever wanted one name at a time.
    if (sh[symtab_idx].sh_link >= (uint32_t)eh.e_shnum) { pic_free(names); return LOAD_ERR_READ; }
    const uint32_t strtab_off  = sh[sh[symtab_idx].sh_link].sh_offset;
    const uint32_t strtab_size = sh[sh[symtab_idx].sh_link].sh_size;
    static char namebuf[96];
    auto sym_name = [&](uint32_t st_name) -> const char * {
        namebuf[0] = 0;
        if (st_name >= strtab_size) return namebuf;
        uint32_t want = strtab_size - st_name;
        if (want > sizeof(namebuf) - 1) want = sizeof(namebuf) - 1;
        if (!read_exact(src, strtab_off + st_name, namebuf, want)) namebuf[0] = 0;
        namebuf[sizeof(namebuf) - 1] = 0;
        return namebuf;
    };

    // --- GOT size, exactly as app_load counts it: distinct symbol index carrying
    // a GOT_BREL, an upper bound on distinct-by-address. Reserved at the base of
    // the RAM block so .data lands above it and r9 is the block base.
    uint32_t got_bytes = 0;
    if (nsyms) {
        uint8_t *seen = (uint8_t *)pic_calloc((nsyms + 7) / 8);
        if (!seen) { pic_free(names); return LOAD_ERR_OOM; }
        uint32_t slots = 0;
        for (int i = 0; i < eh.e_shnum; i++) {
            if (sh[i].sh_type != SHT_REL) continue;
            uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel), have = 0, first = 0;
            for (uint32_t r = 0; r < n; r++) {
                if (r >= first + have) {
                    first = r; have = n - r < REL_CHUNK ? n - r : REL_CHUNK;
                    if (!read_exact(src, sh[i].sh_offset + first * sizeof(Elf32_Rel),
                                    g_relbuf, have * sizeof(Elf32_Rel))) {
                        pic_free(seen); pic_free(names); return LOAD_ERR_READ;
                    }
                }
                const Elf32_Rel *rels = g_relbuf - first;
                if (ELF32_R_TYPE(rels[r].r_info) != R_ARM_GOT_BREL) continue;
                uint32_t s = ELF32_R_SYM(rels[r].r_info);
                if (s < nsyms && !(seen[s >> 3] & (1u << (s & 7)))) {
                    seen[s >> 3] |= (1u << (s & 7)); slots++;
                }
            }
        }
        pic_free(seen);
        got_bytes = (slots * 4u + 3u) & ~3u;
    }

    // --- lay out the blob (RO half) and the RAM block (RW half).
    // Blob: non-writable ALLOC sections, then the veneer region. RAM: the GOT,
    // then writable ALLOC sections. Offsets here are region-relative and final —
    // the blob's because it is position-independent, the RAM's because r9 makes
    // them so. Same SHF_WRITE sort app_load uses, so the two agree on the split.
    static uint32_t blob_off[LOADER_MAX_SECTIONS];
    static uint32_t ram_off[LOADER_MAX_SECTIONS];
    static bool     placed[LOADER_MAX_SECTIONS];
    for (int i = 0; i < eh.e_shnum; i++) { blob_off[i] = ram_off[i] = 0; placed[i] = false; }

    uint32_t bpos = 0, rpos = got_bytes;
    for (int i = 0; i < eh.e_shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
        if (sh[i].sh_flags & SHF_WRITE) continue;
        uint32_t al = sh[i].sh_addralign ? sh[i].sh_addralign : 4; if (al < 4) al = 4;
        bpos = (bpos + al - 1) & ~(al - 1);
        blob_off[i] = bpos; placed[i] = true; bpos += sh[i].sh_size;
    }
    uint32_t veneer_off = (bpos + 3u) & ~3u;    // Thumb-aligned, past the RO sections
    for (int i = 0; i < eh.e_shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
        if (!(sh[i].sh_flags & SHF_WRITE)) continue;
        uint32_t al = sh[i].sh_addralign ? sh[i].sh_addralign : 4; if (al < 4) al = 4;
        rpos = (rpos + al - 1) & ~(al - 1);
        ram_off[i] = rpos; placed[i] = true; rpos += sh[i].sh_size;
    }

    // Where .text and .rodata sit in the blob, for the manifest (the whole blob is
    // one RO_EXEC region to the MPU, as app_load's image already is; these are for
    // accounting and the on-device W^X note).
    for (int i = 0; i < eh.e_shnum; i++) {
        if (!placed[i] || (sh[i].sh_flags & SHF_WRITE)) continue;
        if (sh[i].sh_flags & SHF_EXECINSTR) m->text_size += sh[i].sh_size;
        else { if (!m->rodata_size) m->rodata_off = blob_off[i]; m->rodata_size += sh[i].sh_size; }
    }

    // The veneer region's ceiling: one per distinct firmware target, bounded by
    // the ABI just as app_load bounds its pool.
    uint32_t reloc_count = 0;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_REL) reloc_count += sh[i].sh_size / sizeof(Elf32_Rel);

    // How many ABS32 fixups the recipe can possibly need: the relocations whose
    // TARGET is a writable section, and nothing else. Every relocation against
    // .text becomes a GOT slot or a baked branch, and none of those reach the
    // abs array.
    //
    // Sizing that array by the TOTAL relocation count instead — which is what it
    // did — asked for 6227 entries where 1212 are used: 74 KB of recipe for 14 KB
    // of content, the largest single allocation in the whole install and one
    // nobody had counted. The bound comes from the section headers alone, so this
    // costs no read.
    uint32_t abs_cap = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        if (sh[i].sh_type != SHT_REL) continue;
        uint32_t tgt = sh[i].sh_info;
        if (tgt >= (uint32_t)eh.e_shnum || !placed[tgt] || !(sh[tgt].sh_flags & SHF_WRITE))
            continue;
        abs_cap += sh[i].sh_size / sizeof(Elf32_Rel);
    }

    uint32_t veneer_bound = reloc_count;
    uint32_t max_targets = api_symbol_count();
    if (max_targets && veneer_bound > max_targets) veneer_bound = max_targets;
    uint32_t veneer_cap = (veneer_bound + 1) * VENEER_BYTES;

    // The veneer pool, and the two recipe arrays. All three are small and all
    // three are needed for the whole pass: a veneer is deduped by ABI index and a
    // GOT slot by its resolved target, so both have to remember what they have
    // already handed out. The pool is written into the blob after the last page
    // of .text and .rodata, which is why it can be built in RAM while they cannot.
    uint8_t *veneers = (uint8_t *)pic_calloc(veneer_cap);
    if (!veneers) { pic_free(names); return LOAD_ERR_OOM; }
    uint32_t got_cap = got_bytes / 4;
    m->got = (PicGotEntry *)pic_calloc((got_cap ? got_cap : 1) * sizeof(PicGotEntry));
    m->abs = (PicAbs32 *)pic_calloc((abs_cap ? abs_cap : 1) * sizeof(PicAbs32));
    if (!m->got || !m->abs) { pic_free(veneers); pic_free(names);
                              app_pic_manifest_free(m); return LOAD_ERR_OOM; }

    LoadResult rc = LOAD_OK;
    uint32_t veneer_used = 0;
    uint32_t got_used = 0;

    // Resolve a symbol to (class, region-relative value, is_func). Firmware
    // (undefined) symbols become an SVC veneer in the blob, so a firmware pointer
    // and a firmware call both go through the one gateway — exactly app_load's SVC
    // rule, in the blob rather than a live pool.
    //
    // Resolution is MEMOISED, and the reason is the page pass rather than the
    // arithmetic. Walking the blob a page at a time means the relocation table is
    // re-read once per page, and the symbol each entry names is fetched through a
    // 256-entry window that the scattered indices keep evicting: novad1's install
    // read fourteen megabytes for a three-hundred-kilobyte file, nearly all of it
    // window refills. Three kilobytes of direct-mapped memo takes that to under
    // two. Safe to cache because resolving twice was always meant to give the
    // same answer — pic_veneer dedups by ABI index and got_slot by resolved
    // target, so neither allocates twice for one symbol.
    struct PicMemo { uint32_t idx, value; uint8_t cls, func, valid; };
    static PicMemo memo[256];
    memset(memo, 0, sizeof(memo));
    auto resolve = [&](uint32_t idx, uint8_t *cls, uint32_t *value, bool *is_func) -> LoadResult {
        PicMemo &mm = memo[idx & 255u];
        if (mm.valid && mm.idx == idx) {
            *cls = mm.cls; *value = mm.value; *is_func = mm.func != 0;
            return LOAD_OK;
        }
        auto keep = [&](void) {
            mm.idx = idx; mm.value = *value; mm.cls = *cls;
            mm.func = *is_func ? 1 : 0; mm.valid = 1;
        };
        const Elf32_Sym &s = sym_at(idx);
        if (!sym_io) return LOAD_ERR_READ;
        *is_func = (ELF32_ST_TYPE(s.st_info) == STT_FUNC);
        if (s.st_shndx == SHN_UNDEF) {
            const char *nm = sym_name(s.st_name);
            int ix = api_index_of(nm);
            if (ix < 0) return LOAD_ERR_UNDEF_SYMBOL;
            int32_t v = pic_veneer(veneers, veneer_cap, &veneer_used, (uint32_t)ix);
            if (v < 0) return LOAD_ERR_RELOC_RANGE;
            // The veneer is code: its blob offset is 16-aligned (even), so the
            // Thumb bit is set here. A DEFINED symbol carries its Thumb bit in
            // st_value already (AAELF), so `value` is the FINAL pointer either way
            // and load adds nothing — adding a Thumb bit twice clears it and was
            // the bug app_load's ABS32 comment warns about.
            *cls = PIC_CLASS_SLOT; *value = (veneer_off + (uint32_t)v) | 1u; *is_func = true;
            keep();
            return LOAD_OK;
        }
        if (s.st_shndx == SHN_ABS) { *cls = PIC_CLASS_ABS; *value = s.st_value; keep(); return LOAD_OK; }
        if (s.st_shndx >= eh.e_shnum || !placed[s.st_shndx]) return LOAD_ERR_UNDEF_SYMBOL;
        if (sh[s.st_shndx].sh_flags & SHF_WRITE) { *cls = PIC_CLASS_RAM;  *value = ram_off[s.st_shndx]  + s.st_value; }
        else                                     { *cls = PIC_CLASS_SLOT; *value = blob_off[s.st_shndx] + s.st_value; }
        keep();
        return LOAD_OK;
    };
    // Find-or-add a GOT slot for a resolved target, deduped by the resolved tuple
    // exactly as app_load dedups by resolved address. Returns the slot's byte
    // offset (what a GOT_BREL site stores), or -1 if the reserved GOT overflows.
    auto got_slot = [&](uint8_t cls, uint32_t value, bool is_func) -> int32_t {
        for (uint32_t k = 0; k < got_used; k++)
            if (m->got[k].cls == cls && m->got[k].value == value &&
                m->got[k].is_func == (is_func ? 1 : 0))
                return (int32_t)(k * 4u);
        if (got_used >= got_cap) return -1;
        m->got[got_used].cls = cls; m->got[got_used].value = value;
        m->got[got_used].is_func = is_func ? 1 : 0;
        return (int32_t)(got_used++ * 4u);
    };

    // --- the page pass: read, patch, emit, forget.
    //
    // Each turn fills the buffer from whichever read-only sections overlap it,
    // applies every relocation whose four-byte site lies wholly inside, and hands
    // the result to the sink. A site that straddles the far edge is not patched;
    // instead the emit stops short AT that site and the next turn starts there,
    // so half a patched word is never written to a page that is then programmed
    // and never revisited. That is the only thing here that is not obvious, and
    // it is the only thing that would corrupt a package rather than refuse it.
    //
    // It works because relocation sites do not overlap: if a site S straddles the
    // page end, no other site can extend past S, so cutting the emit at S cannot
    // orphan the tail of a patch already applied. That is a property of the
    // compiler's output rather than of the format, so it is CHECKED — the highest
    // patched byte is compared against the cut before anything is written.
    static uint8_t page[PIC_PAGE];
    uint32_t pos = 0;
    while (pos < veneer_off && rc == LOAD_OK) {
        uint32_t end = pos + PIC_PAGE;
        if (end > veneer_off) end = veneer_off;
        memset(page, 0, end - pos);        // gaps between sections read as zero
        uint32_t patched_to = pos;         // highest byte any patch has touched

        for (int i = 0; i < eh.e_shnum && rc == LOAD_OK; i++) {
            if (!placed[i] || (sh[i].sh_flags & SHF_WRITE)) continue;
            if (sh[i].sh_type == SHT_NOBITS) continue;   // no RO NOBITS in practice
            uint32_t s0 = blob_off[i], s1 = blob_off[i] + sh[i].sh_size;
            uint32_t lo = s0 > pos ? s0 : pos, hi = s1 < end ? s1 : end;
            if (lo >= hi) continue;
            if (!read_exact(src, sh[i].sh_offset + (lo - s0), page + (lo - pos), hi - lo))
                rc = LOAD_ERR_READ;
        }

        uint32_t emit_end = end;
        for (int i = 0; i < eh.e_shnum && rc == LOAD_OK; i++) {
            if (sh[i].sh_type != SHT_REL) continue;
            uint32_t tgt = sh[i].sh_info;
            if (tgt >= (uint32_t)eh.e_shnum || !placed[tgt] || (sh[tgt].sh_flags & SHF_WRITE))
                continue;
            // Skip a relocation section whose whole target lies outside this page.
            // For a package built from many small sections that is most of them;
            // for one folded together with `ld -r` it is none, which is the case
            // the read volume was measured against.
            if (blob_off[tgt] >= end || blob_off[tgt] + sh[tgt].sh_size <= pos) continue;

            uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel), have = 0, first = 0;
            for (uint32_t r = 0; r < n && rc == LOAD_OK; r++) {
                if (r >= first + have) {
                    first = r; have = n - r < REL_CHUNK ? n - r : REL_CHUNK;
                    if (!read_exact(src, sh[i].sh_offset + first * sizeof(Elf32_Rel),
                                    g_relbuf, have * sizeof(Elf32_Rel))) { rc = LOAD_ERR_READ; break; }
                }
                const Elf32_Rel *rels = g_relbuf - first;
                uint32_t type = ELF32_R_TYPE(rels[r].r_info);
                if (type == R_ARM_NONE) continue;
                uint32_t site = blob_off[tgt] + rels[r].r_offset;   // blob-relative
                if (site < pos || site >= end) continue;            // another page's
                if (site + 4 > end) {                               // straddles the edge
                    if (site < emit_end) emit_end = site;
                    continue;
                }

                uint8_t cls; uint32_t value; bool is_func;
                rc = resolve(ELF32_R_SYM(rels[r].r_info), &cls, &value, &is_func);
                if (rc != LOAD_OK) break;
                if (site + 4 > patched_to) patched_to = site + 4;

                switch (type) {
                case R_ARM_GOT_BREL: {
                    uint32_t *p = (uint32_t *)(page + (site - pos));
                    if (*p != 0) { rc = LOAD_ERR_RELOC_UNSUPPORTED; break; }   // addend, cannot express
                    int32_t off = got_slot(cls, value, is_func);
                    if (off < 0) { rc = LOAD_ERR_RELOC_RANGE; break; }
                    *p = (uint32_t)off;
                    break;
                }
                case R_ARM_THM_CALL:
                case R_ARM_THM_JUMP24: {
                    // Branch target must be inside the blob — a local function or
                    // a veneer. Both are PIC_CLASS_SLOT and the displacement is
                    // between two blob offsets, so it is the same wherever the
                    // blob lands. THIS LINE is also what keeps slot-resident code
                    // from ever branching to a RAM address: a BL reaches 16 MB and
                    // flash is 256 MB from SRAM, so a veneer in RAM could not be
                    // reached from here at any address.
                    if (cls != PIC_CLASS_SLOT) { rc = LOAD_ERR_RELOC_RANGE; break; }
                    uint16_t *p = (uint16_t *)(page + (site - pos));
                    int32_t addend = thumb_decode_branch(p);
                    int32_t disp = (int32_t)((value & ~1u) + (uint32_t)addend + 4u) - (int32_t)(site + 4u);
                    if (!thumb_bl_in_range(disp)) { rc = LOAD_ERR_RELOC_RANGE; break; }
                    thumb_encode_branch(p, disp);
                    break;
                }
                default:
                    rc = LOAD_ERR_RELOC_UNSUPPORTED;
                    break;
                }
            }
        }

        if (rc != LOAD_OK) break;
        // A page whose very first site straddles its end would make no progress.
        // It cannot happen — a site is four bytes and a page is four kilobytes —
        // but a loop that could spin forever on a malformed input is refused
        // rather than trusted.
        if (emit_end <= pos) { rc = LOAD_ERR_RELOC_UNSUPPORTED; break; }
        // The overlap assumption, checked rather than believed: nothing patched
        // in this turn may reach past the point the emit stops at, or its tail
        // would be dropped and re-read unpatched.
        if (patched_to > emit_end) { rc = LOAD_ERR_RELOC_UNSUPPORTED; break; }
        if (emit_end != end) g_pic_cuts++;
        if (!sink(sink_ctx, pos, page, emit_end - pos)) { rc = LOAD_ERR_READ; break; }
        pos = emit_end;
    }

    // --- ABS32 fixups against the writable sections, and the .data initialisers.
    // These are the pointers that live in RAM: recorded as {site, class, value}
    // so load applies them once against the real slot and RAM addresses.
    uint32_t abs_used = 0;
    uint32_t data_lo = 0xffffffffu, data_hi = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        if (!placed[i] || !(sh[i].sh_flags & SHF_WRITE)) continue;
        if (sh[i].sh_type == SHT_NOBITS) continue;                 // .bss: zeroed at load
        if (ram_off[i] < data_lo) data_lo = ram_off[i];
        if (ram_off[i] + sh[i].sh_size > data_hi) data_hi = ram_off[i] + sh[i].sh_size;
    }
    if (data_lo == 0xffffffffu) { data_lo = got_bytes; data_hi = got_bytes; }
    m->data_off  = data_lo;
    m->data_size = data_hi - data_lo;
    if (rc == LOAD_OK) {
        m->data_init = (uint8_t *)pic_calloc(m->data_size ? m->data_size : 1);
        if (!m->data_init) rc = LOAD_ERR_OOM;
    }
    for (int i = 0; i < eh.e_shnum && rc == LOAD_OK; i++) {
        if (!placed[i] || !(sh[i].sh_flags & SHF_WRITE) || sh[i].sh_type == SHT_NOBITS) continue;
        if (!read_exact(src, sh[i].sh_offset, m->data_init + (ram_off[i] - data_lo), sh[i].sh_size))
            rc = LOAD_ERR_READ;
    }
    for (int i = 0; i < eh.e_shnum && rc == LOAD_OK; i++) {
        if (sh[i].sh_type != SHT_REL) continue;
        uint32_t tgt = sh[i].sh_info;
        if (tgt >= (uint32_t)eh.e_shnum || !placed[tgt] || !(sh[tgt].sh_flags & SHF_WRITE))
            continue;
        uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel), have = 0, first = 0;
        for (uint32_t r = 0; r < n && rc == LOAD_OK; r++) {
            if (r >= first + have) {
                first = r; have = n - r < REL_CHUNK ? n - r : REL_CHUNK;
                if (!read_exact(src, sh[i].sh_offset + first * sizeof(Elf32_Rel),
                                g_relbuf, have * sizeof(Elf32_Rel))) { rc = LOAD_ERR_READ; break; }
            }
            const Elf32_Rel *rels = g_relbuf - first;
            uint32_t type = ELF32_R_TYPE(rels[r].r_info);
            if (type == R_ARM_NONE) continue;
            if (type != R_ARM_ABS32 && type != R_ARM_TARGET1) { rc = LOAD_ERR_RELOC_UNSUPPORTED; break; }
            if (abs_used >= abs_cap) { rc = LOAD_ERR_RELOC_RANGE; break; }
            uint8_t cls; uint32_t value; bool is_func;
            rc = resolve(ELF32_R_SYM(rels[r].r_info), &cls, &value, &is_func);
            if (rc != LOAD_OK) break;
            // `value` is the final region-relative pointer, Thumb bit and all —
            // load adds base(cls) and nothing else. app_load's ABS32 case is
            // *p = *p + S with S already carrying the bit; this splits S into
            // base + value and keeps the same arithmetic.
            m->abs[abs_used].site  = ram_off[tgt] + rels[r].r_offset;
            m->abs[abs_used].cls   = cls;
            m->abs[abs_used].value = value;
            abs_used++;
        }
    }

    // THE VENEERS GO LAST, after the ABS32 pass and not before it.
    //
    // A pointer to a firmware function stored in .data resolves to a veneer just
    // as a call does, and that pointer is only seen in the pass above — so a
    // package whose only reference to some firmware function is a stored pointer
    // allocates its veneer there. Writing the pool straight after the page loop
    // would leave that veneer in RAM and not in the blob, and the package would
    // branch into erased flash the first time it used the pointer. Nothing else
    // below allocates one, so this is the point at which the pool is final.
    if (rc == LOAD_OK && veneer_used &&
        !sink(sink_ctx, veneer_off, veneers, veneer_used))
        rc = LOAD_ERR_READ;

    // --- entry point.
    if (rc == LOAD_OK) {
        rc = LOAD_ERR_NO_ENTRY;
        for (uint32_t i = 0; i < nsyms; i++) {
            const Elf32_Sym &s = sym_at(i);
            if (!sym_io) { rc = LOAD_ERR_READ; break; }
            if (s.st_shndx == SHN_UNDEF || s.st_shndx >= eh.e_shnum) continue;
            if (!placed[s.st_shndx] || (sh[s.st_shndx].sh_flags & SHF_WRITE)) continue;
            if (strcmp(sym_name(s.st_name), "app_main") != 0) continue;
            m->entry_off = blob_off[s.st_shndx] + s.st_value;
            rc = LOAD_OK;
            break;
        }
    }

    if (rc == LOAD_OK) {
        m->veneer_off  = veneer_off;
        m->veneer_size = veneer_used;
        m->ro_size     = veneer_off + veneer_used;
        m->got_bytes   = got_bytes;
        m->got_count   = got_used;
        m->abs_count   = abs_used;
        uint32_t ram = (rpos + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);
        m->ram_size    = ram;
    }

    pic_free(veneers);
    pic_free(names);
    if (rc != LOAD_OK) app_pic_manifest_free(m);
    return rc;
}

LoadResult app_pic_load(const void *slot, const PicManifest *m, LoadedApp *out) {
    memset(out, 0, sizeof(*out));
    if (!slot || !m) return LOAD_ERR_READ;
    // The one guard against the failure that does not fault: a slot built against
    // a firmware whose ABI has moved would call the wrong function through a baked
    // index. Refuse it, the same test app_peek/app_load make on the ELF.
    if (m->api_major != RPC_API_MAJOR || m->api_minor > RPC_API_MINOR)
        return LOAD_ERR_API_MISMATCH;
    // The blob becomes the code region verbatim, so its base has to be one the
    // protection unit can describe. A slot is a flash sector and always is; this
    // is here so that if that ever stops being true it arrives as a refusal
    // naming itself rather than as a package that cannot fetch its own code.
    if ((uintptr_t)slot & (APP_BLOCK_ALIGN - 1)) return LOAD_ERR_SLOT_ALIGN;

    // The one resident allocation: GOT + .data + .bss, in a single block, its base
    // the GOT origin and so the value of r9. This is the whole cost of a loaded
    // package now — the 122 KB read-only half stays in `slot` (flash on device).
    uint8_t *data_raw = (uint8_t *)g_alloc(m->ram_size + APP_BLOCK_ALIGN);
    if (!data_raw) return LOAD_ERR_OOM;
    uint8_t *data = (uint8_t *)block_align((uintptr_t)data_raw);
    memset(data, 0, m->ram_size);                         // zeros GOT, gaps and .bss

    // .data initialisers FIRST, then the ABS32 fixups exactly once — the order the
    // header warns about. The addend rode in the copied word; this adds the base.
    if (m->data_size) memcpy(data + m->data_off, m->data_init, m->data_size);

    auto base = [&](uint8_t cls) -> uint32_t {
        if (cls == PIC_CLASS_SLOT) return (uint32_t)(uintptr_t)slot;
        if (cls == PIC_CLASS_RAM)  return (uint32_t)(uintptr_t)data;
        return 0;                                         // PIC_CLASS_ABS
    };
    // value already carries the Thumb bit where one is due (see app_pic_install),
    // so the whole GOT slot is base + value and an ABS32 is *p += base + value.
    uint32_t *got = (uint32_t *)data;
    for (uint32_t k = 0; k < m->got_count; k++)
        got[k] = base(m->got[k].cls) + m->got[k].value;
    for (uint32_t a = 0; a < m->abs_count; a++)
        *(uint32_t *)(data + m->abs[a].site) += base(m->abs[a].cls) + m->abs[a].value;

    // The gates stay in RAM: unprivileged code returning from a supervisor call
    // executes the privilege-restoring instruction, and that is the one thing kept
    // off flash so it is not also the first test of unprivileged fetch-from-flash.
    // The firmware veneers, which only ever `svc`, live in the blob beside .text.
    //
    // ROUNDED TO A WHOLE BLOCK, and that is not tidiness — it is the whole
    // difference between a slot-loaded package running and hard-faulting.
    //
    // A protection region is described by a base and a LIMIT, both on a 32-byte
    // granule, so mpu_v8_encode refuses a size that is not a multiple of one:
    // rounding it down would leave the tail unprotected and rounding it up would
    // cover memory the package was never given. Refusing is right. But the
    // refusal is silent — set_region programs no region rather than a wrong one
    // — and unprivileged code has no default map, so "no region" means "no
    // access", including no instruction fetch.
    //
    // The three gates are 48 bytes. That is one and a half blocks, so the region
    // was refused every time, and the FIRST instruction a slot-loaded package
    // ever executes unprivileged is the `bx r3` in the enter gate. It faulted
    // there, every time, on hardware:
    //
    //     HARD FAULT in greet  pc=0x2006b1f8  lr=0x2006b201  cfsr=0x00000001
    //
    // — pc is the gate's `bx` at veneers+24, lr the exit gate at veneers+32.
    // app_load's own pool never hit this because it block-aligns its size; this
    // one asked for the bare 48.
    uint32_t gate_bytes = (VENEER_GATE_BYTES + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);
    uint8_t *ven_raw = (uint8_t *)g_alloc(gate_bytes + APP_BLOCK_ALIGN);
    if (!ven_raw) { g_free(data_raw); return LOAD_ERR_OOM; }
    out->veneers     = (void *)block_align((uintptr_t)ven_raw);
    out->veneers_raw = ven_raw;
    out->veneer_size = gate_bytes;
    veneer_write_gates(out);                              // sets veneers_used / veneer_gates

    out->image      = (void *)slot;      // read-only half, in the slot — NOT freed
    out->image_raw  = nullptr;
    // The same rule, for the same reason, on the blob: `ro_size` is only
    // four-aligned (veneer_off rounds to 4, and a veneer is 16), so five slot
    // installs in eight produced a code region the hardware would not take
    // either. Rounded UP rather than down: the tail of the blob is real package
    // code and leaving it out is a fault in the package's last function. What
    // the rounding adds is at most 31 bytes of the slot's own recipe, further
    // along the same flash — read-only to the hardware whatever the region says,
    // and only reachable while this package is the one running.
    out->text_size  = (m->ro_size + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);
    out->data       = data;
    out->data_raw   = data_raw;
    out->data_size  = m->ram_size;
    out->got_size   = m->got_bytes;
    out->got_count  = m->got_count;
    out->image_size = m->ro_size + m->ram_size;
    out->entry      = (int (*)(int))(uintptr_t)(((uint32_t)(uintptr_t)slot + m->entry_off) | 1u);
    out->header     = m->header;         // name and version: a registered command
                                         // is tagged by this, so it must be real
    // Resident RAM only: the block and the gate pool. The slot is flash, not heap.
    out->bytes_allocated = m->ram_size + gate_bytes + 2 * APP_BLOCK_ALIGN;
    return LOAD_OK;
}

void app_pic_manifest_free(PicManifest *m) {
    if (!m) return;
    // A slot-backed manifest owns nothing: its arrays are memory-mapped flash.
    // Freeing one would hand the allocator an address it never issued, which
    // corrupts the heap somewhere else entirely and much later.
    if (m->borrowed) { m->got = nullptr; m->abs = nullptr; m->data_init = nullptr;
                       m->got_count = m->abs_count = 0; return; }
    pic_free(m->got);       m->got = nullptr;
    pic_free(m->abs);       m->abs = nullptr;
    pic_free(m->data_init); m->data_init = nullptr;
    m->got_count = m->abs_count = 0;
}

void app_unload(LoadedApp *app) {
    if (!app) return;
    // The RAW pointers, not the aligned ones. They are usually the same address
    // and occasionally are not, which is the worst possible shape for a bug:
    // it works on the bench and corrupts the heap in the field.
    if (app->image_raw)   g_free(app->image_raw);
    if (app->data_raw)    g_free(app->data_raw);
    if (app->veneers_raw) g_free(app->veneers_raw);
    app->image = app->image_raw = nullptr;
    app->veneers = app->veneers_raw = nullptr;
    app->data = app->data_raw = nullptr;
    app->entry = nullptr;
    app->image_size = app->text_size = app->data_size = 0;
    app->got_size = app->got_count = 0;
    app->veneer_size = app->veneers_used = app->veneer_gates = 0;
    app->bytes_allocated = 0;
}
