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

static uint32_t veneer_emit(LoadedApp *app, uint32_t target) {
    // Reuse an existing veneer for the same target: a typical app calls
    // fw_printf a dozen times and one trampoline serves them all.
    uint8_t *base = (uint8_t *)app->veneers;
    for (uint32_t off = 0; off < app->veneers_used; off += VENEER_BYTES) {
        if (*(uint32_t *)(base + off + VENEER_LITERAL) == target)
            return (uint32_t)(uintptr_t)(base + off) | 1u;      // Thumb bit
    }
    if (app->veneers_used + VENEER_BYTES > app->veneer_size) return 0;
    uint8_t *v = base + app->veneers_used;
    uint16_t *c = (uint16_t *)v;
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

    Elf32_Shdr sh[LOADER_MAX_SECTIONS];
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

    // --- lay the allocatable sections out in one block. One allocation rather
    // than one per section: the heap this runs on is small, and a single free
    // is the only way "unload reclaims everything" is verifiable.
    SectionMap map[LOADER_MAX_SECTIONS];
    memset(map, 0, sizeof(map));
    uint32_t total = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
        uint32_t align = sh[i].sh_addralign ? sh[i].sh_addralign : 4;
        if (align < 4) align = 4;
        total = (total + align - 1) & ~(align - 1);
        map[i].addr = total;               // offset for now; rebased below
        map[i].size = sh[i].sh_size;
        map[i].loaded = true;
        total += sh[i].sh_size;
    }
    total = (total + 3) & ~3u;

    // Veneer pool. Sized from the number of relocations rather than guessed:
    // worst case is one veneer per distinct out-of-range target.
    uint32_t reloc_count = 0;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_REL) reloc_count += sh[i].sh_size / sizeof(Elf32_Rel);
    uint32_t veneer_bytes = (reloc_count + 1) * VENEER_BYTES;

    uint8_t *image = (uint8_t *)g_alloc(total ? total : 4);
    if (!image) { free(names); return LOAD_ERR_OOM; }
    uint8_t *veneers = (uint8_t *)g_alloc(veneer_bytes);
    if (!veneers) { g_free(image); free(names); return LOAD_ERR_OOM; }

    out->image = image;
    out->image_size = total;
    out->veneers = veneers;
    out->veneer_size = veneer_bytes;
    out->veneers_used = 0;
    out->bytes_allocated = total + veneer_bytes;

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
                // The Thumb bit belongs in a function POINTER. Without it a
                // call through a stored pointer lands in ARM state and faults
                // instantly on a core that has none.
                *p = *p + S + (is_func ? 1u : 0u);
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
                uint32_t real_target = S + (uint32_t)addend + 4;
                int32_t disp = (int32_t)(real_target - (P + 4));
                if (!thumb_bl_in_range(disp)) {
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

void app_unload(LoadedApp *app) {
    if (!app) return;
    if (app->image)   g_free(app->image);
    if (app->veneers) g_free(app->veneers);
    app->image = nullptr;
    app->veneers = nullptr;
    app->entry = nullptr;
    app->image_size = app->veneer_size = app->veneers_used = 0;
    app->bytes_allocated = 0;
}
