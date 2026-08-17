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
#include "pkgslot.h"
// For TaskAppMem — the shape describe() fills, which the shadow map below is
// compared against. See check_shadow_matches_describe and host_describe.cpp.
#include "task.h"
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
// How many went through the SLOT path, which is a different question. A package
// reaches it only by being position-independent, so the count is not `g_loaded`
// and it is not a constant: it is however many packages os/CMakeLists.txt has
// opted into PIC. If that ever reaches zero, every slot-path check in this file
// stops running and nothing else here would notice — which is precisely how
// #103 survived sixty-one green suites.
static int g_pic_checked;
// MOVW/MOVT pairs actually read back, split by where they came from. GCC at -Os
// materialises addresses through literal pools, so the real packages currently
// hold NONE — which would make the check that reads them back pure decoration
// unless the number is on the screen and something else guarantees the path is
// exercised. That something is check_movw_fixture.
static int g_movw_real;
static int g_movw_fixture;
// Branch relocations judged by where they LAND, on the two copy paths. The slot
// path has always decoded these; the copy path checked nothing but the parity of
// a veneer's target word, which says nothing about whether any BL goes there.
static int g_branches;
// How much the flash-slot path was really exercised, and by the biggest package
// that went through it. The count of PIC packages says only that the path RAN;
// it cannot say the run was worth anything. TESTING.md's own words about the
// state before this: "the whole slot path is proved by one 304-byte package...
// the suite fails if that count ever drops to zero; it cannot tell that one is
// thin cover for the path #103 hid in."
static int      g_slot_got, g_slot_branch, g_slot_abs;
static uint32_t g_slot_biggest_blob;
// What the loader READ, as well as what it allocated. The page-at-a-time
// installer buys its small footprint by re-reading the relocation table once per
// page, and "that is cheap" is a claim rather than a fact until the number is on
// the screen next to the file size.
static uint64_t g_read_bytes;
static uint32_t g_read_calls;
static int file_read(void *, uint32_t off, void *dst, uint32_t len) {
    if (fseek(g_f, off, SEEK_SET) != 0) return -1;
    size_t n = fread(dst, 1, len, g_f);
    g_read_bytes += n; g_read_calls++;
    return (int)n;
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

// What to say when an artifact this suite needs is not on disk.
//
// It is a FAILURE and not a skip, and that distinction is the whole of task
// #101. A missing .app used to print SKIP and return 0, so a tree that had
// never run build.sh — or one holding a partial build — checked whatever
// happened to be there and reported a pass. The suite then looked flaky: green
// in a tree that had built, meaningless in one that had not, with nothing in
// the output saying which of those it was.
//
// Naming the file and naming the command is the point. Everything this test
// asserts is about the join between what the compiler produced and what the
// hardware will take, and it cannot assert any of it against a file that is
// not there.
static int missing_artifact(const char *path) {
    printf("  FAIL %s is not there.\n"
           "       This suite loads the packages build.sh produces; there is\n"
           "       nothing to load. Run ./build.sh from the repository root\n"
           "       (or ./build.sh pico2_w for one board) and try again.\n", path);
    return 1;
}

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
static bool g_section_read_failed;
static uint32_t section_flags_at(const char *path, uint32_t want_off,
                                 uint32_t *text_end_out) {
    // A failure to read is NOT "no section covers this offset". Both answers are
    // zero, and the caller treats zero as padding — so an unreadable file would
    // have every offset skipped and the whole placement check would pass without
    // examining a byte. It is recorded rather than returned, because the caller
    // asks this once per word and wants one complaint, not thousands.
    FILE *f = fopen(path, "rb");
    if (!f) { g_section_read_failed = true; return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(sz > 0 ? sz : 1);
    if (!b || sz <= 0 || fread(b, 1, sz, f) != (size_t)sz) {
        fclose(f); free(b); g_section_read_failed = true; return 0;
    }
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

// --- the loader's placement, recomputed from the ELF -------------------------
//
// Every check that has to know where a symbol ENDED UP needs this, and it
// MIRRORS app_load rather than sharing its code: a helper that called the
// loader's own placement would agree with it whatever either of them did. It
// has to be kept in step by hand, which is the price of being able to disagree.
//
// Two passes, non-writable first, with the GOT reserved at the base of the
// writable half exactly as app_load does.
struct ElfMirror {
    uint8_t     *b;
    Elf32_Ehdr  *eh;
    Elf32_Shdr  *sh;
    Elf32_Sym   *syms;
    const char  *str;
    uint32_t     nsyms;
    uint32_t     addr[LOADER_MAX_SECTIONS];   // runtime address of each section
    bool         placed[LOADER_MAX_SECTIONS];
    bool         writ[LOADER_MAX_SECTIONS];
    uint32_t     text_end;                    // where the halves divide
};

// The layout half, over bytes already in memory. Split out so a synthetic
// object — one built by this file rather than read off disk — goes through the
// same mirror as a real package and is judged by exactly the same code.
// Takes ownership of `bytes`, which must have come from malloc.
static bool mirror_from_bytes(uint8_t *bytes, long sz, const LoadedApp &app,
                              ElfMirror *m) {
    memset(m, 0, sizeof(*m));
    if (!bytes || sz <= 0) { free(bytes); return false; }
    m->b = bytes;
    m->eh = (Elf32_Ehdr *)m->b;
    m->sh = (Elf32_Shdr *)(m->b + m->eh->e_shoff);
    if (m->eh->e_shnum > LOADER_MAX_SECTIONS) { free(m->b); m->b = nullptr; return false; }

    uint32_t total = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < m->eh->e_shnum; i++) {
            if (!(m->sh[i].sh_flags & SHF_ALLOC) || m->sh[i].sh_size == 0) continue;
            if (((m->sh[i].sh_flags & SHF_WRITE) != 0) != (pass == 1)) continue;
            uint32_t al = m->sh[i].sh_addralign ? m->sh[i].sh_addralign : 4;
            if (al < 4) al = 4;
            total = (total + al - 1) & ~(al - 1);
            m->addr[i] = total;
            m->placed[i] = true;
            m->writ[i] = (pass == 1);
            total += m->sh[i].sh_size;
        }
        if (pass == 0) {
            total = (total + (APP_BLOCK_ALIGN - 1)) & ~(APP_BLOCK_ALIGN - 1);
            m->text_end = total;
            total += app.got_size;                 // the GOT the loader reserved
        }
    }
    // Offsets become addresses: the two halves are separate allocations.
    for (int i = 0; i < m->eh->e_shnum; i++) {
        if (!m->placed[i]) continue;
        m->addr[i] = m->writ[i]
                   ? (uint32_t)(uintptr_t)app.data + (m->addr[i] - m->text_end)
                   : (uint32_t)(uintptr_t)app.image + m->addr[i];
    }

    int symtab_i = -1;
    for (int i = 0; i < m->eh->e_shnum; i++)
        if (m->sh[i].sh_type == SHT_SYMTAB) symtab_i = i;
    if (symtab_i < 0) { free(m->b); m->b = nullptr; return false; }
    m->syms  = (Elf32_Sym *)(m->b + m->sh[symtab_i].sh_offset);
    m->str   = (const char *)(m->b + m->sh[m->sh[symtab_i].sh_link].sh_offset);
    m->nsyms = m->sh[symtab_i].sh_size / sizeof(Elf32_Sym);
    return true;
}

static bool mirror_open(const char *path, const LoadedApp &app, ElfMirror *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *bytes = (uint8_t *)malloc(sz > 0 ? sz : 1);
    if (!bytes || sz <= 0 || fread(bytes, 1, sz, f) != (size_t)sz) {
        fclose(f); free(bytes); return false;
    }
    fclose(f);
    return mirror_from_bytes(bytes, sz, app, m);
}
static void mirror_close(ElfMirror *m) { free(m->b); m->b = nullptr; }

// --- the permission map the device will actually program ---------------------
//
// describe() in os/shell/apps.cpp turns a LoadedApp into a TaskAppMem, and
// task_app_mem_apply hands each field to set_region. Three of the five spans
// come out of the load: the read-only half, which is executable; the writable
// half, which never is; and the veneer pool, which is. (The other two, a
// sandbox's stack and arena, are allocated per call and are not the loader's
// to get wrong.)
//
// MIRRORED here rather than called, for the reason every mirror in this file
// exists: a helper that called describe() would agree with it whatever either
// of them did. The arithmetic is copied deliberately, so a change to one and
// not the other shows up as a failure here rather than as a fault on a board.
//
// THE ONE THING THAT MAKES THIS A MODEL AND NOT A LIST: a span that
// mpu_v8_encode refuses is marked NOT PROGRAMMED, because that is exactly what
// the hardware ends up with. set_region clears the limit register, tries to
// encode, and on failure writes a zero base and returns — leaving the region
// disabled. Unprivileged code has no default map to fall back on, so a span
// that was refused is not a span with weaker permissions, it is a span with
// none: no read, and no instruction fetch. That is the whole of #103, and a
// map that recorded the INTENDED permission would model the bug away.
struct ShadowSpan {
    const char *what;
    uint32_t    base, size;
    MpuPerm     perm;
    bool        exec, write;
    bool        programmed;      // what the hardware will really be left holding
};
struct Shadow { ShadowSpan s[3]; int n; };

static Shadow shadow_of(const LoadedApp &app) {
    // Only the part of the pool actually written, rounded up to a whole block —
    // and clamped, because the rounding must not reach past the allocation.
    // Same two lines as describe().
    uint32_t vsize = mpu_align_up(app.veneers_used, MPU_V8_GRAIN);
    if (vsize > app.veneer_size) vsize = app.veneer_size;

    Shadow sm{};
    struct { const char *what; uint32_t base, size; MpuPerm perm; } want[] = {
        { "code",    (uint32_t)(uintptr_t)app.image,   app.text_size, MPU_RO_EXEC   },
        { "data",    (uint32_t)(uintptr_t)app.data,    app.data_size, MPU_RW_NOEXEC },
        { "veneers", (uint32_t)(uintptr_t)app.veneers, vsize,         MPU_RO_EXEC   },
    };
    for (auto &w : want) {
        ShadowSpan &s = sm.s[sm.n++];
        s.what  = w.what;
        s.base  = w.base;
        s.size  = w.size;
        s.perm  = w.perm;
        s.exec  = (w.perm == MPU_RO_EXEC);
        s.write = (w.perm == MPU_RW_NOEXEC);
        MpuV8Region r;
        s.programmed = w.base && w.size && mpu_v8_encode(w.base, w.size, w.perm, &r);
    }
    return sm;
}

// --- and the same question asked of the real describe() ----------------------
//
// The map above is a COPY, on purpose, and that is worth keeping: a helper that
// called the loader's own placement would agree with it whatever either of them
// did. What a copy cannot do is notice the thing it is a copy OF changing. Edit
// describe() to hand out a different span and every region check in this file
// goes on checking the old one, in silence, exactly as it did before #103.
//
// So the two are compared. host_describe.cpp includes os/shell/apps.cpp and
// hands back the real describe(), and the three spans it produces have to match
// the three this file computed — base and size, per span. Everything else about
// a ShadowSpan (the permission, and whether the hardware would accept it) stays
// this file's own independent answer, so both tripwires exist: the loader moving
// under the mirror, and describe() moving under the loader.
//
// What this does NOT cover: the PERMISSIONS. A TaskAppMem carries spans and
// nothing else — MPU_RO_EXEC and MPU_RW_NOEXEC are chosen in task_app_mem_apply
// in os/mpu_rp2.cpp, which has hardware headers and cannot be compiled here at
// all. Nothing in this suite would notice one of those three being changed to
// the other. That is on TESTING.md's honest list rather than papered over.
void host_app_describe(const LoadedApp *a, TaskAppMem *m);

static int check_shadow_matches_describe(const LoadedApp &app, const Shadow &sm,
                                         const char *how) {
    TaskAppMem m;
    host_app_describe(&app, &m);
    struct { const char *what; uint32_t base, size; } real[3] = {
        { "code",    (uint32_t)(uintptr_t)m.text,   m.text_size },
        { "data",    (uint32_t)(uintptr_t)m.data,   m.data_size },
        { "veneers", (uint32_t)(uintptr_t)m.veneer, m.veneer_size },
    };
    int bad = 0;
    if (sm.n != 3) {
        printf("       FAIL %s: the shadow has %d span(s), describe() fills 3\n",
               how, sm.n);
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        if (sm.s[i].base == real[i].base && sm.s[i].size == real[i].size) continue;
        printf("       FAIL %s: describe() in os/shell/apps.cpp gives the %s region "
               "as %08x+%u; this file's shadow of it says %08x+%u.\n"
               "       They have to agree or every region check here is checking "
               "a map the device does not program.\n",
               how, real[i].what, real[i].base, real[i].size,
               sm.s[i].base, sm.s[i].size);
        bad = 1;
    }
    return bad;
}

// Which span an address really lands in. Only ones the hardware will hold: a
// refused span describes nothing.
static const ShadowSpan *shadow_find(const Shadow &sm, uint32_t a) {
    for (int i = 0; i < sm.n; i++) {
        const ShadowSpan &s = sm.s[i];
        if (!s.programmed) continue;
        if (a >= s.base && a < s.base + s.size) return &s;
    }
    return nullptr;
}

// Firmware is in XIP flash and is covered by NO package region, deliberately:
// a sandboxed package reaches it through the supervisor rather than by fetching
// from it. A code pointer landing there is correct, and is judged by the rules
// that already apply to one — Thumb bit set, and an ABI index that resolves.
static bool is_firmware_addr(uint32_t a) {
    return a >= 0x10000000u && a < 0x20000000u;
}

// --- can what was loaded actually RUN? ---------------------------------------
//
// Everything above this point checks the ARITHMETIC of loading: that
// relocations resolve to the right numbers and that the numbers describe
// regions the hardware will take. #103 satisfied all of it and could not
// execute a single instruction, because nothing asked the other question —
// whether the addresses the loader produced are ones the CPU will be ALLOWED to
// fetch from once privilege has been dropped.
//
// So: for every code pointer this load produced, land it on the shadow map and
// require an executable span, with the Thumb bit the ABI requires. A pointer
// that lands in no span at all is the #103 failure exactly, and it is reported
// as what it is rather than as an address that looks perfectly reasonable.
static int check_code_addr(const Shadow &sm, const char *how,
                           const char *what, uint32_t p) {
    int bad = 0;
    if (!p) {
        printf("       FAIL %s: %s is null\n", how, what);
        return 1;
    }
    // ARM ELF carries the Thumb bit in st_value; branching to an even address
    // faults with INVSTATE rather than doing anything useful.
    if (!(p & 1u)) {
        printf("       FAIL %s: %s is %08x — a code address with no Thumb bit\n",
               how, what, p);
        bad = 1;
    }
    uint32_t a = p & ~1u;
    const ShadowSpan *sp = shadow_find(sm, a);
    if (!sp) {
        if (!is_firmware_addr(a)) {
            printf("       FAIL %s: %s points at %08x, which is in none of the "
                   "package's regions and is not firmware — after privilege drops "
                   "nothing lets the CPU fetch it\n", how, what, a);
            bad = 1;
        }
        return bad;                 // firmware: no region by design
    }
    if (!sp->exec) {
        printf("       FAIL %s: %s points at %08x, inside the %s region, which is "
               "NOT executable\n", how, what, a, sp->what);
        bad = 1;
    }
    return bad;
}

// --- the address built out of two instructions -------------------------------
//
// R_ARM_THM_MOVW_ABS_NC and R_ARM_THM_MOVT_ABS materialise a 32-bit address in a
// register out of two instruction halfwords: the MOVW carries the low sixteen
// bits and the MOVT the high sixteen. Nothing else in this file reads one back,
// and it is the ONE relocation where the loader supplies the Thumb bit ITSELF
// rather than taking it from st_value — which is precisely the arithmetic that,
// on R_ARM_ABS32, once produced `function+2` with the bit clear: a pointer that
// links, loads, and faults with INVSTATE the moment it is called.
//
// AAELF32 defines the result as ((S + A) | T), where S is the symbol's address
// with bit 0 CLEAR and T is 1 for a Thumb function. Confirmed against the
// reference implementation rather than from memory: `movw r0, #:lower16:fn` /
// `movt r0, #:upper16:fn` against a Thumb `fn` at 0x20001000, linked by GNU ld,
// comes out as 0x20001001 — the Thumb bit present exactly ONCE. The loader's S
// already carries that bit (resolve() returns map[shndx].addr + st_value, and
// st_value is odd for a Thumb function), so the correct value is S itself.
//
// A is the addend, which for a REL relocation lives in the instruction's own
// immediate field. GCC and GAS emit zero there — `f240 0000` / `f2c0 0000`,
// measured — and the loader OVERWRITES the field rather than adding to it, so a
// nonzero addend is a case it does not implement. That is reported as a failure
// naming what it is, rather than being quietly compared against the wrong
// expectation.

// The T3 encodings. MOVW is 11110 i 10 0100 imm4 / 0 imm3 Rd imm8; MOVT is the
// same with bit 7 of the first halfword's low byte set (0100 -> 1100).
#define MOVW_OP_MASK 0xFBF0u
#define MOVW_OP      0xF240u
#define MOVT_OP      0xF2C0u

static bool mov_imm16_decode(const uint8_t *p, bool want_movt,
                             uint32_t *imm, uint32_t *rd) {
    uint32_t hw1 = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    uint32_t hw2 = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
    if ((hw1 & MOVW_OP_MASK) != (want_movt ? MOVT_OP : MOVW_OP)) return false;
    if (hw2 & 0x8000u) return false;                 // bit 15 is zero in T3
    uint32_t imm4 = hw1 & 0xfu, i = (hw1 >> 10) & 1u;
    uint32_t imm3 = (hw2 >> 12) & 0x7u, imm8 = hw2 & 0xffu;
    *imm = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
    *rd  = (hw2 >> 8) & 0xfu;
    return true;
}

// How the loader resolved a symbol, recomputed from the ELF. Mirrors resolve()
// in loader.cpp: an undefined symbol comes from the firmware — as an address in
// DIRECT mode and as the veneer carrying its ABI index in SVC mode, which is the
// only form a sandboxed package ever sees.
static bool mirror_resolve(const ElfMirror &m, const LoadedApp &app, uint32_t sidx,
                           bool svc, uint32_t *S, bool *is_func) {
    if (sidx >= m.nsyms) return false;
    const Elf32_Sym &sy = m.syms[sidx];
    *is_func = ELF32_ST_TYPE(sy.st_info) == STT_FUNC;
    if (sy.st_shndx == SHN_UNDEF) {
        *is_func = true;                 // everything the firmware exports is callable
        const char *nm = m.str + sy.st_name;
        if (!svc) { *S = api_lookup(nm); return *S != 0; }
        int ix = api_index_of(nm);
        if (ix < 0) return false;
        const uint8_t *v = (const uint8_t *)app.veneers;
        for (uint32_t off = app.veneer_gates; off + 16 <= app.veneers_used; off += 16)
            if (*(const uint32_t *)(v + off + 12) == (uint32_t)ix) {
                *S = ((uint32_t)(uintptr_t)(v + off)) | 1u;
                return true;
            }
        return false;
    }
    if (sy.st_shndx == SHN_ABS) { *S = sy.st_value; return true; }
    if (sy.st_shndx >= (uint32_t)m.eh->e_shnum || !m.placed[sy.st_shndx]) return false;
    *S = m.addr[sy.st_shndx] + sy.st_value;
    return true;
}

// Walk every MOVW/MOVT pair, decode the address back out of the patched
// instructions, and judge it exactly as any other code pointer is judged.
//
// PAIRED BY THE RELOCATIONS, not by scanning for instruction encodings: the two
// halves are two separate relocations naming the same symbol, and the MOVT sits
// four bytes past its MOVW writing the same register. All three of those have to
// agree before a pair is judged, so a stray MOVT belonging to some other MOVW
// cannot be silently combined with the wrong low half.
static int check_movw_movt(const ElfMirror &m, const LoadedApp &app,
                           const Shadow &sm, const char *how, bool svc,
                           int *pairs_out) {
    int bad = 0, pairs = 0;
    for (int s = 0; s < m.eh->e_shnum; s++) {
        if (m.sh[s].sh_type != SHT_REL) continue;
        uint32_t tgt = m.sh[s].sh_info;
        if (tgt >= (uint32_t)m.eh->e_shnum || !m.placed[tgt]) continue;
        Elf32_Rel *rel = (Elf32_Rel *)(m.b + m.sh[s].sh_offset);
        uint32_t n = m.sh[s].sh_size / sizeof(Elf32_Rel);
        for (uint32_t r = 0; r < n; r++) {
            if (ELF32_R_TYPE(rel[r].r_info) != R_ARM_THM_MOVW_ABS_NC) continue;
            uint32_t sidx = ELF32_R_SYM(rel[r].r_info);
            // Its MOVT: same symbol, four bytes on. Anywhere in this section's
            // relocations, since nothing promises they are adjacent in the table.
            const Elf32_Rel *hi = nullptr;
            for (uint32_t q = 0; q < n; q++) {
                if (ELF32_R_TYPE(rel[q].r_info) != R_ARM_THM_MOVT_ABS) continue;
                if (ELF32_R_SYM(rel[q].r_info) != sidx) continue;
                if (rel[q].r_offset != rel[r].r_offset + 4) continue;
                hi = &rel[q]; break;
            }
            const char *nm = sidx < m.nsyms ? m.str + m.syms[sidx].st_name : "?";
            if (!hi) {
                printf("       FAIL %s: the MOVW for %s at +0x%x has no MOVT four "
                       "bytes on — half an address was built and nothing here can "
                       "say what the other half is\n", how, nm, rel[r].r_offset);
                bad = 1; continue;
            }
            pairs++;

            // What the instructions held BEFORE relocation is the addend, and the
            // loader overwrites rather than adds. Read from the file image.
            if (m.sh[tgt].sh_type == SHT_NOBITS) {
                printf("       FAIL %s: a MOVW/MOVT in a section with no contents\n", how);
                bad = 1; continue;
            }
            const uint8_t *orig_lo = m.b + m.sh[tgt].sh_offset + rel[r].r_offset;
            const uint8_t *orig_hi = m.b + m.sh[tgt].sh_offset + hi->r_offset;
            uint32_t a_lo = 0, a_hi = 0, rd_o_lo = 0, rd_o_hi = 0;
            if (!mov_imm16_decode(orig_lo, false, &a_lo, &rd_o_lo) ||
                !mov_imm16_decode(orig_hi, true,  &a_hi, &rd_o_hi)) {
                printf("       FAIL %s: the site for %s does not hold a MOVW/MOVT "
                       "pair before relocation\n", how, nm);
                bad = 1; continue;
            }
            if (a_lo || a_hi) {
                printf("       FAIL %s: %s is referenced with a nonzero addend "
                       "(%u/%u), which the loader overwrites rather than adds\n",
                       how, nm, a_lo, a_hi);
                bad = 1; continue;
            }

            // And what they hold now.
            const uint8_t *lo = (const uint8_t *)(uintptr_t)(m.addr[tgt] + rel[r].r_offset);
            const uint8_t *up = (const uint8_t *)(uintptr_t)(m.addr[tgt] + hi->r_offset);
            uint32_t v_lo = 0, v_hi = 0, rd_lo = 0, rd_hi = 0;
            if (!mov_imm16_decode(lo, false, &v_lo, &rd_lo) ||
                !mov_imm16_decode(up, true,  &v_hi, &rd_hi)) {
                printf("       FAIL %s: patching %s destroyed the MOVW/MOVT opcode "
                       "— the instruction is no longer a move\n", how, nm);
                bad = 1; continue;
            }
            // The register is not the loader's to change. A mask that reached one
            // bit too far would move the address into a register the code never
            // reads, which faults nowhere near here.
            if (rd_lo != rd_o_lo || rd_hi != rd_o_hi) {
                printf("       FAIL %s: patching %s changed the destination "
                       "register (r%u->r%u / r%u->r%u)\n",
                       how, nm, rd_o_lo, rd_lo, rd_o_hi, rd_hi);
                bad = 1; continue;
            }
            if (rd_lo != rd_hi) {
                printf("       FAIL %s: the MOVW and MOVT for %s build into "
                       "different registers (r%u and r%u)\n", how, nm, rd_lo, rd_hi);
                bad = 1; continue;
            }

            uint32_t built = (v_hi << 16) | v_lo;
            uint32_t S = 0; bool is_func = false;
            if (!mirror_resolve(m, app, sidx, svc, &S, &is_func)) {
                printf("       FAIL %s: cannot work out where %s ended up, so the "
                       "address these two instructions build cannot be judged\n",
                       how, nm);
                bad = 1; continue;
            }
            // ((S + A) | T), with A zero and S already carrying T for a function.
            uint32_t want = (S & ~1u) | (is_func ? 1u : 0u);
            if (built != want) {
                printf("       FAIL %s: MOVW/MOVT at +0x%x builds %08x for %s, "
                       "which is at %08x%s\n", how, rel[r].r_offset, built, nm, want,
                       (built == want + 1u)
                           ? " — the Thumb bit was added twice, so this is the "
                             "symbol plus two with the bit CLEAR"
                           : "");
                bad = 1; continue;
            }
            // Right value is not the same claim as reachable.
            if (is_func) {
                char what[96];
                snprintf(what, sizeof(what), "the MOVW/MOVT address of %s", nm);
                bad |= check_code_addr(sm, how, what, built);
            }
        }
    }
    if (pairs_out) *pairs_out = pairs;
    return bad;
}

// The offset a Thumb-2 BL / B.W actually encodes, decoded from the ARMv7-M
// definition rather than by calling the loader's own helper. A checker that used
// the code under test would agree with it whatever either of them did, and the
// encode/decode pair being self-consistently wrong is exactly the bug that
// produces a package which links, loads, and branches into the wrong place.
//
// The CPU computes the target as (address of the instruction) + 4 + this.
static int32_t thumb_branch_off(const uint8_t *p) {
    uint32_t hw1 = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    uint32_t hw2 = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
    uint32_t sgn = (hw1 >> 10) & 1u;
    uint32_t j1  = (hw2 >> 13) & 1u, j2 = (hw2 >> 11) & 1u;
    uint32_t i1  = 1u - (j1 ^ sgn), i2 = 1u - (j2 ^ sgn);
    int32_t v = (int32_t)((i1 << 23) | (i2 << 22) |
                          ((hw1 & 0x3ffu) << 12) | ((hw2 & 0x7ffu) << 1));
    return sgn ? v - (1 << 24) : v;          // two's complement across 25 bits
}

// --- every branch, by where it actually lands --------------------------------
//
// THM_CALL and THM_JUMP24 are most of the relocations in a package — novad1 has
// 3066 of them — and on the copy path nothing judged one. The slot path decoded
// every branch and confirmed it reaches the symbol it names; here the strongest
// claim made was that a veneer's target word was odd, which says nothing about
// whether any BL in the image actually goes there.
//
// A wrong branch is the worst-behaved thing the loader can produce. It links, it
// loads, every other check in this file passes, and the package jumps into the
// middle of an unrelated instruction — the +4 note in loader.cpp is that exact
// mistake, four bytes early, on every call.
//
// Two legitimate destinations, and both are checked rather than either being
// assumed:
//
//   DIRECT — the target is in BL range, so the branch goes straight to it.
//   VENEERED — it is not, which for a firmware call is the normal case rather
//     than an edge one: firmware is in XIP flash 256 MB from SRAM. The branch
//     then goes to a trampoline in the pool, and what has to be true is that the
//     trampoline carries the right destination. In DIRECT veneer mode that is
//     the firmware ADDRESS; in SVC mode the loader resolves an undefined symbol
//     to its supervisor-call veneer before the branch is encoded at all, so the
//     branch is in range and there is no second hop — a veneered branch in SVC
//     mode means an out-of-range target the sandbox cannot express, which the
//     loader is supposed to refuse outright.
static int check_branches(const ElfMirror &m, const LoadedApp &app,
                          const Shadow &sm, const char *how, bool svc,
                          int *sites_out) {
    int bad = 0, sites = 0, direct = 0, veneered = 0, relocs = 0;
    const uint8_t *vbase = (const uint8_t *)app.veneers;

    for (int s = 0; s < m.eh->e_shnum; s++) {
        if (m.sh[s].sh_type != SHT_REL) continue;
        uint32_t tgt = m.sh[s].sh_info;
        Elf32_Rel *rel = (Elf32_Rel *)(m.b + m.sh[s].sh_offset);
        uint32_t n = m.sh[s].sh_size / sizeof(Elf32_Rel);
        for (uint32_t r = 0; r < n; r++) {
            uint32_t type = ELF32_R_TYPE(rel[r].r_info);
            if (type != R_ARM_THM_CALL && type != R_ARM_THM_JUMP24) continue;
            relocs++;
            // Branches live in the executable half. One in a section the loader
            // did not place, or in the writable half, is not something this can
            // read — and it is worth saying so rather than passing over it.
            if (tgt >= (uint32_t)m.eh->e_shnum || !m.placed[tgt]) {
                printf("       FAIL %s: a branch in section %u, which was never "
                       "placed — it cannot have been relocated\n", how, tgt);
                bad = 1; continue;
            }
            if (m.writ[tgt] || m.sh[tgt].sh_type == SHT_NOBITS) continue;

            uint32_t sidx = ELF32_R_SYM(rel[r].r_info);
            const char *nm = sidx < m.nsyms ? m.str + m.syms[sidx].st_name : "?";
            uint32_t S = 0; bool is_func = false;
            if (!mirror_resolve(m, app, sidx, svc, &S, &is_func)) {
                printf("       FAIL %s: cannot work out where the branch to %s "
                       "was meant to land\n", how, nm);
                bad = 1; continue;
            }
            sites++;

            // What the instruction meant before relocation is its addend, and the
            // ARM convention folds the Thumb pipeline offset into it — an
            // unresolved BL encodes -4, not 0. Both sides carry the same +4, so
            // it is written out on both rather than cancelled quietly.
            uint32_t site = m.addr[tgt] + rel[r].r_offset;
            int32_t a_orig = thumb_branch_off(m.b + m.sh[tgt].sh_offset + rel[r].r_offset);
            int32_t a_new  = thumb_branch_off((const uint8_t *)(uintptr_t)site);
            uint32_t reached = site + 4u + (uint32_t)a_new;
            uint32_t want    = (S & ~1u) + (uint32_t)a_orig + 4u;

            if (reached == want) {
                direct++;
            } else if (vbase && reached >= (uint32_t)(uintptr_t)vbase &&
                       reached <  (uint32_t)(uintptr_t)vbase + app.veneers_used) {
                uint32_t voff = reached - (uint32_t)(uintptr_t)vbase;
                veneered++;
                if (voff % 16u) {
                    printf("       FAIL %s: the branch to %s lands %u bytes into "
                           "the veneer pool, which is not the start of a veneer\n",
                           how, nm, voff);
                    bad = 1; continue;
                }
                if (voff < app.veneer_gates) {
                    printf("       FAIL %s: the branch to %s lands on a privilege "
                           "GATE at +%u, not on a call veneer — calling it would "
                           "drop privilege into nowhere\n", how, nm, voff);
                    bad = 1; continue;
                }
                if (svc) {
                    // resolve() has already turned an undefined symbol into its
                    // own veneer, so nothing here can legitimately be out of
                    // range. One that is means a target that is neither firmware
                    // nor inside the image, and a trampoline holding a raw
                    // address where the supervisor expects an index.
                    printf("       FAIL %s: the branch to %s went through a veneer, "
                           "which a sandboxed package has no way to reach\n", how, nm);
                    bad = 1; continue;
                }
                uint32_t lit = *(const uint32_t *)(uintptr_t)(vbase + voff + 12);
                if (lit != (want | 1u)) {
                    printf("       FAIL %s: the branch to %s goes to the veneer at "
                           "+%u, which targets %08x — %s is at %08x\n",
                           how, nm, voff, lit, nm, want | 1u);
                    bad = 1; continue;
                }
            } else {
                printf("       FAIL %s: the branch at +0x%x reaches %08x, but %s is "
                       "at %08x and there is no veneer there\n",
                       how, rel[r].r_offset, reached, nm, want);
                bad = 1; continue;
            }
            // And the same question every other code pointer is asked. The
            // address is even by construction — a Thumb branch offset has no low
            // bit — so the Thumb bit is supplied here rather than checked; what
            // is being asked is whether the CPU will be allowed to fetch there.
            char what[96];
            snprintf(what, sizeof(what), "the branch to %s", nm);
            bad |= check_code_addr(sm, how, what, reached | 1u);
        }
    }
    // A package whose relocations say there are branches, and a walk that judged
    // none of them, is the failure this file exists to be the opposite of.
    if (relocs && !sites) {
        printf("       FAIL %s: %d branch relocation(s) and not one was checked\n",
               how, relocs);
        bad = 1;
    }
    if (sites)
        printf("       %d branch(es): %d direct, %d through a veneer, every one "
               "reaches the symbol it names\n", sites, direct, veneered);
    if (sites_out) *sites_out = sites;
    return bad;
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
static int check_got(const char *path, const LoadedApp &app, bool svc, const char *how) {
    int bad = 0;
    ElfMirror mm;
    // A failure to mirror used to `return 0`, which reads as "the GOT is fine".
    // It is not a verdict, it is the absence of one.
    if (!mirror_open(path, app, &mm)) {
        printf("       FAIL could not read %s back to check the GOT against\n", path);
        return 1;
    }
    Elf32_Ehdr *eh = mm.eh;
    Elf32_Shdr *sh = mm.sh;
    uint8_t *b = mm.b;
    const uint32_t *base = mm.addr;
    const bool *placed = mm.placed;
    const uint32_t text_end = mm.text_end;

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

    Elf32_Sym *syms = mm.syms;
    const char *str = mm.str;
    const uint32_t *got = (const uint32_t *)app.data;
    const Shadow sm = shadow_of(app);

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
            // Resolved through the SAME mirror the branch and MOVW checks use,
            // and told which veneer mode it is in. That is not tidying: in SVC
            // mode an undefined symbol does not resolve to a firmware address at
            // all, it resolves to the package's own supervisor-call veneer — so
            // a GOT checked with the privileged answer would be checked against
            // a number the sandboxed loader never produces.
            uint32_t expect = 0; bool is_func = false;
            if (!mirror_resolve(mm, app, sidx, svc, &expect, &is_func)) {
                // Not a skip. A slot whose expected value cannot be worked out
                // is a slot nothing checked, and the whole subject of this file
                // is checks that did not happen quietly.
                printf("       FAIL %s GOT: cannot work out where %s ended up, so "
                       "its slot went unchecked\n", how, str + sy.st_name);
                bad = 1; continue;
            }
            if (slot != expect) {
                printf("       FAIL %s GOT: slot for %s holds %08x, the symbol is at %08x\n",
                       how, str + sy.st_name, slot, expect);
                bad = 1;
            }
            // A GOT slot naming a FUNCTION is a code pointer the package will
            // load and branch to. Right value is not the same claim as
            // reachable: it has to land somewhere the hardware will let the
            // CPU fetch from.
            if (is_func || ELF32_ST_TYPE(sy.st_info) == STT_FUNC) {
                char what[80];
                snprintf(what, sizeof(what), "the GOT slot for %s", str + sy.st_name);
                bad |= check_code_addr(sm, how, what, slot);
            }
        }
    }
    // No used slot may be zero. Every symbol resolves above the GOT (.data/.bss),
    // into the read-only half (.rodata / a local function) or to a firmware
    // address — never to 0, so a 0 is a slot the loader forgot to fill.
    for (uint32_t k = 0; k < app.got_count; k++)
        if (got[k] == 0) { printf("       FAIL GOT: slot %u was never filled\n", k); bad = 1; }

    // A GOT with slots and no sites, or sites and no slots, is a walk that read
    // nothing — which reads exactly like a pass.
    if (app.got_count && !sites) {
        printf("       FAIL %s GOT: %u slot(s) and not one GOT_BREL site was "
               "walked\n", how, app.got_count);
        bad = 1;
    }
    if (sites && !bad)
        printf("       %s GOT ok: %d GOT_BREL site(s) through %u slot(s), every one resolves right\n",
               how, sites, app.got_count);
    mirror_close(&mm);
    return bad;
}

// Every region the OS will hand the hardware must actually be encodable.
//
// This is the join between the loader's layout and the protection hardware's
// rules, and it fails silently in the direction that matters: an unencodable
// region is refused, set_region leaves it DISABLED, and unprivileged code has
// no default map to fall back on — so "no region" means no access at all,
// including no instruction fetch. Everything reports success and the package
// hard-faults on the first instruction it executes unprivileged.
//
// A veneer is sixteen bytes and a region is described in thirty-twos, so a
// length that is not a whole number of blocks is the shape of the mistake. It
// has now happened twice: once on app_load's pool, and once on app_pic_load's
// three gates, which are 48 bytes — one and a half blocks, refused every time,
// and the fault landed on the enter gate's `bx` at veneers+24. So the check
// runs against BOTH load paths rather than only the one it was written for.
static int check_mpu_regions(const LoadedApp &app, const char *how) {
    int bad = 0;
    Shadow sm = shadow_of(app);
    // Before anything is judged against the shadow: is the shadow still the map
    // the device would program? Asked on every path, because describe() is what
    // runs a package however it was loaded.
    bad |= check_shadow_matches_describe(app, sm, how);
    for (int i = 0; i < sm.n; i++) {
        const ShadowSpan &s = sm.s[i];
        if (!s.size) continue;                    // legitimately absent
        if (!s.programmed) {
            printf("       FAIL %s: the %s region cannot be encoded "
                   "(base %08x, %u bytes) — the hardware would be left with no "
                   "region there at all\n", how, s.what, s.base, s.size);
            bad = 1;
        }
    }
    uint32_t vsize = mpu_align_up(app.veneers_used, MPU_V8_GRAIN);
    if (vsize > app.veneer_size) vsize = app.veneer_size;
    if (app.veneers_used && !vsize) {
        printf("       FAIL %s: veneers were written but the region is empty\n", how);
        bad = 1;
    }
    // The gates are the first thing a sandboxed package executes and the last
    // thing it executes on the way out, so the region has to reach past the LAST
    // of them — not merely past the first.
    if (app.veneer_gates && vsize < app.veneer_gates) {
        printf("       FAIL %s: the region stops at %u B, short of the %u B of gates\n",
               how, vsize, app.veneer_gates);
        bad = 1;
    }
    return bad;
}

// The whole-package half of the same question: the map itself has to be sane,
// and the handful of code pointers that exist outside any relocation — the
// entry point and the three gates — have to be fetchable.
static int check_executable(const LoadedApp &app, const char *how) {
    int bad = 0;
    Shadow sm = shadow_of(app);

    // W^X. Nothing the package can write to may also be somewhere it can
    // execute from; that is the property the two-half split exists to give, and
    // it is one flipped permission away from being lost silently.
    for (int i = 0; i < sm.n; i++) {
        if (sm.s[i].exec && sm.s[i].write) {
            printf("       FAIL %s: the %s region is both writable and executable\n",
                   how, sm.s[i].what);
            bad = 1;
        }
    }
    // And they must not overlap. ARMv8-M calls overlapping regions
    // UNPREDICTABLE: an access one region allows can be refused by the other,
    // and the refusal is reported against an address plainly inside a region
    // that grants it.
    for (int i = 0; i < sm.n; i++) {
        for (int j = i + 1; j < sm.n; j++) {
            const ShadowSpan &a = sm.s[i], &b = sm.s[j];
            if (!a.programmed || !b.programmed) continue;
            if (a.base < b.base + b.size && b.base < a.base + a.size) {
                printf("       FAIL %s: the %s and %s regions overlap "
                       "(%08x+%u and %08x+%u)\n",
                       how, a.what, b.what, a.base, a.size, b.base, b.size);
                bad = 1;
            }
        }
    }

    // The first thing that runs.
    bad |= check_code_addr(sm, how, "the entry point", (uint32_t)(uintptr_t)app.entry);

    // The gates. These are the ones #103 landed on: the enter gate's `bx r3` is
    // the FIRST instruction a sandboxed package executes after privilege has
    // been dropped, and the exit gate is where it returns to. They are kept off
    // flash precisely so they are not also the first test of unprivileged
    // fetch-from-flash — which is worth nothing if their region is not
    // programmed.
    if (app.veneer_gates) {
        bad |= check_code_addr(sm, how, "the enter gate",  app_enter_gate(&app));
        bad |= check_code_addr(sm, how, "the exit gate",   app_exit_gate(&app));
        bad |= check_code_addr(sm, how, "the return gate", app_return_gate(&app));
    }

    // r9, for a position-independent package. Every global it has is reached as
    // an offset from this, so a base that is not the writable block means the
    // package reads someone else's memory for every variable it owns — which is
    // not a crash, it is wrong answers. check_got asserts this on the copy path;
    // asserting it here covers the slot path too, which had nothing.
    if (app.got_size) {
        uint32_t r9 = app_pic_base(&app);
        if (r9 != (uint32_t)(uintptr_t)app.data) {
            printf("       FAIL %s: r9 would be %08x, but the writable block "
                   "(and so the GOT) is at %08x\n",
                   how, r9, (uint32_t)(uintptr_t)app.data);
            bad = 1;
        }
        if (app.got_size > app.data_size) {
            printf("       FAIL %s: a %u-byte GOT does not fit a %u-byte writable "
                   "block\n", how, app.got_size, app.data_size);
            bad = 1;
        }
        // The GOT is data. A package that could execute its own GOT could
        // execute anything it can write.
        const ShadowSpan *sp = shadow_find(sm, r9);
        if (!sp) {
            printf("       FAIL %s: the GOT at %08x is in no region the hardware "
                   "will hold — the package cannot read its own globals\n", how, r9);
            bad = 1;
        } else if (sp->exec || !sp->write) {
            printf("       FAIL %s: the GOT at %08x is in the %s region, which is "
                   "%s\n", how, r9, sp->what,
                   sp->exec ? "executable" : "not writable");
            bad = 1;
        }
    }
    return bad;
}

static int load_one(const char *path) {
    g_f = fopen(path, "rb");
    if (!g_f) return missing_artifact(path);
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

    bad |= check_mpu_regions(app, "copy-to-RAM");
    bad |= check_executable(app, "copy-to-RAM");
    // And the decisive one: walk every byte of the image and check that nothing
    // writable ended up before text_end and nothing read-only after it.
    {
        uint32_t text_end = 0, misplaced = 0;
        g_section_read_failed = false;
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
        if (g_section_read_failed) {
            printf("       FAIL %s could not be read back, so no byte of it was "
                   "checked against the half it landed in\n", path);
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
    // DRIVEN BY THE RELOCATIONS, and that is the fix for a flake that made this
    // whole suite untrustworthy (task #101). It used to scan every word of both
    // halves and judge any value that landed inside the loaded ranges and
    // pointed at an executable section. That cannot be made to work, for a
    // reason easy to miss: most words in the read-only half are INSTRUCTIONS,
    // and an instruction pair is a 32-bit number like any other. novad1 holds
    // `cmp r6, r2 / adcs r3, r3` at .text+0xac8 — the word 0x415b4296 — and the
    // host's arena is mmapped at an address the kernel randomises, so about one
    // run in ten it landed at 0x415b____ and that pair "pointed" into the image.
    // The suite then reported a code pointer with no Thumb bit at an offset
    // holding no pointer at all, at a different offset each time, and it read
    // exactly like a loader bug. It was the test.
    //
    // The relocations know which words are pointers, so ask them. That is also
    // strictly stricter: a site is judged because it IS one, and the symbol it
    // names says whether it is a function, instead of an address range being
    // asked to guess. R_ARM_ABS32 and R_ARM_TARGET1 are the two the loader
    // applies by writing an absolute address into a word — see the switch in
    // loader.cpp — so they are the two walked here.
    int checked = 0, even_ptrs = 0, exec_bad = 0;
    {
        const Shadow sm = shadow_of(app);
        ElfMirror pm;
        if (!mirror_open(path, app, &pm)) {
            printf("       FAIL could not read %s back to walk its relocations\n", path);
            bad = 1;
        } else {
            for (int s = 0; s < pm.eh->e_shnum; s++) {
                if (pm.sh[s].sh_type != SHT_REL) continue;
                uint32_t tgt = pm.sh[s].sh_info;
                if (tgt >= (uint32_t)pm.eh->e_shnum || !pm.placed[tgt]) continue;
                Elf32_Rel *rel = (Elf32_Rel *)(pm.b + pm.sh[s].sh_offset);
                uint32_t n = pm.sh[s].sh_size / sizeof(Elf32_Rel);
                for (uint32_t r = 0; r < n; r++) {
                    uint32_t type = ELF32_R_TYPE(rel[r].r_info);
                    if (type != R_ARM_ABS32 && type != R_ARM_TARGET1) continue;
                    uint32_t sidx = ELF32_R_SYM(rel[r].r_info);
                    if (sidx >= pm.nsyms) continue;
                    const Elf32_Sym &sy = pm.syms[sidx];
                    // Only a pointer to CODE has to be odd; one to data is
                    // legitimately even and judging it would be noise.
                    //
                    // Two ways of being code, and the union of them on
                    // purpose. STT_FUNC is the symbol saying so, which is what
                    // the loader itself keys on. Landing in a section marked
                    // executable is what the scan this replaced used, and GCC
                    // does emit STT_NOTYPE for assembly labels and some
                    // aliases — so taking only the first would have narrowed
                    // what is judged. (Measured across all six packages: no
                    // site is currently in the second set and not the first.
                    // Asking both means that stays true without being
                    // re-measured.)
                    bool is_func = ELF32_ST_TYPE(sy.st_info) == STT_FUNC;
                    bool into_code = sy.st_shndx < pm.eh->e_shnum &&
                                     pm.placed[sy.st_shndx] &&
                                     (pm.sh[sy.st_shndx].sh_flags & SHF_EXECINSTR);
                    if (!is_func && !into_code) continue;
                    uint32_t site = pm.addr[tgt] + rel[r].r_offset;
                    uint32_t v = *(const uint32_t *)(uintptr_t)site;
                    checked++;
                    if (!(v & 1u)) {
                        printf("       FAIL %s is stored at %08x as %08x — a function "
                               "pointer with NO Thumb bit\n",
                               pm.str + sy.st_name, site, v);
                        even_ptrs++;
                    }
                    // And the other half of the question: is it fetchable?
                    // A pointer to a real function that the protection unit
                    // will not let the CPU fetch from is a package that loads
                    // perfectly and cannot run.
                    char what[80];
                    snprintf(what, sizeof(what), "the stored pointer to %s",
                             pm.str + sy.st_name);
                    exec_bad += check_code_addr(sm, "copy-to-RAM", what, v);
                }
            }
            // The other kind of stored address: the pair of instructions that
            // builds one in a register. Zero of these in anything GCC emits at
            // -Os, which is why the count is PRINTED rather than assumed — see
            // check_movw_fixture, which exists because a check with nothing to
            // check is not a check.
            int mpairs = 0;
            bad |= check_movw_movt(pm, app, sm, "copy-to-RAM", false, &mpairs);
            g_movw_real += mpairs;
            // And every branch, by where it lands rather than by whether some
            // veneer word happened to be odd.
            int brs = 0;
            bad |= check_branches(pm, app, sm, "copy-to-RAM", false, &brs);
            g_branches += brs;
            mirror_close(&pm);
        }
    }
    printf("       %d stored function pointer(s), %d missing the Thumb bit, "
           "%d not executable\n", checked, even_ptrs, exec_bad);
    if (even_ptrs || exec_bad) bad = 1;

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
    printf("       %d veneer target(s), %d missing the Thumb bit\n", vtargets, veven);
    if (veven) bad = 1;
    // Every package here calls the firmware, and firmware is 256 MB away in XIP
    // flash, so every one of them MUST have gone through a trampoline. A pool
    // with bytes in it and no target words in range is a pool full of something
    // else, and it used to print nothing at all.
    if (app.veneers_used && !vtargets) {
        printf("       FAIL %u B of veneers and not one target address among them\n",
               app.veneers_used);
        bad = 1;
    }

    // A position-independent package: verify the whole r9/GOT indirection.
    if (app.got_size) bad += check_got(path, app, false, "copy-to-RAM");

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
    if (!g_f) return missing_artifact(path);
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

    // THE CONFIGURATION A SANDBOXED PACKAGE ACTUALLY RUNS IN, and until now the
    // one nothing asked about. In SVC mode the copy path grows the same three
    // gates the slot path always has, and they are the first and last
    // instructions executed with privilege dropped. The regions were checked in
    // DIRECT mode, where there are no gates at all.
    bad |= check_mpu_regions(app, "sandboxed");
    bad |= check_executable(app, "sandboxed");

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

    // And where every branch in the image actually goes, which in this mode is a
    // different question from the same walk on the privileged path: an undefined
    // symbol has been resolved to its OWN supervisor-call veneer before the
    // branch was encoded, so every call is in range and none of them should have
    // needed a trampoline. A wrong index in the veneer is caught above; a branch
    // that reaches the wrong veneer calls the wrong function just as thoroughly,
    // and nothing asked about that.
    {
        const Shadow sm = shadow_of(app);
        ElfMirror pm;
        if (!mirror_open(path, app, &pm)) {
            printf("       FAIL sandboxed: could not read %s back to walk its "
                   "branches\n", path);
            bad = 1;
        } else {
            int mpairs = 0, brs = 0;
            bad |= check_movw_movt(pm, app, sm, "sandboxed", true, &mpairs);
            g_movw_real += mpairs;
            bad |= check_branches(pm, app, sm, "sandboxed", true, &brs);
            g_branches += brs;
            mirror_close(&pm);
        }
    }

    // And the GOT this mode produces, which is a DIFFERENT GOT. Nothing checked
    // it: check_got ran on the privileged load only, where an undefined symbol
    // resolves to a firmware address. Sandboxed it resolves to the package's own
    // supervisor-call veneer instead, so every slot naming firmware holds a
    // different number — the numbers a device actually runs on, and the ones
    // that were never read back.
    if (app.got_size) bad += check_got(path, app, true, "sandboxed");

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

// A flash chip, for the end-to-end run through the real slot format. It has to
// be inside the low arena rather than in bss, because the loader keeps addresses
// in uint32_t and a package loaded from a slot resolves against the slot's own
// address — the same reason the images are mmapped low.
//
// pkgslot_test proves the format against a fake in far more detail; this one
// exists so the pipeline can be run end to end with a REAL package. It still
// refuses to program a bit back on, because that is the rule whose breaking
// would make everything else here meaningless.
static uint8_t *g_fake_flash;
static uint32_t g_fake_bytes;
static bool     g_fake_violation;

static bool fake_slot_erase(void *, uint32_t off, uint32_t len) {
    if (off % PKGSLOT_ERASE || len % PKGSLOT_ERASE) { g_fake_violation = true; return false; }
    if ((uint64_t)off + len > g_fake_bytes) { g_fake_violation = true; return false; }
    memset(g_fake_flash + off, 0xFF, len);
    return true;
}
static bool fake_slot_program(void *, uint32_t off, const void *data, uint32_t len) {
    if (off % PKGSLOT_PROG || len % PKGSLOT_PROG) { g_fake_violation = true; return false; }
    if ((uint64_t)off + len > g_fake_bytes) { g_fake_violation = true; return false; }
    const uint8_t *s = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        if ((g_fake_flash[off + i] & s[i]) != s[i]) g_fake_violation = true;
        g_fake_flash[off + i] &= s[i];
    }
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
    if (!f) return missing_artifact(path);
    fseek(f, 0, SEEK_END); uint32_t fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(fsz);
    // Every one of the three exits below used to `return 0` — a pass, from a
    // function that had checked nothing. The slot path is the one #103 hid in,
    // so an unchecked run of it must be loud.
    if (!b || fread(b, 1, fsz, f) != fsz) {
        printf("  FAIL %-12s could not be read back for the slot checks\n", name);
        fclose(f); free(b); return 1;
    }
    fclose(f);
    Elf32_Ehdr *eh = (Elf32_Ehdr *)b;
    Elf32_Shdr *sh = (Elf32_Shdr *)(b + eh->e_shoff);
    int symtab_i = -1;
    for (int i = 0; i < eh->e_shnum; i++) if (sh[i].sh_type == SHT_SYMTAB) symtab_i = i;
    if (symtab_i < 0) {
        printf("  FAIL %-12s has no symbol table — nothing here can be checked\n", name);
        free(b); return 1;
    }
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
    //
    // BLOCK-ALIGNED, because a real slot is a flash sector and one is. The arena
    // deliberately hands out 8-aligned pointers so a loader that forgot to align
    // its own heap blocks is caught — but the slot is not a heap block, and
    // leaving it 8-aligned models a device that does not exist while hiding the
    // thing that matters here: the blob base is handed to the protection unit as
    // the code region, so it has to be one the hardware can describe.
    auto slot_alloc = [&]() -> uint8_t * {
        uint8_t *p = (uint8_t *)alloc32(SLOTCAP + APP_BLOCK_ALIGN);
        if (!p) return nullptr;
        return (uint8_t *)(((uintptr_t)p + (APP_BLOCK_ALIGN - 1)) & ~(uintptr_t)(APP_BLOCK_ALIGN - 1));
    };
    uint8_t *slotA = slot_alloc();
    uint8_t *slotB = slot_alloc();
    // Not a skip. An arena too small for two slots means the whole slot path
    // went unchecked for this package, and a suite that prints a pass for that
    // is the exact instrument that let #103 through. Raise ARENA instead.
    if (!slotA || !slotB) {
        printf("  FAIL %-12s the %u KB arena does not fit two %u KB slots — nothing\n"
               "       on the slot path was checked. Raise ARENA in this file.\n",
               name, (unsigned)(ARENA / 1024), (unsigned)(SLOTCAP / 1024));
        free(b); return 1;
    }
    SlotBuf sbA{slotA, SLOTCAP, 0}, sbB{slotB, SLOTCAP, 0};
    PicManifest mA{}, mB{};
    AppSource src{}; src.read = file_read; src.ctx = nullptr; src.size = fsz;

    // 0) THE PRE-FLIGHT, first, because that is the order the device uses it in.
    //
    // app_pic_measure decides whether this package goes to a slot at all, and it
    // has to decide BEFORE pkgslot_begin erases the slot's header — after that
    // point a refusal costs the package that was already there. It works from the
    // section headers and one relocation scan, which means it is a second
    // implementation of the layout the producer computes, and two implementations
    // drift. What is asserted below is exactly what the routing relies on:
    // the RAM figure is EXACT, the blob and body figures are upper bounds, and
    // the slack in those bounds is small enough to be honest.
    PicMeasure pm{};
    g_f = fopen(path, "rb");
    g_read_bytes = 0; g_read_calls = 0;
    LoadResult rcM = app_pic_measure(src, &pm);
    uint64_t measure_read = g_read_bytes;
    uint32_t measure_calls = g_read_calls;
    fclose(g_f);
    if (rcM != LOAD_OK) {
        printf("  FAIL %-12s pic-measure: %s\n", name, load_result_str(rcM));
        free(b); return 1;
    }

    g_f = fopen(path, "rb");
    g_read_bytes = 0; g_read_calls = 0;
    LoadResult rcA = app_pic_install(src, slot_sink, &sbA, &mA);
    uint64_t install_read = g_read_bytes;
    uint32_t install_calls = g_read_calls;
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
    g_pic_checked++;
    if (mA.ro_size > g_slot_biggest_blob) g_slot_biggest_blob = mA.ro_size;
    printf("  ok   %-12s slot %u B (text %u + rodata %u + veneers %u), RAM %u B\n",
           name, mA.ro_size, mA.text_size, mA.rodata_size, mA.veneer_size, mA.ram_size);

    // What the install COST, from the loader's own accounting rather than from
    // section sums — the number that decides whether a board can install this at
    // all. `biggest` is the one that fails first: a booted device has around
    // 89 KB in its largest free block and far more free in total, so a single
    // request over that is refused while the total looks fine.
    //
    // THE ASSERTION THAT KEEPS THIS WORKING. Every install failure on a board so
    // far has been one allocation nobody counted — the read-only half, the string
    // table, the symbol table, the over-sized ABS32 recipe — and each looked fine
    // in a section sum. A device does not fail on the total; it fails when one
    // request is bigger than the largest free block, which is around 89 KB on a
    // booted board while the free total is nearer 280.
    uint32_t peak = 0, biggest = 0, cuts = 0;
    app_pic_install_cost(&peak, &biggest, &cuts);
    printf("       install: peak %u B held, biggest single %u B, read %llu B in %u calls "
           "(file %u B), %u page cut(s)\n",
           peak, biggest, (unsigned long long)install_read, install_calls, fsz, cuts);
    if (biggest >= 89u * 1024u) {
        printf("       FAIL install asks for %u B in one block — over the 89 KB "
               "largest free block a booted device has\n", biggest);
        bad = 1;
    }
    // And the total, because three allocations of 80 KB would each pass the test
    // above and still not fit.
    if (peak >= 89u * 1024u) {
        printf("       FAIL install holds %u B at once — over the 89 KB block\n", peak);
        bad = 1;
    }

    // What the body will really be, in pkgslot_commit's order. The producer does
    // not report it, so it is reconstructed here from the manifest and checked
    // against the writer's own count in (5).
    auto up4 = [](uint32_t v) { return (v + 3u) & ~3u; };
    uint32_t real_body = up4(mA.ro_size) + mA.got_count * (uint32_t)sizeof(PicGotEntry);
    real_body = up4(real_body) + mA.abs_count * (uint32_t)sizeof(PicAbs32);
    real_body = up4(real_body) + mA.data_size;
    const uint32_t need = (pm.body_bound + PKGSLOT_PROG - 1u) & ~(PKGSLOT_PROG - 1u);

    printf("       measure: %s, RAM %u B, blob <= %u B, body <= %u B (real %u B, "
           "slack %d B), read %llu B in %u calls\n",
           pm.pic ? "PIC" : "not PIC", pm.ram_size, pm.ro_bound, pm.body_bound,
           real_body, (int)need - (int)real_body,
           (unsigned long long)measure_read, measure_calls);

    // Exact, both of them: they come from the same section walk the producer
    // does, and the RAM figure is what a caller would quote as the resident cost.
    if (pm.ram_size != mA.ram_size) {
        printf("       FAIL measure says RAM %u B, install produced %u B\n",
               pm.ram_size, mA.ram_size); bad = 1;
    }
    if (pm.got_bytes != mA.got_bytes) {
        printf("       FAIL measure says GOT %u B, install produced %u B\n",
               pm.got_bytes, mA.got_bytes); bad = 1;
    }
    // Every package here is PIC — check_pic is only called for those — so a
    // measure that says otherwise would route it to the copy path and this whole
    // feature would quietly never happen.
    if (!pm.pic) { printf("       FAIL measure does not see this as PIC\n"); bad = 1; }
    // Bounds, and in the safe direction. A bound BELOW the real figure is the
    // dangerous one: it accepts a package that then runs off the end of a slot.
    if (pm.ro_bound < mA.ro_size) {
        printf("       FAIL blob bound %u B is under the real %u B\n",
               pm.ro_bound, mA.ro_size); bad = 1;
    }
    if (need < real_body) {
        printf("       FAIL body bound %u B (rounded %u) is under the real %u B\n",
               pm.body_bound, need, real_body); bad = 1;
    }
    // And the slack, because a bound that is merely SAFE can also be useless.
    // Nearly all of it is the veneer pool's ceiling — one per firmware function
    // the ABI exports — and if that clamp ever stops applying the bound jumps to
    // one veneer per relocation, which for a Nova D1 is 100 KB of a 256 KB slot
    // and would start refusing packages that fit perfectly well.
    if (need - real_body > 32u * 1024u) {
        printf("       FAIL body bound %u B overshoots the real %u B by %u\n",
               need, real_body, need - real_body); bad = 1;
    }

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

    // The same join the copy path is held to. It was NOT checked here, and that
    // is the whole of why a slot-loaded package hard-faulted on a board while
    // every host suite stayed green: the gate pool was 48 bytes, the hardware
    // cannot describe 48 bytes, and the region was silently left off.
    bad |= check_mpu_regions(appA, "from a slot");

    // Resolve a symbol to its runtime address the way the CPU would see it, from
    // the ELF alone: firmware -> the blob veneer holding its ABI index; a defined
    // symbol -> the slot (RO) or the RAM block (writable) at its section offset.
    auto resolve = [&](const LoadedApp &app, const uint8_t *blob, uint32_t sidx, bool *ok) -> uint32_t {
        *ok = true;
        const Elf32_Sym &s = syms[sidx];
        bool func = (ELF32_ST_TYPE(s.st_info) == STT_FUNC);
        uint32_t slotbase = (uint32_t)(uintptr_t)blob;
        if (s.st_shndx == SHN_UNDEF) {
            int ix = api_index_of(str + s.st_name);
            // -1 is not an index. Left to become (uint32_t)-1 it is
            // VENEER_NOT_AN_INDEX, the literal the gates carry — so a fake
            // symbol table that filled up would go looking for a veneer holding
            // the gate's own marker, and the failure would name the symbol
            // rather than the table. That has happened once already, at
            // FAKE_SYMS 64, and it read exactly like a loader bug.
            if (ix < 0) {
                printf("       FAIL the fake firmware symbol table filled up at %u "
                       "entries — raise FAKE_SYMS; nothing below this point was "
                       "checked for %s\n", api_symbol_count(), str + s.st_name);
                *ok = false; return 0;
            }
            for (uint32_t off = mA.veneer_off; off + 16 <= mA.veneer_off + mA.veneer_size; off += 16)
                if (*(uint32_t *)(blob + off + 12) == (uint32_t)ix) return (slotbase + off) | 1u;
            *ok = false; return 0;
        }
        if (s.st_shndx == SHN_ABS) return s.st_value;
        if (s.st_shndx >= (uint32_t)eh->e_shnum || !placed[s.st_shndx]) { *ok = false; return 0; }
        uint32_t a = writ[s.st_shndx] ? (uint32_t)(uintptr_t)app.data + ram_off[s.st_shndx] + s.st_value
                                      : slotbase + blob_off[s.st_shndx] + s.st_value;
        return a | (func ? 1u : 0u);
    };

    // Is this symbol something the CPU will be asked to BRANCH to? The union
    // of the two ways of saying so — STT_FUNC, which is what the loader keys
    // on, and landing in a section marked executable, which catches the
    // STT_NOTYPE assembly labels and aliases GCC also emits.
    auto points_at_code = [&](uint32_t sidx) -> bool {
        if (sidx >= nsyms) return false;
        const Elf32_Sym &s = syms[sidx];
        if (ELF32_ST_TYPE(s.st_info) == STT_FUNC) return true;
        return s.st_shndx < (uint32_t)eh->e_shnum && placed[s.st_shndx] &&
               (sh[s.st_shndx].sh_flags & SHF_EXECINSTR) != 0;
    };

    auto verify = [&](const LoadedApp &app, const uint8_t *blob, const char *tag) {
        const uint32_t *got = (const uint32_t *)app.data;
        // The permission map this instance will run under. Everything resolved
        // below is checked against it as well as against its expected value:
        // #103 produced perfectly correct addresses that the CPU was not
        // allowed to fetch from.
        const Shadow sm = shadow_of(app);
        int got_sites = 0, abs_sites = 0, br_sites = 0;
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
                    uint32_t off = *(uint32_t *)(blob + blob_off[tgt] + rel[r].r_offset);
                    if (off % 4 || off >= mA.got_bytes) { printf("       FAIL %s GOT offset %u out of range\n", tag, off); bad = 1; continue; }
                    bool ok; uint32_t want = resolve(app, blob, sidx, &ok);
                    if (!ok) { printf("       FAIL %s cannot resolve %s\n", tag, str + syms[sidx].st_name); bad = 1; continue; }
                    if (got[off / 4] != want) {
                        printf("       FAIL %s GOT[%u] = %08x, %s is at %08x\n",
                               tag, off / 4, got[off / 4], str + syms[sidx].st_name, want); bad = 1;
                    }
                    // Right value, and fetchable. On this path an undefined
                    // symbol resolves to a call veneer INSIDE the blob, which
                    // is the code region — so unlike the copy path there is no
                    // firmware address here to make an exception for, and the
                    // check applies to every function the package can reach.
                    if (points_at_code(sidx)) {
                        char what[80];
                        snprintf(what, sizeof(what), "GOT[%u], the slot for %s",
                                 off / 4, str + syms[sidx].st_name);
                        bad |= check_code_addr(sm, tag, what, got[off / 4]);
                    }
                } else if (type == R_ARM_THM_CALL || type == R_ARM_THM_JUMP24) {
                    // THE MAJORITY OF THE RELOCATIONS IN A PACKAGE, and until
                    // this case existed nothing checked one. novad1 has 2856 of
                    // them against 2159 GOT slots, and a branch that came out
                    // wrong would pass every other check here: the blob-to-blob
                    // comparison cannot see it, because both blobs are produced
                    // by the same page loop and a dropped patch is identical in
                    // each.
                    if (writ[tgt]) continue;          // branches live in the RO half
                    br_sites++;
                    bool ok; uint32_t S = resolve(app, blob, sidx, &ok);
                    if (!ok) { printf("       FAIL %s cannot resolve branch %s\n",
                                      tag, str + syms[sidx].st_name); bad = 1; continue; }
                    uint32_t site = blob_off[tgt] + rel[r].r_offset;
                    // What the instruction MEANT before relocation, and what it
                    // reaches now. The +4 pipeline offset is on both sides and
                    // cancels, so this holds wherever the blob happens to be.
                    int32_t a_orig = thumb_branch_off(b + sh[tgt].sh_offset + rel[r].r_offset);
                    int32_t a_new  = thumb_branch_off(blob + site);
                    uint32_t have = (uint32_t)(uintptr_t)blob + site + (uint32_t)a_new;
                    uint32_t want = (S & ~1u) + (uint32_t)a_orig;
                    if (have != want) {
                        printf("       FAIL %s branch at +%u reaches %08x, %s is at %08x\n",
                               tag, site, have + 4, str + syms[sidx].st_name, want + 4);
                        bad = 1;
                    }
                } else if (type == R_ARM_ABS32 || type == R_ARM_TARGET1) {
                    abs_sites++;
                    if (!writ[tgt]) continue;      // handled as a branch/GOT in RO
                    bool ok; uint32_t S = resolve(app, blob, sidx, &ok);
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
                    // An ABS32 naming a function is a function pointer stored
                    // in the writable half, which is where the misdiagnosed
                    // novad1 fault was thought to live. Whether or not that was
                    // the cause, it is a real way for a package to load and not
                    // run, so it is asked here rather than assumed.
                    if (points_at_code(sidx)) {
                        char what[80];
                        snprintf(what, sizeof(what), "the stored pointer to %s",
                                 str + syms[sidx].st_name);
                        bad |= check_code_addr(sm, tag, what, have);
                    }
                }
            }
        }
        // No used GOT slot may be zero — every symbol resolves somewhere real.
        for (uint32_t k = 0; k < app.got_count; k++)
            if (got[k] == 0) { printf("       FAIL %s GOT[%u] never filled\n", tag, k); bad = 1; }
        // And the whole-package half: the map's own shape, the entry point, the
        // three gates, and r9. The gates are what #103 faulted on and this path
        // is where it hid.
        bad |= check_executable(app, tag);
        // A WALK THAT FOUND NOTHING IS NOT A PASS. Every line above is inside a
        // loop over relocation sections, so a mirror that placed nothing, a
        // section index that stopped matching, or an ELF read back wrong would
        // run all of it zero times and fall out here with `bad` untouched —
        // which is the shape of #103 exactly, one level up.
        if (!got_sites && !br_sites && !abs_sites) {
            printf("       FAIL %s: not one relocation site was resolved — this "
                   "instance was not checked at all\n", tag);
            bad = 1;
        }
        g_slot_got += got_sites; g_slot_branch += br_sites; g_slot_abs += abs_sites;
        // Reported whether or not something else already failed: the counts are
        // how a reader sees how much cover this path really has, and suppressing
        // them on the first failure hides exactly that.
        printf("       %s: %d GOT + %d branch + %d ABS32 site(s) resolve against the slot at %p%s\n",
               tag, got_sites, br_sites, abs_sites, (void *)app.data,
               bad ? "" : ", every code pointer executable");
    };
    verify(appA, slotA, "load A");
    verify(appB, slotA, "load B");

    // 4) a slot built for another ABI is refused, not run — the one failure that
    // would otherwise not fault, because a stale index calls the wrong function.
    PicManifest badver = mA; badver.api_major = (uint16_t)(mA.api_major + 1);
    LoadedApp appX{};
    if (app_pic_load(slotA, &badver, &appX) != LOAD_ERR_API_MISMATCH) {
        printf("       FAIL a slot from a newer ABI was not refused\n"); bad = 1;
        app_unload(&appX);
    }

    // And a slot that does not start on a protection block, which would give the
    // package a code region the hardware silently declines to program. Refused
    // rather than loaded: `pkg` falls back to the file, which works.
    LoadedApp appY{};
    if (app_pic_load(slotA + 8, &mA, &appY) != LOAD_ERR_SLOT_ALIGN) {
        printf("       FAIL a slot off the block boundary was not refused\n"); bad = 1;
        app_unload(&appY);
    }

    // 5) THE WHOLE PIPELINE, through the real slot format.
    //
    // Everything above proves the producer and the loader agree with each other.
    // This proves they still agree with a package that went to FLASH in between:
    // the same ELF, streamed page by page into a real PkgSlotWriter over a fake
    // chip that erases to 0xFF and refuses to turn a bit back on, committed,
    // reopened from the mapping, and loaded from there.
    //
    // slotB is reused as the chip. It has already done its job — proving the blob
    // is position-independent — and 256 KB more of the arena would be spent on
    // nothing.
    {
        g_fake_flash = slotB;
        g_fake_bytes = SLOTCAP;
        g_fake_violation = false;
        SlotFlash fl{ nullptr, fake_slot_erase, fake_slot_program };
        static PkgSlotWriter w;
        PicManifest mS{};
        bool okw = pkgslot_begin(&w, &fl, 0, SLOTCAP);
        g_f = fopen(path, "rb");
        LoadResult rcS = okw ? app_pic_install(src, pkgslot_sink, &w, &mS) : LOAD_ERR_OOM;
        fclose(g_f);
        if (rcS != LOAD_OK) {
            printf("       FAIL install into a slot: %s\n", load_result_str(rcS)); bad = 1;
        } else if (!pkgslot_commit(&w, &mS)) {
            printf("       FAIL committing the slot\n"); bad = 1;
            app_pic_manifest_free(&mS);
        } else {
            app_pic_manifest_free(&mS);      // the slot is the copy that matters now
            PicManifest ms{};
            PkgSlotStatus st = pkgslot_open(slotB, SLOTCAP, &ms);
            if (st != PKGSLOT_OK) {
                printf("       FAIL reopening the slot: %s\n", pkgslot_status_str(st)); bad = 1;
            } else {
                const uint8_t *blob = (const uint8_t *)pkgslot_blob(slotB);
                // Page-at-a-time through a flash format must reproduce the blob
                // the one-shot path produced, byte for byte. If the page cut, the
                // 0xFF padding or the CRC range were wrong, this is where it shows.
                if (ms.ro_size != mA.ro_size || memcmp(blob, slotA, mA.ro_size) != 0) {
                    printf("       FAIL the slot blob differs from the assembled one\n"); bad = 1;
                }
                LoadedApp appS{};
                if (app_pic_load(blob, &ms, &appS) != LOAD_OK) {
                    printf("       FAIL loading from the slot\n"); bad = 1;
                } else {
                    verify(appS, blob, "from slot");
                    bad |= check_mpu_regions(appS, "from a real slot");
                    printf("       ran the whole way: ELF -> %u B slot -> %u B resident\n",
                           ms.ro_size, appS.bytes_allocated);
                    app_unload(&appS);
                }
                app_pic_manifest_free(&ms);   // borrowed: must release nothing

                // What the writer actually programmed, against what the
                // pre-flight promised before anything was erased.
                if (w.page_base > need) {
                    printf("       FAIL the slot body is %u B; measure allowed %u\n",
                           w.page_base, need); bad = 1;
                }
            }
        }

        // 6) THE BOOT FALLBACK, and it has to LOAD, not merely be declined.
        //
        // A firmware update whose ABI moved leaves every slot on the device
        // holding baked indices that now name different functions. That is the
        // one failure a package would not crash on — it would call the wrong
        // thing, happily — so pkgslot_open refuses the slot, and the package has
        // to come up some other way or the device loses it.
        //
        // The other way is the .app file, which is still on the filesystem: the
        // slot path never removes it, precisely so this exists. Boot is also when
        // it can be afforded, because the heap has one large contiguous block at
        // that point and the copy needs the image in one piece.
        //
        // The slot is aged here by editing the mapping directly, not by writing
        // through the fake chip. Flash cannot turn a bit back on, and the state
        // being reproduced is a slot some OLDER firmware wrote correctly — its
        // CRCs agree with its contents. Nothing is corrupt; it is simply not ours.
        {
            PkgSlotMeta meta;
            memcpy(&meta, slotB + PKGSLOT_META_OFF, sizeof(meta));
            meta.api_minor = (uint16_t)(RPC_API_MINOR + 1);
            uint8_t metabuf[PKGSLOT_META_BYTES];
            memcpy(metabuf, slotB + PKGSLOT_META_OFF, PKGSLOT_META_BYTES);
            memcpy(metabuf, &meta, sizeof(meta));
            memcpy(slotB + PKGSLOT_META_OFF, metabuf, PKGSLOT_META_BYTES);
            PkgSlotCommit c;
            memcpy(&c, slotB, sizeof(c));
            c.meta_crc = pkgslot_crc32(0, metabuf, PKGSLOT_META_BYTES);
            memcpy(slotB, &c, sizeof(c));

            PicManifest aged{};
            PkgSlotStatus as = pkgslot_open(slotB, SLOTCAP, &aged);
            if (as != PKGSLOT_BAD_ABI) {
                printf("       FAIL an aged slot opened as %s, not bad ABI\n",
                       pkgslot_status_str(as)); bad = 1;
            }

            // The fallback itself: the same AppSource the install read, through
            // the ordinary copy-to-RAM loader. A full relocation pass — this is a
            // load, not a status code.
            g_f = fopen(path, "rb");
            LoadedApp fb{};
            LoadResult fbrc = app_load(src, &fb);
            fclose(g_f);
            if (fbrc != LOAD_OK) {
                printf("       FAIL the boot fallback did not load: %s%s%s\n",
                       load_result_str(fbrc), fb.detail[0] ? " - " : "", fb.detail);
                bad = 1;
            } else {
                uint32_t e = (uint32_t)(uintptr_t)fb.entry & ~1u;
                uint32_t base = (uint32_t)(uintptr_t)fb.image;
                bool sane = fb.entry && (e >= base) && (e < base + fb.text_size) &&
                            strcmp(fb.header.name, mA.header.name) == 0;
                if (!sane) {
                    printf("       FAIL the fallback loaded something wrong: '%s' "
                           "entry %08x image %08x+%u\n",
                           fb.header.name, e, base, fb.text_size);
                    bad = 1;
                } else {
                    printf("       slot refused (%s) -> loaded '%s' from the file "
                           "instead, %u B resident\n",
                           pkgslot_status_str(as), fb.header.name, fb.bytes_allocated);
                }
                app_unload(&fb);
            }
        }
        if (g_fake_violation) {
            printf("       FAIL the install programmed flash it had not erased\n"); bad = 1;
        }
        g_fake_flash = nullptr;
    }

    app_unload(&appA);
    app_unload(&appB);
    app_pic_manifest_free(&mA);
    free(b);
    g_arena = nullptr; g_used_bytes = 0;
    return bad;
}

// --- the one relocation no real package contains ------------------------------
//
// Everything else in this file loads what the compiler actually produced, on
// purpose. This does not, and the reason is the whole of why the MOVW/MOVT check
// was missing in the first place: GCC 14.2 at -Os for Cortex-M33 materialises
// addresses through literal pools, so not one of the six packages carries a
// single R_ARM_THM_MOVW_ABS_NC. A check that walks them would have run over zero
// sites in every package, printed nothing, and passed for ever.
//
// The loader supports the pair anyway — it turns up with -mword-relocations, with
// other GCC versions, and from hand-written assembly — so the arithmetic is live
// code that nothing exercised. This builds the smallest object that does exercise
// it: three MOVW/MOVT pairs naming the three kinds of symbol whose treatment
// differs.
//
//   target_fn      a defined THUMB FUNCTION. st_value carries the Thumb bit
//                  already, so the answer is the symbol's address with the bit
//                  set ONCE. Adding it again gives function+2 with the bit
//                  CLEAR, which is the defect this whole check exists for.
//   fixture_datum  a defined DATA object. T is 0 and the address must stay even
//                  — the control, and the case that stays correct under the
//                  defect, which is what makes it worth having.
//   fw_millis      an UNDEFINED firmware symbol. Resolves to a firmware address
//                  in DIRECT mode and to a supervisor-call veneer in SVC mode,
//                  and both of those already carry the Thumb bit too.
//
// The bytes are what GAS really emits, checked against it rather than recalled:
// `movw r0, #:lower16:fn` assembles to f240 0000 and `movt r0, #:upper16:fn` to
// f2c0 0000, with the addend field zero in both.

static const uint8_t *g_membuf;
static uint32_t g_memlen;
static int mem_read(void *, uint32_t off, void *dst, uint32_t len) {
    if (off > g_memlen) return -1;
    uint32_t n = g_memlen - off;
    if (n > len) n = len;
    memcpy(dst, g_membuf + off, n);
    return (int)n;
}

// Little-endian writers, so the object is byte-identical on any host.
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define FIX_TEXT_BYTES 32u
#define FIX_DATA_BYTES 4u
#define FIX_NSECT      8
#define FIX_NSYM       5

// Returns a malloc'd object and its length.
static uint8_t *build_movw_object(uint32_t *len_out) {
    // .text: target_fn, then app_main with three MOVW/MOVT pairs and a return.
    static const uint16_t text[FIX_TEXT_BYTES / 2] = {
        0x4770, 0xbf00,                 // +0  target_fn: bx lr ; nop
        0xf240, 0x0000,                 // +4  movw r0, #0   -> target_fn
        0xf2c0, 0x0000,                 // +8  movt r0, #0
        0xf240, 0x0100,                 // +12 movw r1, #0   -> fixture_datum
        0xf2c0, 0x0100,                 // +16 movt r1, #0
        0xf240, 0x0200,                 // +20 movw r2, #0   -> fw_millis
        0xf2c0, 0x0200,                 // +24 movt r2, #0
        0x4770, 0xbf00,                 // +28 bx lr ; nop
    };
    static const char shstr[] =
        "\0.text\0.rel.text\0.data\0.rpc_app_header\0.symtab\0.strtab\0.shstrtab";
    // Offsets into shstr, in the order the names appear above.
    const uint32_t n_text = 1, n_rel = 7, n_data = 17, n_hdr = 23,
                   n_sym = 39, n_str = 47, n_shstr = 55;
    static const char strtab[] = "\0target_fn\0fixture_datum\0app_main\0fw_millis";
    const uint32_t s_target = 1, s_datum = 11, s_main = 25, s_millis = 34;

    // Six relocations: a MOVW and a MOVT for each of the three symbols.
    struct { uint32_t off, sym, type; } rels[6] = {
        { 4,  1, R_ARM_THM_MOVW_ABS_NC }, { 8,  1, R_ARM_THM_MOVT_ABS },
        { 12, 2, R_ARM_THM_MOVW_ABS_NC }, { 16, 2, R_ARM_THM_MOVT_ABS },
        { 20, 4, R_ARM_THM_MOVW_ABS_NC }, { 24, 4, R_ARM_THM_MOVT_ABS },
    };

    RpcAppHeader hdr{};
    hdr.magic = RPC_APP_MAGIC;
    hdr.api_major = RPC_API_MAJOR;
    hdr.api_minor = RPC_API_MINOR;
    snprintf(hdr.name, sizeof(hdr.name), "movwfixture");

    const uint32_t eh_sz = sizeof(Elf32_Ehdr), sh_sz = sizeof(Elf32_Shdr);
    uint32_t off = eh_sz;
    const uint32_t o_text = off;  off += FIX_TEXT_BYTES;
    const uint32_t o_rel  = off;  off += 6 * (uint32_t)sizeof(Elf32_Rel);
    const uint32_t o_data = off;  off += FIX_DATA_BYTES;
    const uint32_t o_hdr  = off;  off += (uint32_t)sizeof(RpcAppHeader);
    const uint32_t o_sym  = off;  off += FIX_NSYM * (uint32_t)sizeof(Elf32_Sym);
    const uint32_t o_str  = off;  off += (uint32_t)sizeof(strtab);
    const uint32_t o_shs  = off;  off += (uint32_t)sizeof(shstr);
    off = (off + 3u) & ~3u;
    const uint32_t o_shdr = off;  off += FIX_NSECT * sh_sz;

    uint8_t *b = (uint8_t *)calloc(off, 1);
    if (!b) return nullptr;

    memcpy(b, "\x7f" "ELF", 4);
    b[4] = 1;                       // ELFCLASS32
    b[5] = 1;                       // little-endian
    b[6] = 1;                       // EV_CURRENT
    put16(b + 16, ET_REL);
    put16(b + 18, EM_ARM);
    put32(b + 20, 1);               // e_version
    put32(b + 32, o_shdr);          // e_shoff
    put16(b + 40, (uint32_t)eh_sz); // e_ehsize
    put16(b + 46, (uint32_t)sh_sz); // e_shentsize
    put16(b + 48, FIX_NSECT);       // e_shnum
    put16(b + 50, 7);               // e_shstrndx

    for (uint32_t i = 0; i < FIX_TEXT_BYTES / 2; i++) put16(b + o_text + i * 2, text[i]);
    put32(b + o_data, 0xDEADBEEFu);
    memcpy(b + o_hdr, &hdr, sizeof(hdr));
    memcpy(b + o_str, strtab, sizeof(strtab));
    memcpy(b + o_shs, shstr, sizeof(shstr));
    for (int i = 0; i < 6; i++) {
        put32(b + o_rel + i * 8, rels[i].off);
        put32(b + o_rel + i * 8 + 4, (rels[i].sym << 8) | rels[i].type);
    }

    // The symbols. Locals first, which ELF requires and sh_info records.
    // st_value carries the Thumb bit for a function, exactly as GAS emits it —
    // that is the whole point of the fixture.
    struct { uint32_t name, value, size, info, shndx; } sy[FIX_NSYM] = {
        { 0,        0, 0, 0x00, 0 },                    // the reserved entry
        { s_target, 1, 2, 0x02, 1 },                    // LOCAL FUNC, .text+0 | T
        { s_datum,  0, 4, 0x01, 3 },                    // LOCAL OBJECT, .data+0
        { s_main,   5, 28, 0x12, 1 },                   // GLOBAL FUNC, .text+4 | T
        { s_millis, 0, 0, 0x10, SHN_UNDEF },            // GLOBAL NOTYPE, undefined
    };
    for (int i = 0; i < FIX_NSYM; i++) {
        uint8_t *p = b + o_sym + i * sizeof(Elf32_Sym);
        put32(p, sy[i].name);
        put32(p + 4, sy[i].value);
        put32(p + 8, sy[i].size);
        p[12] = (uint8_t)sy[i].info;
        p[13] = 0;
        put16(p + 14, sy[i].shndx);
    }

    struct { uint32_t name, type, flags, off, size, link, info, align, entsz; } sc[FIX_NSECT] = {
        { 0,       0 /*SHT_NULL*/, 0,                        0,      0,               0, 0, 0, 0 },
        { n_text,  SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,  o_text, FIX_TEXT_BYTES,  0, 0, 4, 0 },
        { n_rel,   SHT_REL,      0,                          o_rel,  6 * 8,           5, 1, 4, 8 },
        { n_data,  SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,      o_data, FIX_DATA_BYTES,  0, 0, 4, 0 },
        { n_hdr,   SHT_PROGBITS, SHF_ALLOC,                  o_hdr,  (uint32_t)sizeof(RpcAppHeader), 0, 0, 4, 0 },
        { n_sym,   SHT_SYMTAB,   0,                          o_sym,  FIX_NSYM * (uint32_t)sizeof(Elf32_Sym), 6, 3, 4, (uint32_t)sizeof(Elf32_Sym) },
        { n_str,   SHT_STRTAB,   0,                          o_str,  (uint32_t)sizeof(strtab), 0, 0, 1, 0 },
        { n_shstr, SHT_STRTAB,   0,                          o_shs,  (uint32_t)sizeof(shstr),  0, 0, 1, 0 },
    };
    for (int i = 0; i < FIX_NSECT; i++) {
        uint8_t *p = b + o_shdr + i * sh_sz;
        put32(p,      sc[i].name);
        put32(p + 4,  sc[i].type);
        put32(p + 8,  sc[i].flags);
        put32(p + 12, 0);                 // sh_addr
        put32(p + 16, sc[i].off);
        put32(p + 20, sc[i].size);
        put32(p + 24, sc[i].link);
        put32(p + 28, sc[i].info);
        put32(p + 32, sc[i].align);
        put32(p + 36, sc[i].entsz);
    }

    *len_out = off;
    return b;
}

// Load the fixture and read the addresses back. Run in both veneer modes,
// because the two resolve an undefined symbol completely differently and the
// Thumb bit has to survive either route.
static int check_movw_fixture(void) {
    int bad = 0;
    for (int svc = 0; svc < 2; svc++) {
        const char *how = svc ? "movw fixture (sandboxed)" : "movw fixture";
        uint32_t len = 0;
        uint8_t *obj = build_movw_object(&len);
        if (!obj) { printf("  FAIL %s: could not build the object\n", how); return 1; }

        g_arena = nullptr; g_used_bytes = 0; g_arena_cap = 0;
        loader_set_allocator(alloc32, free32);
        loader_set_veneer_mode(svc ? LOADER_VENEER_SVC : LOADER_VENEER_DIRECT);
        g_membuf = obj; g_memlen = len;
        AppSource src{}; src.read = mem_read; src.ctx = nullptr; src.size = len;
        LoadedApp app{};
        LoadResult rc = app_load(src, &app);
        loader_set_veneer_mode(LOADER_VENEER_DIRECT);
        if (rc != LOAD_OK) {
            printf("  FAIL %s: the object did not load: %s%s%s\n", how,
                   load_result_str(rc), app.detail[0] ? " - " : "", app.detail);
            free(obj); g_membuf = nullptr;
            g_arena = nullptr; g_used_bytes = 0;
            return 1;
        }

        // Its own copy of the bytes, since the mirror takes ownership.
        uint8_t *copy = (uint8_t *)malloc(len);
        ElfMirror m;
        if (!copy) { printf("  FAIL %s: out of memory\n", how); free(obj); return 1; }
        memcpy(copy, obj, len);
        if (!mirror_from_bytes(copy, (long)len, app, &m)) {
            printf("  FAIL %s: the object could not be mirrored back\n", how);
            free(obj); g_membuf = nullptr;
            g_arena = nullptr; g_used_bytes = 0;
            return 1;
        }
        const Shadow sm = shadow_of(app);
        int pairs = 0;
        bad |= check_movw_movt(m, app, sm, how, svc != 0, &pairs);
        // THREE, and not "at least one". This object is built right here, so the
        // number is known exactly — and a walk that quietly found fewer sites
        // than the object contains is the failure this whole file is about.
        if (pairs != 3) {
            printf("  FAIL %s: read back %d MOVW/MOVT pair(s), the object has 3\n",
                   how, pairs);
            bad = 1;
        }
        g_movw_fixture += pairs;
        if (!bad)
            printf("  ok   %-12s %d MOVW/MOVT pair(s): a function, a datum and a "
                   "firmware call, every address rebuilt from the two halfwords\n",
                   svc ? "movw/svc" : "movw", pairs);
        mirror_close(&m);
        free(obj);
        g_membuf = nullptr;
        g_arena = nullptr; g_used_bytes = 0;
    }
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

// --- the inputs, named rather than skipped (task #101) -----------------------
//
// This suite is only as good as the artifacts it reads, and it used to be
// entirely silent about not having them. A directory was chosen on the presence
// of greet.app ALONE, and every other name that was not in it printed SKIP and
// counted as a pass — so a tree that had built one package, or built five of
// six, or held a directory left over from an older layout, ran a fraction of
// the checks and reported success. In a tree that had never run build.sh the
// only complaint was one line at the very end.
//
// Both are the same mistake in miniature as #103 itself: something silently did
// not happen, and nothing said so. So: a directory qualifies only if it holds
// EVERY package, an incomplete one is named along with the first file it is
// missing, and there is no path through this file where an artifact is absent
// and the answer is a pass.
static bool dir_has(const char *dir, const char *name) {
    char p[96];
    snprintf(p, sizeof(p), "%s/%s.app", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
// The first package this directory does NOT hold, or null if it holds them all.
static const char *missing_from(const char *dir) {
    for (const char *n : kNames) if (!dir_has(dir, n)) return n;
    return nullptr;
}

int main(int argc, char **argv) {
    static char paths[8][64];
    static const char *kApps[8];
    int napps = 0;

    // Complete directories only. A partial one is remembered separately, because
    // "half a build" is worth naming and an empty tree is merely expected.
    const char *dir = nullptr;
    const char *partial = nullptr, *partial_missing = nullptr;
    for (const char *d : kBuildDirs) {
        const char *miss = missing_from(d);
        if (!miss) { dir = d; break; }
        if (!partial)
            for (const char *n : kNames)
                if (dir_has(d, n)) { partial = d; partial_missing = miss; break; }
    }
    if (dir) {
        printf("  from %s\n", dir);
        for (const char *n : kNames) {
            snprintf(paths[napps], sizeof(paths[0]), "%s/%s.app", dir, n);
            kApps[napps] = paths[napps];
            napps++;
        }
    } else if (argc == 1) {
        // Nothing to run against, so say which artifact is missing and how to
        // produce it, rather than failing later inside the loader and reading
        // like a loader bug.
        if (partial)
            printf("  FAIL %s has some packages but not %s.app.\n"
                   "       That is a partial or stale build directory, and running against\n"
                   "       it would check a fraction of what this suite covers and still\n"
                   "       print a pass. Run ./build.sh from the repository root.\n",
                   partial, partial_missing);
        else
            printf("  FAIL no built packages anywhere: looked for %s.app in %s\n"
                   "       and %d other place(s), and found none of them.\n"
                   "       Run ./build.sh from the repository root (or ./build.sh pico2_w\n"
                   "       for one board), and run this from os/host.\n",
                   kNames[0], kBuildDirs[0],
                   (int)(sizeof(kBuildDirs) / sizeof(kBuildDirs[0])) - 1);
        printf("  realapp: 0 loaded, 1 failed\n");
        return 1;
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
        // `dir` was chosen because it holds EVERY package, so this file is there
        // by construction. It used to be an `if` with no else, which meant the
        // whole install-order regression below could stop running because of a
        // missing file and say nothing at all.
        if (!probe_f) {
            fails += missing_artifact(path);
        } else {
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
                // The measurement this whole block is built on. It used to fall
                // through here in silence, so a package that stopped loading
                // took the install-order regression with it and the run still
                // printed a pass.
                fclose(g_f);
                printf("     FAIL the reference image did not load, so nothing "
                       "about the install order was checked\n");
                fails++;
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
        printf("  FAIL loaded no apps at all — run ./build.sh from the repository "
               "root, and run this from os/host\n");
        fails++;
    }
    // And nothing on the slot path is a failure of the same kind. Everything
    // #103 turned out to be lived there, and a package reaches it only by being
    // position-independent — so if the last PIC opt-in were ever removed, every
    // slot check in this file would stop running and the suite would go on
    // saying "ok". Only assert it for the full run: a single named .app may
    // legitimately be a package nobody made PIC.
    if (argc == 1 && g_pic_checked == 0) {
        printf("  FAIL no position-independent package was checked, so the whole\n"
               "       flash-slot path ran none of its checks. Something has to be\n"
               "       opted in — see rpc_add_app(... PIC) in os/CMakeLists.txt.\n");
        fails++;
    }
    // And that what it ran on was worth running on.
    //
    // "At least one PIC package" was the old bar, and it was cleared for months
    // by greet: 304 bytes, three branches, no GOT worth the name. Every slot-path
    // assertion — r9 against the GOT base, the three gates being fetchable, each
    // slot landing in executable memory — was exercised by that and nothing else,
    // while the package the path exists FOR is 123 KB with two thousand GOT slots.
    // A count of one cannot tell those apart; these numbers can.
    //
    // The floors are far below what novad1 actually produces (2328 / 3066 / 133 KB)
    // and far above anything a stub can reach, so they say "a real package went
    // through here" without being a figure that has to be revised whenever one
    // changes size.
    if (argc == 1 && dir) {
        printf("  slot-path cover: %d GOT + %d branch + %d ABS32 site(s), biggest "
               "blob %u B\n", g_slot_got, g_slot_branch, g_slot_abs, g_slot_biggest_blob);
        if (g_slot_got < 500 || g_slot_branch < 500 || g_slot_biggest_blob < 32u * 1024u) {
            printf("  FAIL the flash-slot path ran, but only over something small: "
                   "%d GOT and %d branch site(s) in a %u B blob.\n"
                   "       That is the cover greet gave it while every real package\n"
                   "       took the copy path — enough to say the code ran, not enough\n"
                   "       to say it works. Keep a substantial package opted in with\n"
                   "       rpc_add_app(... PIC) in os/CMakeLists.txt.\n",
                   g_slot_got, g_slot_branch, g_slot_biggest_blob);
            fails++;
        }
    }

    // The relocation no package currently contains. Run whatever else did or did
    // not happen above, and unconditionally — it needs no build directory, and
    // the whole reason it exists is that the check reading MOVW/MOVT back would
    // otherwise walk zero sites and pass.
    printf("  the MOVW/MOVT pair\n");
    fails += check_movw_fixture();
    printf("       %d pair(s) in the real packages, %d in the fixture%s\n",
           g_movw_real, g_movw_fixture,
           g_movw_real ? "" : " — GCC at -Os builds addresses through literal "
                              "pools, so the packages carry none and the fixture "
                              "is the only thing exercising this");
    // A fixture that stopped producing sites would leave the check reading
    // nothing while still printing a pass, which is the exact instrument this
    // file exists to be the opposite of.
    if (g_movw_fixture == 0) {
        printf("  FAIL the MOVW/MOVT check read back no pairs at all — nothing "
               "exercised it\n");
        fails++;
    }
    // The copy path used to check no branch at all. Printed so a change that
    // stopped it walking them would show as a number falling to zero rather
    // than as one fewer silent line.
    if (argc == 1 && dir && g_branches == 0) {
        printf("  FAIL not one branch was checked by value on the copy path\n");
        fails++;
    }
    printf("  realapp: %d loaded (%d through a flash slot), %d branch(es) checked "
           "by value on the copy paths, %d failed\n",
           g_loaded, g_pic_checked, g_branches, fails);
    return fails ? 1 : 0;
}
