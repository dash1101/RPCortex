// Load every REAL built .app through the real loader.
//
// The other loader tests use synthetic objects, which only ever contain what the
// test author thought to put in them. This one loads what the compiler actually
// produced — .bss for each static, string literals in merge sections, and
// whatever relocation types GCC felt like emitting. That is the input the device
// gets. novad1 adds the other real shape: several objects folded into one with
// `ld -r`, which no compiler run produces directly.
//
// Two things it took to make this meaningful on a 64-bit host, both of which
// were themselves findings:
//
//   * the loader stores section addresses in uint32_t, correct on the device and
//     garbage when a host pointer is truncated into it. MAP_32BIT gives real
//     memory below 2 GB so the same code runs unchanged.
//   * the image and the veneer pool must be ADJACENT. Separate allocations put
//     them megabytes apart on a host, and a veneer more than 16 MB from the code
//     branching to it is out of BL range — a false failure that says nothing
//     about a device whose whole heap is 400 KB.
#include "loader.h"
#include "elf.h"
#include "mpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

// The loader stores section addresses in uint32_t, which is correct on the
// device and truncates a 64-bit host pointer to garbage. MAP_32BIT gives real
// memory below 2 GB so the same code can be exercised unchanged — which is the
// whole point of testing it here rather than only on hardware.
// A bump allocator over ONE region, so the image and the veneer pool land
// adjacent to each other exactly as they do on a device with a 400 KB heap.
// Separate mmaps put them megabytes apart, and a veneer more than 16 MB from the
// code that branches to it is out of BL range — a false failure that says
// nothing about the device.
static uint8_t *g_arena;
static size_t   g_used_bytes;
// A cap the tests can tighten, so "this heap only fits one image" is something
// that can be ARRANGED rather than only observed on a device.
static size_t   g_arena_cap;
#define ARENA (1u << 20)

static void *alloc32(size_t n) {
    if (!g_arena) {
        void *p = mmap(nullptr, ARENA, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        g_arena = (uint8_t *)p;
        // Start deliberately OFF a block boundary.
        //
        // mmap returns a page, and every size the loader asks for is a whole
        // number of blocks, so a bump allocator starting at zero hands back
        // perfectly aligned pointers forever — and a loader that had forgotten
        // to align at all would pass every check by luck. A real heap makes no
        // such promise; eight bytes is what malloc actually guarantees.
        g_used_bytes = 8;
    }
    n = (n + 7) & ~(size_t)7;
    size_t cap = g_arena_cap ? g_arena_cap : ARENA;
    if (g_used_bytes + n > cap) return nullptr;
    void *r = g_arena + g_used_bytes;
    g_used_bytes += n;
    return r;
}
static void free32(void *) { }      // one-shot; the arena goes away with us

static FILE *g_f;
static int g_loaded;   // how many apps actually got checked
static int file_read(void *, uint32_t off, void *dst, uint32_t len) {
    if (fseek(g_f, off, SEEK_SET) != 0) return -1;
    return (int)fread(dst, 1, len, g_f);
}
// A fake firmware symbol table: every fw_* resolves to a plausible address.
//
// The Thumb bit is SET, because that is what the device reports. api.cpp builds
// its table with SYM(fn) = (uint32_t)&fn, and the address of a Thumb function
// carries bit 0. An earlier version of this fake handed back even addresses,
// which is not a configuration any board is ever in — and it bypassed the exact
// arithmetic these relocations exist to get right, so a whole class of bug could
// not have shown up here.
//
// Addresses sit ~256 MB from the loaded image on purpose: that is the real
// distance from XIP flash to SRAM, so every firmware call is out of BL range and
// has to go through a veneer, exercising that path rather than the easy one.
//
// The table is SHARED by every app in a run and grows as symbols are met, so it
// has to be at least as wide as the union of what they all use — not what the
// largest one uses. At 64 it filled up partway through the list and the app that
// happened to be last failed with "unresolved symbol" naming a function the
// firmware exports perfectly well. Sized to the real ABI (169 entries) with room
// to grow, since being too small fails in a way that points at the wrong thing.
#define FAKE_SYMS 256
static char g_seen[FAKE_SYMS][40];
static uint32_t g_addr[FAKE_SYMS];
static int g_ns;
static int api_slot(const char *n) {
    for (int i = 0; i < g_ns; i++) if (!strcmp(g_seen[i], n)) return i;
    if (g_ns >= FAKE_SYMS) return -1;
    snprintf(g_seen[g_ns], 40, "%s", n);
    g_addr[g_ns] = (0x10001000u + (uint32_t)g_ns * 4u) | 1u;
    return g_ns++;
}
uint32_t api_lookup(const char *n) { int i = api_slot(n); return i < 0 ? 0 : g_addr[i]; }
// The index form, from the SAME table — a sandboxed package names a function by
// position, and a fake where the position and the address disagreed would let a
// mismatched pair pass unnoticed.
int api_index_of(const char *n) { return api_slot(n); }
uint32_t api_addr_at(uint32_t i) { return i < (uint32_t)g_ns ? g_addr[i] : 0; }
// The capacity, not the count so far: the loader asks this BEFORE resolving
// anything, to bound the veneer pool, and an answer that grew as symbols were
// met would size the pool from whichever app ran first.
uint32_t api_symbol_count(void) { return FAKE_SYMS; }

// Where a section landed in the loaded image, recomputed from the ELF so the
// answers match what was actually placed.
//
// This MIRRORS the loader's layout rather than sharing it, deliberately: a
// helper that called the loader's own code would agree with it whatever either
// of them did. It has to be kept in step by hand, which is the price of it
// being able to disagree.
//
// `want_off` is the offset being asked about. Returns the flags of the section
// containing it, or 0 if none does.
static uint32_t section_flags_at(const char *path, uint32_t want_off,
                                 uint32_t *text_end_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) { fclose(f); free(b); return 0; }
    fclose(f);
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b;
    Elf32_Shdr *sh = (Elf32_Shdr *)(b + eh->e_shoff);
    uint32_t total = 0, flags = 0, text_end = 0;
    // Two passes, non-writable first — the same order the loader uses so the
    // read-only half is contiguous and can be one protection region.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < eh->e_shnum; i++) {
            if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
            if (((sh[i].sh_flags & SHF_WRITE) != 0) != (pass == 1)) continue;
            uint32_t al = sh[i].sh_addralign ? sh[i].sh_addralign : 4;
            if (al < 4) al = 4;
            total = (total + al - 1) & ~(al - 1);
            if (want_off >= total && want_off < total + sh[i].sh_size)
                flags = sh[i].sh_flags | 0x80000000u;   // marker: a section was found
            total += sh[i].sh_size;
        }
        if (pass == 0) {
            total = (total + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);
            text_end = total;
        }
    }
    free(b);
    if (text_end_out) *text_end_out = text_end;
    return flags;
}

static bool in_text(const char *path, uint32_t off) {
    return (section_flags_at(path, off, nullptr) & SHF_EXECINSTR) != 0;
}

// --- the GOT, for a position-independent package -----------------------------
//
// The decisive stage-2 check. A PIC package reaches every global THROUGH a GOT
// indexed off r9, so if the loader built that GOT wrong the package cannot see
// its own data — and it must fail HERE, visibly, rather than on a board. The host
// cannot execute the code, so this does by hand exactly what the CPU would: for
// each GOT_BREL it reads the offset the loader patched into .text, follows it to
// the GOT slot, and checks the slot holds the address the symbol actually landed
// at. A wrong offset, an empty slot or a slot pointing at the wrong place is the
// whole failure mode, and every one of them shows up as a line here.
//
// Runs only for a PIC package (got_size != 0); a non-PIC one skips it entirely,
// so the default path is neither changed nor judged by a rule that is not its.
static int check_got(const char *path, const LoadedApp &app) {
    int bad = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) { fclose(f); free(b); return 0; }
    fclose(f);
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b;
    Elf32_Shdr *sh = (Elf32_Shdr *)(b + eh->e_shoff);

    // Redo the loader's placement to get each section's RUNTIME base, so a symbol
    // address can be computed independently and the GOT slot checked against it.
    // Two passes, non-writable first, with the GOT reserved at the base of the
    // writable half exactly as app_load does — this MIRRORS the loader on purpose
    // rather than sharing its code, so the two can disagree.
    static uint32_t base[LOADER_MAX_SECTIONS];
    static bool     placed[LOADER_MAX_SECTIONS];
    for (int i = 0; i < eh->e_shnum; i++) { base[i] = 0; placed[i] = false; }
    uint32_t total = 0, text_end = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < eh->e_shnum; i++) {
            if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0) continue;
            if (((sh[i].sh_flags & SHF_WRITE) != 0) != (pass == 1)) continue;
            uint32_t al = sh[i].sh_addralign ? sh[i].sh_addralign : 4; if (al < 4) al = 4;
            total = (total + al - 1) & ~(al - 1);
            base[i] = total; placed[i] = true;
            total += sh[i].sh_size;
        }
        if (pass == 0) {
            total = (total + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);
            text_end = total;
            total += app.got_size;                 // the GOT the loader reserved
        }
    }
    for (int i = 0; i < eh->e_shnum; i++) {
        if (!placed[i]) continue;
        bool w = (sh[i].sh_flags & SHF_WRITE) != 0;
        base[i] = w ? (uint32_t)(uintptr_t)app.data + (base[i] - text_end)
                    : (uint32_t)(uintptr_t)app.image + base[i];
    }

    if (text_end != app.text_size) {
        printf("       FAIL GOT: layout disagrees on where the halves divide (%u vs %u)\n",
               app.text_size, text_end); bad = 1;
    }
    if (app.got_size % 4) { printf("       FAIL GOT: size %u is not a whole number of slots\n", app.got_size); bad = 1; }
    if (app_pic_base(&app) != (uint32_t)(uintptr_t)app.data) {
        printf("       FAIL GOT: r9 base is not the writable block base\n"); bad = 1;
    }
    if ((uint32_t)(uintptr_t)app.data % APP_BLOCK_ALIGN) { printf("       FAIL GOT: base is off the block boundary\n"); bad = 1; }
    if (app.got_count * 4 > app.got_size) { printf("       FAIL GOT: %u slots overrun a %u-byte GOT\n", app.got_count, app.got_size); bad = 1; }

    int symtab_i = -1;
    for (int i = 0; i < eh->e_shnum; i++) if (sh[i].sh_type == SHT_SYMTAB) symtab_i = i;
    if (symtab_i < 0) { free(b); return bad; }
    Elf32_Sym *syms = (Elf32_Sym *)(b + sh[symtab_i].sh_offset);
    const char *str = (const char *)(b + sh[sh[symtab_i].sh_link].sh_offset);
    const uint32_t *got = (const uint32_t *)app.data;

    int sites = 0;
    for (int s = 0; s < eh->e_shnum; s++) {
        if (sh[s].sh_type != SHT_REL) continue;
        uint32_t tgt = sh[s].sh_info;
        if (tgt >= (uint32_t)eh->e_shnum || !placed[tgt]) continue;
        Elf32_Rel *rel = (Elf32_Rel *)(b + sh[s].sh_offset);
        uint32_t n = sh[s].sh_size / sizeof(Elf32_Rel);
        for (uint32_t r = 0; r < n; r++) {
            if (ELF32_R_TYPE(rel[r].r_info) != R_ARM_GOT_BREL) continue;
            uint32_t sidx = ELF32_R_SYM(rel[r].r_info);
            sites++;
            uint32_t off = *(const uint32_t *)(uintptr_t)(base[tgt] + rel[r].r_offset);
            if (off % 4 || off >= app.got_size) {
                printf("       FAIL GOT: site +0x%x holds offset %u, outside a %u-byte GOT\n",
                       rel[r].r_offset, off, app.got_size);
                bad = 1; continue;
            }
            uint32_t slot = got[off / 4];
            const Elf32_Sym &sy = syms[sidx];
            uint32_t expect;
            if (sy.st_shndx == SHN_UNDEF)      expect = api_lookup(str + sy.st_name);
            else if (sy.st_shndx == SHN_ABS)   expect = sy.st_value;
            else if (sy.st_shndx < eh->e_shnum && placed[sy.st_shndx])
                                               expect = base[sy.st_shndx] + sy.st_value;
            else continue;                     // a section the loader did not place
            if (slot != expect) {
                printf("       FAIL GOT: slot for %s holds %08x, the symbol is at %08x\n",
                       str + sy.st_name, slot, expect);
                bad = 1;
            }
        }
    }
    // No used slot may be zero. Every symbol resolves above the GOT (.data/.bss),
    // into the read-only half (.rodata / a local function) or to a firmware
    // address — never to 0, so a 0 is a slot the loader forgot to fill.
    for (uint32_t k = 0; k < app.got_count; k++)
        if (got[k] == 0) { printf("       FAIL GOT: slot %u was never filled\n", k); bad = 1; }

    if (sites && !bad)
        printf("       GOT ok: %d GOT_BREL site(s) through %u slot(s), every one resolves right\n",
               sites, app.got_count);
    free(b);
    return bad;
}

static int load_one(const char *path) {
    g_f = fopen(path, "rb");
    if (!g_f) { printf("  SKIP %s (not built)\n", path); return 0; }
    fseek(g_f, 0, SEEK_END); uint32_t sz = ftell(g_f);

    loader_set_allocator(alloc32, free32);
    AppSource src{}; src.read = file_read; src.ctx = nullptr; src.size = sz;
    LoadedApp app;
    LoadResult rc = app_load(src, &app);
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (rc != LOAD_OK) {
        printf("  FAIL %-12s %s%s%s\n", name, load_result_str(rc),
               app.detail[0] ? " - " : "", app.detail);
        fclose(g_f);
        return 1;
    }
    g_loaded++;
    printf("  ok   %-12s image %5u B   veneers %4u/%-4u   entry +0x%lx\n",
           name, app.image_size, app.veneers_used, app.veneer_size,
           (unsigned long)((uintptr_t)app.entry - (uintptr_t)app.image));

    // The entry point must land INSIDE the image. A relocation that put it
    // anywhere else would still "load" and then jump into nothing.
    uintptr_t e = (uintptr_t)app.entry & ~1u;
    uintptr_t b = (uintptr_t)app.image;
    int bad = 0;
    // Against the READ-ONLY half specifically: the entry point is code, and
    // image_size is a sum rather than a span now that the halves are separate
    // allocations. Checking it against b + image_size would pass an entry point
    // that had landed in the gap between two unrelated heap blocks.
    if (e < b || e >= b + app.text_size) {
        printf("       FAIL entry point is outside the executable half\n");
        bad = 1;
    }
    if ((uintptr_t)app.entry & 1u) { /* Thumb bit: required */ } else {
        printf("       FAIL entry point has no Thumb bit — it would fault on call\n");
        bad = 1;
    }
    if (app.veneers_used > app.veneer_size) {
        printf("       FAIL veneer pool overran\n");
        bad = 1;
    }

    // --- the two halves ------------------------------------------------------
    //
    // Code and data want opposite permissions, so they have to be in separate,
    // contiguous, separately-coverable spans. Every check here is about
    // something that would silently produce NO protection rather than a visible
    // failure: a block on the wrong boundary cannot be encoded as a region at
    // all, and a writable section that slipped into the read-only half would be
    // marked read-only and fault the first time the package assigned to a
    // global.
    if (b % APP_BLOCK_ALIGN) {
        printf("       FAIL image starts at %p, off the block boundary\n", app.image);
        bad = 1;
    }
    if ((uintptr_t)app.veneers % APP_BLOCK_ALIGN) {
        printf("       FAIL veneer pool is off the block boundary\n");
        bad = 1;
    }
    if (app.text_size % APP_BLOCK_ALIGN || app.data_size % APP_BLOCK_ALIGN) {
        printf("       FAIL a half is not a whole number of blocks (%u / %u)\n",
               app.text_size, app.data_size);
        bad = 1;
    }
    if (app.text_size + app.data_size != app.image_size) {
        printf("       FAIL image_size is not the two halves added up\n");
        bad = 1;
    }
    // THE HALVES ARE SEPARATE ALLOCATIONS and must not be assumed adjacent.
    // They used to be one block, and this check used to require it; the split
    // is what lets a 123 KB package load on a heap whose largest free piece is
    // smaller than that. What still has to hold is that the writable half is on
    // its own boundary and does not overlap the other.
    if (app.data && (uintptr_t)app.data % APP_BLOCK_ALIGN) {
        printf("       FAIL the writable half is off the block boundary\n");
        bad = 1;
    }
    if (app.data) {
        uintptr_t d = (uintptr_t)app.data;
        if (d < b + app.text_size && d + app.data_size > b) {
            printf("       FAIL the two halves overlap\n");
            bad = 1;
        }
    }
    if (!app.data && app.data_size) {
        printf("       FAIL a writable half with no address\n");
        bad = 1;
    }
    // The entry point is code, so it must be in the half that stays executable.
    if (e - b >= app.text_size) {
        printf("       FAIL entry point is in the writable half\n");
        bad = 1;
    }

    // Every region the OS will hand the hardware must actually be encodable.
    //
    // This is the join between the loader's layout and the protection
    // hardware's rules, and it fails silently in the direction that matters: an
    // unencodable region is refused, the region is left disabled, and the result
    // is a package running with no protection while everything reports success.
    //
    // A veneer is sixteen bytes and a region is described in thirty-twos, so a
    // package with an odd number of veneers produces a length the hardware
    // cannot express — which is exactly what happened, and only to some
    // packages, depending on how many distinct firmware functions they call.
    {
        MpuV8Region r;
        uint32_t vsize = mpu_align_up(app.veneers_used, MPU_V8_GRAIN);
        if (vsize > app.veneer_size) vsize = app.veneer_size;
        struct { const char *what; uintptr_t base; uint32_t size; MpuPerm perm; } rgn[] = {
            { "code",    (uintptr_t)app.image,   app.text_size, MPU_RO_EXEC },
            { "data",    (uintptr_t)app.data,    app.data_size, MPU_RW_NOEXEC },
            { "veneers", (uintptr_t)app.veneers, vsize,         MPU_RO_EXEC },
        };
        for (auto &g : rgn) {
            if (!g.size) continue;                    // legitimately absent
            if (!mpu_v8_encode((uint32_t)g.base, g.size, g.perm, &r)) {
                printf("       FAIL the %s region cannot be encoded "
                       "(base %08lx, %u bytes)\n",
                       g.what, (unsigned long)g.base, g.size);
                bad = 1;
            }
        }
        if (app.veneers_used && !vsize) {
            printf("       FAIL veneers were written but the region is empty\n");
            bad = 1;
        }
    }
    // And the decisive one: walk every byte of the image and check that nothing
    // writable ended up before text_end and nothing read-only after it.
    {
        uint32_t text_end = 0, misplaced = 0;
        for (uint32_t off = 0; off < app.image_size; off += 4) {
            uint32_t fl = section_flags_at(path, off, &text_end);
            if (!(fl & 0x80000000u)) continue;             // padding between sections
            bool writable = (fl & SHF_WRITE) != 0;
            if (writable != (off >= app.text_size)) misplaced++;
        }
        if (misplaced) {
            printf("       FAIL %u byte(s) are in the wrong half\n", misplaced * 4);
            bad = 1;
        }
        if (text_end != app.text_size) {
            printf("       FAIL the loader and this test disagree on where the "
                   "halves divide (%u vs %u)\n", app.text_size, text_end);
            bad = 1;
        }
    }

    // EVERY function pointer the package stores must be odd.
    //
    // ARM ELF already carries the Thumb bit in st_value, so a loader that adds
    // one itself produces function+2 with the bit CLEAR. Calling that faults
    // with INVSTATE the moment a registered command is invoked — which is
    // exactly what happened, and only to commands, because the entry point is
    // resolved by symbol lookup rather than by a relocation.
    //
    // Scanning the whole image for words that land inside it and look like code
    // catches the whole class: any ABS32 to a function, not just the ones a
    // hand-written test thought to check.
    // TWO RANGES, walked separately, because the halves are separate
    // allocations. Reading `image_size` words from `image` used to be the whole
    // image and is now the read-only block plus whatever the heap put after it:
    // the first run after the split reported a bad code pointer at +0xf10c,
    // which was not in the package at all.
    //
    // The offsets this reports and hands to in_text are ELF-LAYOUT offsets —
    // the writable half continues where the read-only half stopped — because
    // that is the space section_flags_at and in_text describe. Only the
    // addresses are in two pieces.
    const uintptr_t db = (uintptr_t)app.data;
    struct Half { const uint32_t *words; uint32_t bytes; uint32_t off_base; uintptr_t addr; };
    const Half halves[2] = {
        { (const uint32_t *)app.image, app.text_size, 0,              b  },
        { (const uint32_t *)app.data,  app.data_size, app.text_size,  db },
    };

    int checked = 0, even_ptrs = 0;
    for (const Half &h : halves) {
        if (!h.words || !h.bytes) continue;
        for (uint32_t w = 0; w < h.bytes / 4; w++) {
            uint32_t v = h.words[w];
            uint32_t a = v & ~1u;
            // Which half does it point into, if either?
            uint32_t off;
            if (a >= b && a < b + app.text_size)                 off = a - (uint32_t)b;
            else if (db && a >= db && a < db + app.data_size)    off = (a - (uint32_t)db) + app.text_size;
            else continue;                                        // not a pointer into us
            // Only judge values that point into an executable section. Data
            // pointers are legitimately even, so counting them would be noise.
            if (!in_text(path, off)) continue;
            checked++;
            if (!(v & 1u)) {
                printf("       FAIL word at +0x%x holds %08x -> code at +0x%x with NO Thumb bit\n",
                       h.off_base + w * 4, v, off);
                even_ptrs++;
            }
        }
    }
    if (checked) printf("       %d code pointer(s), %d missing the Thumb bit\n",
                        checked, even_ptrs);
    if (even_ptrs) bad = 1;

    // And every veneer's target word, which the scan above structurally cannot
    // see: veneers live outside the image, and they are how EVERY firmware call
    // is made. A veneer loads its target into pc, so an even target faults the
    // same way a bad function pointer does — just one level further out, in the
    // path taken by fw_print rather than by a registered command.
    uint32_t *vw = (uint32_t *)app.veneers;
    int vtargets = 0, veven = 0;
    for (uint32_t w = 0; w < app.veneers_used / 4; w++) {
        // Firmware lives in XIP flash. A word in that range inside a veneer is a
        // target address, not an instruction encoding.
        if (vw[w] < 0x10000000u || vw[w] >= 0x20000000u) continue;
        vtargets++;
        if (!(vw[w] & 1u)) {
            printf("       FAIL veneer target %08x has NO Thumb bit\n", vw[w]);
            veven++;
        }
    }
    if (vtargets) printf("       %d veneer target(s), %d missing the Thumb bit\n",
                         vtargets, veven);
    if (veven) bad = 1;

    // A position-independent package: verify the whole r9/GOT indirection.
    if (app.got_size) bad += check_got(path, app);

    fclose(g_f);
    return bad;
}

// --- the sandboxed form of the same app --------------------------------------
//
// Loaded a second time with the veneers built as supervisor calls, which is
// what a package gets when it runs unprivileged. Everything here is machine
// code the host cannot execute, so what is checked is the ENCODING — and the
// encoding is the part that fails without a word: a wrong literal offset makes
// the veneer load its own instructions as an index, and a wrong index makes the
// supervisor call a different function than the one the package asked for.
static int load_svc(const char *path) {
    g_f = fopen(path, "rb");
    if (!g_f) return 0;
    fseek(g_f, 0, SEEK_END); uint32_t sz = ftell(g_f);

    loader_set_veneer_mode(LOADER_VENEER_SVC);
    loader_set_allocator(alloc32, free32);
    AppSource src{}; src.read = file_read; src.ctx = nullptr; src.size = sz;
    LoadedApp app;
    LoadResult rc = app_load(src, &app);
    loader_set_veneer_mode(LOADER_VENEER_DIRECT);
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    fclose(g_f);

    if (rc != LOAD_OK) {
        printf("  FAIL %-12s sandboxed: %s%s%s\n", name, load_result_str(rc),
               app.detail[0] ? " - " : "", app.detail);
        return 1;
    }
    int bad = 0;
    const uint8_t *v = (const uint8_t *)app.veneers;
    const uint16_t *h = (const uint16_t *)v;

    // The two fixed gates come first and at known offsets, because the shim
    // that enters package code has to be able to name them.
    if (app.veneer_gates != 48) {
        printf("       FAIL sandboxed: the gates are %u bytes, not 48\n", app.veneer_gates);
        bad = 1;
    }
    // Both privilege gates take the new CONTROL value in r12 and NOTHING in
    // r0-r3. A function may return a value in any of those, and
    // __aeabi_uldivmod returns its remainder in r2 and r3 — so a gate that
    // used one as scratch would corrupt every 64-bit division a package does,
    // which is not a crash, it is wrong answers.
    const uint16_t *ret = (const uint16_t *)(v + 0);
    const uint16_t *ent = (const uint16_t *)(v + 16);
    const uint16_t *ext = (const uint16_t *)(v + 32);
    if (ret[0] != 0xf38c || ret[1] != 0x8814 ||
        ret[2] != 0xf3bf || ret[3] != 0x8f6f || ret[4] != 0x4770) {
        printf("       FAIL sandboxed: the return gate is not "
               "msr CONTROL,r12 / isb / bx lr\n");
        bad = 1;
    }
    if (ent[0] != 0xf38c || ent[1] != 0x8814 ||
        ent[2] != 0xf3bf || ent[3] != 0x8f6f || ent[4] != 0x4718) {
        printf("       FAIL sandboxed: the enter gate is not "
               "msr CONTROL,r12 / isb / bx r3\n");
        bad = 1;
    }
    if (ext[0] != 0xdf01) {
        printf("       FAIL sandboxed: the exit gate is not svc #1\n");
        bad = 1;
    }
    // The two must not be the same gate. Entering has to leave LR holding
    // somewhere for app_main to return to, so it cannot also carry the
    // destination there.
    if (ret[4] == ent[4]) {
        printf("       FAIL sandboxed: both gates end the same way\n");
        bad = 1;
    }
    // The gates carry a literal that is not a usable index, so that even a
    // reuse scan starting in the wrong place cannot hand one out as the veneer
    // for a real function.
    if (*(const uint32_t *)(v + 12) != 0xFFFFFFFFu ||
        *(const uint32_t *)(v + 16 + 12) != 0xFFFFFFFFu ||
        *(const uint32_t *)(v + 32 + 12) != 0xFFFFFFFFu) {
        printf("       FAIL sandboxed: a gate carries a literal that could pass "
               "for an ABI index\n");
        bad = 1;
    }
    if (app_return_gate(&app) != ((uint32_t)(uintptr_t)v | 1u) ||
        app_enter_gate(&app)  != (((uint32_t)(uintptr_t)v + 16) | 1u) ||
        app_exit_gate(&app)   != (((uint32_t)(uintptr_t)v + 32) | 1u)) {
        printf("       FAIL sandboxed: the gate addresses do not match where they were written\n");
        bad = 1;
    }

    // Every call veneer. The literal must hold an INDEX the firmware exports,
    // not an address — mixing the two is the mistake that would look like the
    // package calling a wild function, and only sometimes.
    int calls = 0;
    for (uint32_t off = app.veneer_gates; off < app.veneers_used; off += 16) {
        const uint16_t *c = (const uint16_t *)(v + off);
        uint32_t lit = *(const uint32_t *)(v + off + 12);
        calls++;
        if (c[0] != 0xf8df || c[1] != 0xc008) {
            printf("       FAIL sandboxed veneer at +%u does not load from +12\n", off);
            bad = 1;
        }
        if (c[2] != 0xdf00) {
            printf("       FAIL sandboxed veneer at +%u is not svc #0\n", off);
            bad = 1;
        }
        // An index, and one that resolves. api_addr_at is the bounds check the
        // supervisor will apply, so asking it here is asking the same question.
        if (!api_addr_at(lit)) {
            printf("       FAIL sandboxed veneer at +%u holds %08x, which is not "
                   "an index the firmware exports\n", off, lit);
            bad = 1;
        }
        // The giveaway that an address slipped in where an index belongs.
        if (lit >= 0x10000000u) {
            printf("       FAIL sandboxed veneer at +%u holds an ADDRESS, not an index\n", off);
            bad = 1;
        }
    }
    if (!calls) {
        printf("       FAIL sandboxed: no call veneers at all — nothing was checked\n");
        bad = 1;
    }
    printf("  ok   %-12s sandboxed: %d gate(s) + %d supervisor call(s)\n",
           name, 3, calls);
    return bad;
}

// --- the flash-slot form: install then load (task #93 stage 3) ---------------
//
// The decisive stage-3 check, and the reason the whole thing is safe. A PIC
// package's read-only half is meant to be assemble-once, place-anywhere so it can
// live in a flash slot and run in place. app_pic_install produces that blob and a
// manifest; app_pic_load instantiates only the writable half. This does three
// things the copy-to-RAM check cannot:
//
//   1. POSITION-INDEPENDENCE. Install the same package into two buffers at two
//      addresses and compare the blobs byte for byte. Identical or it is not
//      placeable — and this is the check that catches an absolute address that
//      checkapp's .text-only scan would miss in .rodata.
//   2. RESOLUTION, against an INDEPENDENT layout. For every GOT_BREL site the
//      offset the loader baked is followed to its slot and checked to hold the
//      symbol's real runtime address, computed here from the ELF and the slot/RAM
//      bases rather than from the loader's own answer. Same for every ABS32 in
//      .data.
//   3. LOAD TWICE. Two independent RAM blocks from one blob, both resolving —
//      which proves the slot is read-only at load and catches an ABS32 applied
//      twice (init + 2S instead of init + S), the bug that reads as heap rot.
//
// And the footprint the task turns on, asserted rather than printed: the RAM the
// load costs must clear the 89 KB largest-free-block a booted board reports.

struct SlotBuf { uint8_t *buf; uint32_t cap; uint32_t max_off; };
static bool slot_sink(void *ctx, uint32_t off, const void *data, uint32_t len) {
    SlotBuf *s = (SlotBuf *)ctx;
    if ((uint64_t)off + len > s->cap) return false;
    memcpy(s->buf + off, data, len);
    if (off + len > s->max_off) s->max_off = off + len;
    return true;
}

// A package is position-independent iff it reaches its data through a GOT — i.e.
// it carries at least one R_ARM_GOT_BREL. That is exactly what makes the slot
// path apply to it, so it is how the test decides whether to run at all.
static bool is_pic(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(sz);
    bool got = false;
    if (b && fread(b, 1, sz, f) == (size_t)sz) {
        Elf32_Ehdr *eh = (Elf32_Ehdr *)b;
        Elf32_Shdr *sh = (Elf32_Shdr *)(b + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum && !got; i++) {
            if (sh[i].sh_type != SHT_REL) continue;
            Elf32_Rel *rel = (Elf32_Rel *)(b + sh[i].sh_offset);
            uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel);
            for (uint32_t r = 0; r < n; r++)
                if (ELF32_R_TYPE(rel[r].r_info) == R_ARM_GOT_BREL) { got = true; break; }
        }
    }
    free(b); fclose(f);
    return got;
}

static int check_pic(const char *path) {
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;

    // Fresh arena: the slots and the two RAM blocks all come from it, so it must
    // start empty rather than with whatever the copy-path checks left behind.
    g_arena = nullptr; g_used_bytes = 0; g_arena_cap = 0;
    loader_set_allocator(alloc32, free32);

    // The ELF in RAM, for the INDEPENDENT layout below — mirrored, never shared
    // with the loader, so the two can disagree.
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  SKIP %s (not built)\n", name); return 0; }
    fseek(f, 0, SEEK_END); uint32_t fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(fsz);
    if (!b || fread(b, 1, fsz, f) != fsz) { fclose(f); free(b); return 0; }
    fclose(f);
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b;
    Elf32_Shdr *sh = (Elf32_Shdr *)(b + eh->e_shoff);
    int symtab_i = -1;
    for (int i = 0; i < eh->e_shnum; i++) if (sh[i].sh_type == SHT_SYMTAB) symtab_i = i;
    if (symtab_i < 0) { free(b); return 0; }
    Elf32_Sym *syms = (Elf32_Sym *)(b + sh[symtab_i].sh_offset);
    const char *str = (const char *)(b + sh[sh[symtab_i].sh_link].sh_offset);

    // Mirror app_pic_install's layout: GOT bytes (distinct GOT_BREL symbol index),
    // RO sections contiguous from 0 (blob), writable from the GOT base (RAM).
    static uint32_t blob_off[LOADER_MAX_SECTIONS], ram_off[LOADER_MAX_SECTIONS];
    static bool placed[LOADER_MAX_SECTIONS], writ[LOADER_MAX_SECTIONS];
    for (int i = 0; i < eh->e_shnum; i++) { blob_off[i] = ram_off[i] = 0; placed[i] = writ[i] = false; }
    uint32_t nsyms = sh[symtab_i].sh_size / sizeof(Elf32_Sym);
    uint8_t *seen = (uint8_t *)calloc((nsyms + 7) / 8, 1);
    uint32_t slots = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_REL) continue;
        Elf32_Rel *rel = (Elf32_Rel *)(b + sh[i].sh_offset);
        uint32_t n = sh[i].sh_size / sizeof(Elf32_Rel);
        for (uint32_t r = 0; r < n; r++) {
            if (ELF32_R_TYPE(rel[r].r_info) != R_ARM_GOT_BREL) continue;
            uint32_t s = ELF32_R_SYM(rel[r].r_info);
            if (s < nsyms && !(seen[s >> 3] & (1u << (s & 7)))) { seen[s >> 3] |= (1u << (s & 7)); slots++; }
        }
    }
    free(seen);
    uint32_t got_bytes = (slots * 4u + 3u) & ~3u;
    uint32_t bpos = 0, rpos = got_bytes;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0 || (sh[i].sh_flags & SHF_WRITE)) continue;
        uint32_t al = sh[i].sh_addralign ? sh[i].sh_addralign : 4; if (al < 4) al = 4;
        bpos = (bpos + al - 1) & ~(al - 1); blob_off[i] = bpos; placed[i] = true; bpos += sh[i].sh_size;
    }
    for (int i = 0; i < eh->e_shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC) || sh[i].sh_size == 0 || !(sh[i].sh_flags & SHF_WRITE)) continue;
        uint32_t al = sh[i].sh_addralign ? sh[i].sh_addralign : 4; if (al < 4) al = 4;
        rpos = (rpos + al - 1) & ~(al - 1); ram_off[i] = rpos; placed[i] = writ[i] = true; rpos += sh[i].sh_size;
    }

    int bad = 0;
    const uint32_t SLOTCAP = 256u * 1024u;

    // 1) install twice, at two different arena addresses.
    uint8_t *slotA = (uint8_t *)alloc32(SLOTCAP);
    uint8_t *slotB = (uint8_t *)alloc32(SLOTCAP);
    if (!slotA || !slotB) { printf("  SKIP %-12s (arena too small)\n", name); free(b); return 0; }
    SlotBuf sbA{slotA, SLOTCAP, 0}, sbB{slotB, SLOTCAP, 0};
    PicManifest mA{}, mB{};
    AppSource src{}; src.read = file_read; src.ctx = nullptr; src.size = fsz;

    g_f = fopen(path, "rb");
    LoadResult rcA = app_pic_install(src, slot_sink, &sbA, &mA);
    fclose(g_f);
    if (rcA != LOAD_OK) {
        printf("  FAIL %-12s pic-install: %s\n", name, load_result_str(rcA));
        free(b); return 1;
    }
    g_f = fopen(path, "rb");
    LoadResult rcB = app_pic_install(src, slot_sink, &sbB, &mB);
    fclose(g_f);
    if (rcB != LOAD_OK) { printf("  FAIL %-12s pic-install(B): %s\n", name, load_result_str(rcB)); free(b); app_pic_manifest_free(&mA); return 1; }

    g_loaded++;
    printf("  ok   %-12s slot %u B (text %u + rodata %u + veneers %u), RAM %u B\n",
           name, mA.ro_size, mA.text_size, mA.rodata_size, mA.veneer_size, mA.ram_size);

    // The property that makes a flash slot possible at all.
    if (slotA == slotB || mA.ro_size != mB.ro_size || memcmp(slotA, slotB, mA.ro_size) != 0) {
        printf("       FAIL not position-independent: the two blobs differ\n"); bad = 1;
    } else {
        printf("       position-independent: identical blob at two addresses\n");
    }
    app_pic_manifest_free(&mB);

    // 2) the footprint the task turns on — the ONE resident allocation, asserted.
    if (mA.ram_size >= 89u * 1024u) {
        printf("       FAIL RAM block %u B does not clear the 89 KB largest free block\n", mA.ram_size);
        bad = 1;
    } else {
        printf("       RAM block %u B clears 89 KB (was the ~114 KB read-only half)\n", mA.ram_size);
    }
    // The entry point must land in the executable half of the blob.
    if (!(mA.entry_off < mA.text_size))
        { printf("       FAIL entry +0x%x is not in .text (0..%u)\n", mA.entry_off, mA.text_size); bad = 1; }

    // 3) load TWICE from the one blob, and verify each against the independent
    // layout. Two RAM blocks, so an ABS32 to RAM resolves to a different value in
    // each and an ABS32 applied twice cannot hide behind a shared address.
    LoadedApp appA{}, appB{};
    if (app_pic_load(slotA, &mA, &appA) != LOAD_OK) { printf("       FAIL pic-load A\n"); free(b); app_pic_manifest_free(&mA); return bad + 1; }
    if (app_pic_load(slotA, &mA, &appB) != LOAD_OK) { printf("       FAIL pic-load B\n"); app_unload(&appA); free(b); app_pic_manifest_free(&mA); return bad + 1; }

    // Resolve a symbol to its runtime address the way the CPU would see it, from
    // the ELF alone: firmware -> the blob veneer holding its ABI index; a defined
    // symbol -> the slot (RO) or the RAM block (writable) at its section offset.
    auto resolve = [&](const LoadedApp &app, uint32_t sidx, bool *ok) -> uint32_t {
        *ok = true;
        const Elf32_Sym &s = syms[sidx];
        bool func = (ELF32_ST_TYPE(s.st_info) == STT_FUNC);
        uint32_t slotbase = (uint32_t)(uintptr_t)slotA;
        if (s.st_shndx == SHN_UNDEF) {
            int ix = api_index_of(str + s.st_name);
            for (uint32_t off = mA.veneer_off; off + 16 <= mA.veneer_off + mA.veneer_size; off += 16)
                if (*(uint32_t *)(slotA + off + 12) == (uint32_t)ix) return (slotbase + off) | 1u;
            *ok = false; return 0;
        }
        if (s.st_shndx == SHN_ABS) return s.st_value;
        if (s.st_shndx >= (uint32_t)eh->e_shnum || !placed[s.st_shndx]) { *ok = false; return 0; }
        uint32_t a = writ[s.st_shndx] ? (uint32_t)(uintptr_t)app.data + ram_off[s.st_shndx] + s.st_value
                                      : slotbase + blob_off[s.st_shndx] + s.st_value;
        return a | (func ? 1u : 0u);
    };

    auto verify = [&](const LoadedApp &app, const char *tag) {
        const uint32_t *got = (const uint32_t *)app.data;
        int got_sites = 0, abs_sites = 0;
        for (int si = 0; si < eh->e_shnum; si++) {
            if (sh[si].sh_type != SHT_REL) continue;
            uint32_t tgt = sh[si].sh_info;
            if (tgt >= (uint32_t)eh->e_shnum || !placed[tgt]) continue;
            Elf32_Rel *rel = (Elf32_Rel *)(b + sh[si].sh_offset);
            uint32_t n = sh[si].sh_size / sizeof(Elf32_Rel);
            for (uint32_t r = 0; r < n; r++) {
                uint32_t type = ELF32_R_TYPE(rel[r].r_info);
                uint32_t sidx = ELF32_R_SYM(rel[r].r_info);
                if (type == R_ARM_GOT_BREL) {
                    got_sites++;
                    // the offset the loader baked into .text, followed to its slot
                    uint32_t off = *(uint32_t *)(slotA + blob_off[tgt] + rel[r].r_offset);
                    if (off % 4 || off >= mA.got_bytes) { printf("       FAIL %s GOT offset %u out of range\n", tag, off); bad = 1; continue; }
                    bool ok; uint32_t want = resolve(app, sidx, &ok);
                    if (!ok) { printf("       FAIL %s cannot resolve %s\n", tag, str + syms[sidx].st_name); bad = 1; continue; }
                    if (got[off / 4] != want) {
                        printf("       FAIL %s GOT[%u] = %08x, %s is at %08x\n",
                               tag, off / 4, got[off / 4], str + syms[sidx].st_name, want); bad = 1;
                    }
                } else if (type == R_ARM_ABS32 || type == R_ARM_TARGET1) {
                    abs_sites++;
                    if (!writ[tgt]) continue;      // handled as a branch/GOT in RO
                    bool ok; uint32_t S = resolve(app, sidx, &ok);
                    if (!ok) { printf("       FAIL %s cannot resolve ABS %s\n", tag, str + syms[sidx].st_name); bad = 1; continue; }
                    // expected = the ORIGINAL addend (from the ELF's .data) + S,
                    // applied exactly once.
                    uint32_t addend = *(uint32_t *)(b + sh[tgt].sh_offset + rel[r].r_offset);
                    uint32_t site = ram_off[tgt] + rel[r].r_offset;      // within the RAM block
                    uint32_t have = *(uint32_t *)((uint8_t *)app.data + site);
                    if (have != addend + S) {
                        printf("       FAIL %s ABS32 at +%u = %08x, expected %08x (addend %08x + S %08x)\n",
                               tag, site, have, addend + S, addend, S); bad = 1;
                    }
                }
            }
        }
        // No used GOT slot may be zero — every symbol resolves somewhere real.
        for (uint32_t k = 0; k < app.got_count; k++)
            if (got[k] == 0) { printf("       FAIL %s GOT[%u] never filled\n", tag, k); bad = 1; }
        if (!bad) printf("       %s: %d GOT + %d ABS32 site(s) resolve against the slot at %p\n",
                         tag, got_sites, abs_sites, (void *)app.data);
    };
    verify(appA, "load A");
    verify(appB, "load B");

    // 4) a slot built for another ABI is refused, not run — the one failure that
    // would otherwise not fault, because a stale index calls the wrong function.
    PicManifest badver = mA; badver.api_major = (uint16_t)(mA.api_major + 1);
    LoadedApp appX{};
    if (app_pic_load(slotA, &badver, &appX) != LOAD_ERR_API_MISMATCH) {
        printf("       FAIL a slot from a newer ABI was not refused\n"); bad = 1;
        app_unload(&appX);
    }

    app_unload(&appA);
    app_unload(&appB);
    app_pic_manifest_free(&mA);
    free(b);
    g_arena = nullptr; g_used_bytes = 0;
    return bad;
}

// Where build.sh actually puts the apps. It builds per BOARD, so there is no
// single "build" directory — and there WAS a stale one left from an older
// layout that this test happily read for hours while reporting success. The
// first directory that exists wins; they hold identical apps, since every
// package is built for ARMv6-M so one binary serves both architectures.
static const char *kBuildDirs[] = {
    "../build_pico2_w/apps", "../build_pico_w/apps",
    "../build_pico2/apps",   "../build_pico/apps",
    "../build/apps",
};
// httpd is the largest SINGLE-file package and novad1 the first MULTI-file one —
// several objects folded together with `ld -r`. That produces a different
// section layout from anything the compiler emits directly, and the loader has
// to handle it identically or a package that builds fine will not load.
static const char *kNames[] = { "greet", "bench", "stress", "tuidemo", "httpd", "novad1" };

int main(int argc, char **argv) {
    static char paths[8][64];
    static const char *kApps[8];
    int napps = 0;

    const char *dir = nullptr;
    for (const char *d : kBuildDirs) {
        char probe[80];
        snprintf(probe, sizeof(probe), "%s/greet.app", d);
        FILE *f = fopen(probe, "rb");
        if (f) { fclose(f); dir = d; break; }
    }
    if (dir) {
        printf("  from %s\n", dir);
        for (const char *n : kNames) {
            snprintf(paths[napps], sizeof(paths[0]), "%s/%s.app", dir, n);
            kApps[napps] = paths[napps];
            napps++;
        }
    }
    int fails = 0;

    // --- two copies at once, on a heap that only fits one --------------------
    //
    // This is the failure that stopped a device installing its own update.
    // pkg_install_file used to LOAD the new image to read its name, and only
    // then unload the copy already running — so an upgrade needed two images
    // resident at the same moment. With 96 KB free and a 53 KB image, the second
    // load returned LOAD_ERR_OOM and the message was "out of memory" from a step
    // nobody expected to allocate.
    //
    // The check is the shape of the fix rather than the exact numbers: a heap
    // sized for ONE image must load one, refuse the second, and — the part that
    // matters — load the second successfully once the first is unloaded.
    if (dir && argc == 1) {
        char path[80];
        snprintf(path, sizeof(path), "%s/novad1.app", dir);
        FILE *probe_f = fopen(path, "rb");
        if (probe_f) {
            fclose(probe_f);
            printf("  two-at-once on a one-image heap\n");

            // Measure what one costs, then cap the arena just under two.
            g_arena = nullptr; g_used_bytes = 0;
            LoadedApp a{};
            g_f = fopen(path, "rb");
            AppSource s1{}; s1.read = file_read;
            loader_set_allocator(alloc32, free32);
            if (app_load(s1, &a) == LOAD_OK) {
                size_t one = g_used_bytes;
                app_unload(&a);
                fclose(g_f);

                // A header read must cost almost nothing — that is the whole
                // reason it can happen before making room.
                g_arena = nullptr; g_used_bytes = 0;
                g_f = fopen(path, "rb");
                RpcAppHeader hdr{};
                bool peeked = app_peek(s1, &hdr) == LOAD_OK;
                fclose(g_f);
                printf("     one image %zu B, header read alone %zu B\n", one, g_used_bytes);
                if (!peeked || hdr.magic != RPC_APP_MAGIC) {
                    printf("     FAIL app_peek did not read the header\n");
                    fails++;
                } else if (g_used_bytes > one / 8) {
                    printf("     FAIL app_peek allocated %zu B, which is not 'almost nothing'\n",
                           g_used_bytes);
                    fails++;
                }

                // Now the property itself: TWO images do not fit a heap sized
                // for one and a half. That is the whole reason the install
                // order had to change — validating before unloading needed
                // exactly this and could not have it.
                //
                // The third step, "unload then load succeeds", cannot be
                // expressed here: this allocator is a bump pointer and free32
                // is a no-op, so nothing is ever given back. What IS checkable
                // is that the loader reports releasing what it took, which is
                // the same claim from the other side.
                g_arena = nullptr; g_used_bytes = 0;
                g_arena_cap = one + one / 2;          // room for one, not two
                LoadedApp first{}, second{};
                g_f = fopen(path, "rb");
                bool got_first = app_load(s1, &first) == LOAD_OK;
                size_t after_first = g_used_bytes;
                LoadResult r2 = app_load(s1, &second);
                fclose(g_f);
                if (!got_first) { printf("     FAIL the first load did not fit\n"); fails++; }
                if (r2 == LOAD_OK) {
                    printf("     FAIL two images fitted a one-image heap\n");
                    fails++;
                    app_unload(&second);
                } else {
                    printf("     ok   two at once is refused (%zu B used of %zu)\n",
                           after_first, g_arena_cap);
                }
                if (got_first) {
                    // Everything it took, given back — image, veneers and the
                    // alignment slack on both. `unload` reporting a shortfall is
                    // how a leak in the loader would show up at all.
                    // `one` was measured from a bump pointer that starts at 8
                    // deliberately off-boundary, so the comparison allows for
                    // that head start rather than reporting it as a leak.
                    if (first.bytes_allocated + 8 < one) {
                        printf("     FAIL unload accounts for %u B of %zu taken\n",
                               (unsigned)first.bytes_allocated, one);
                        fails++;
                    } else {
                        printf("     ok   unload accounts for every byte it took\n");
                    }
                    app_unload(&first);
                }
                g_arena_cap = 0;
            } else {
                fclose(g_f);
            }
            g_arena = nullptr; g_used_bytes = 0;
        }
    }

    if (argc > 1) {
        for (int i = 1; i < argc; i++) fails += load_one(argv[i]);
        for (int i = 1; i < argc; i++) if (is_pic(argv[i])) fails += check_pic(argv[i]);
    } else {
        for (int i = 0; i < napps; i++) fails += load_one(kApps[i]);
        // And every one of them again as a sandboxed package, which produces a
        // completely different veneer pool from the same file.
        for (int i = 0; i < napps; i++) fails += load_svc(kApps[i]);
        // And every PIC package a third way: installed to a flash slot and loaded
        // from it, which is the whole of task #93 stage 3.
        for (int i = 0; i < napps; i++) if (is_pic(kApps[i])) fails += check_pic(kApps[i]);
    }
    // Loading nothing is a failure, not a pass. These paths are relative to this
    // directory, so running from anywhere else — or a change to where the build
    // puts its apps — would skip every check and still report success. That is
    // the precise shape of the bug this file exists to catch, and it would be
    // absurd for the test to have it too.
    if (g_loaded == 0) {
        printf("  FAIL loaded no apps at all — build them first, and run this "
               "from os/host\n");
        fails++;
    }
    printf("  realapp: %d loaded, %d failed\n", g_loaded, fails);
    return fails ? 1 : 0;
}
