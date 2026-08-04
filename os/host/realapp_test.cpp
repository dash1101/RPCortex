// Load every REAL built .app through the real loader.
//
// The other loader tests use synthetic objects, which only ever contain what the
// test author thought to put in them. This one loads what the compiler actually
// produced — with -ffunction-sections giving dozens of sections, .bss for each
// static, string literals in merge sections, and whatever relocation types GCC
// felt like emitting. That is the input the device gets.
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
    if (g_used_bytes + n > ARENA) return nullptr;
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
static char g_seen[64][40];
static uint32_t g_addr[64];
static int g_ns;
static int api_slot(const char *n) {
    for (int i = 0; i < g_ns; i++) if (!strcmp(g_seen[i], n)) return i;
    if (g_ns >= 64) return -1;
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
uint32_t api_symbol_count(void) { return 64; }

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
    if (e < b || e >= b + app.image_size) {
        printf("       FAIL entry point is outside the image\n");
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
    if (app.text_size % APP_BLOCK_ALIGN || app.image_size % APP_BLOCK_ALIGN) {
        printf("       FAIL a half is not a whole number of blocks (%u / %u)\n",
               app.text_size, app.image_size);
        bad = 1;
    }
    if (app.text_size + app.data_size != app.image_size) {
        printf("       FAIL the halves do not add up to the image\n");
        bad = 1;
    }
    if (app.data && (uintptr_t)app.data != b + app.text_size) {
        printf("       FAIL the writable half does not start where the other ends\n");
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
    uint32_t *words = (uint32_t *)app.image;
    int checked = 0, even_ptrs = 0;
    for (uint32_t w = 0; w < app.image_size / 4; w++) {
        uint32_t v = words[w];
        if (v < b || v >= b + app.image_size) continue;   // not a pointer into us
        uint32_t off = (v & ~1u) - b;
        // Only judge values that point into an executable section. Data pointers
        // are legitimately even, so counting them would be noise.
        if (!in_text(path, off)) continue;
        checked++;
        if (!(v & 1u)) {
            printf("       FAIL word at +0x%x holds %08x -> code at +0x%x with NO Thumb bit\n",
                   w * 4, v, off);
            even_ptrs++;
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
    if (app.veneer_gates != 32) {
        printf("       FAIL sandboxed: the gates are %u bytes, not 32\n", app.veneer_gates);
        bad = 1;
    }
    if (h[0] != 0xf382 || h[1] != 0x8814) {
        printf("       FAIL sandboxed: the return gate does not start with msr CONTROL, r2\n");
        bad = 1;
    }
    if (h[2] != 0xf3bf || h[3] != 0x8f6f || h[4] != 0x4718) {
        printf("       FAIL sandboxed: the return gate is not isb then bx r3\n");
        bad = 1;
    }
    if (((const uint16_t *)(v + 16))[0] != 0xdf01) {
        printf("       FAIL sandboxed: the exit gate is not svc #1\n");
        bad = 1;
    }
    // The gates carry a literal that is not a usable index, so that even a
    // reuse scan starting in the wrong place cannot hand one out as the veneer
    // for a real function.
    if (*(const uint32_t *)(v + 12) != 0xFFFFFFFFu ||
        *(const uint32_t *)(v + 16 + 12) != 0xFFFFFFFFu) {
        printf("       FAIL sandboxed: a gate carries a literal that could pass "
               "for an ABI index\n");
        bad = 1;
    }
    if (app_return_gate(&app) != ((uint32_t)(uintptr_t)v | 1u) ||
        app_exit_gate(&app)   != (((uint32_t)(uintptr_t)v + 16) | 1u)) {
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
           name, 2, calls);
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
static const char *kNames[] = { "greet", "bench", "stress", "tuidemo" };

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
    if (argc > 1) {
        for (int i = 1; i < argc; i++) fails += load_one(argv[i]);
    } else {
        for (int i = 0; i < napps; i++) fails += load_one(kApps[i]);
        // And every one of them again as a sandboxed package, which produces a
        // completely different veneer pool from the same file.
        for (int i = 0; i < napps; i++) fails += load_svc(kApps[i]);
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
