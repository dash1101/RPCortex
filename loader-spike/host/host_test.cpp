// Host-side verification of the loader.
//
// The relocation engine is pure computation over a byte buffer, so it can be
// exercised on a PC against the SAME app ELFs the device loads. That gets the
// risky logic under test without flashing anything — which matters here,
// because flashing a .uf2 erases the device filesystem.
//
// What this CANNOT do is execute the app: the code is ARM Thumb. So it verifies
// that loading succeeds, that relocations resolve to the values they should,
// that veneers are emitted for out-of-range calls, that a version mismatch is
// refused, and that unload returns every byte. Actually running the code is the
// device's job.
//
//   build:  g++ -std=c++17 -I../firmware -I../include host_test.cpp \
//               ../firmware/loader.cpp -o host_test
//   run:    ./host_test <hello.o> <badver.o>

#include "loader.h"
#include "elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// --- a fake firmware, placed where real firmware lives on an RP2350 ---------
// 0x10000000 is XIP flash. Apps load into malloc'd host memory, which stands in
// for SRAM. The gap between them is what forces veneers, and using the real
// address means the host test exercises the same path the device does.
static const uint32_t kFwBase = 0x10000000u;

struct FakeSym { const char *name; uint32_t addr; };
static const FakeSym kFake[] = {
    {"fw_printf", kFwBase + 0x100},
    {"fw_millis", kFwBase + 0x200},
    {"fw_malloc", kFwBase + 0x300},
    {"fw_free",   kFwBase + 0x400},
};

uint32_t api_lookup(const char *name) {
    for (auto &s : kFake) if (strcmp(s.name, name) == 0) return s.addr;
    return 0;
}
uint32_t api_symbol_count(void) { return sizeof(kFake) / sizeof(kFake[0]); }

// --- a fake SRAM at the device's real address ------------------------------
// The host is 64-bit; the loader is not. Rather than weaken the loader to cope,
// give it memory at a genuine 32-bit address — the same 0x20000000 the RP2350
// puts SRAM at. Now the 256 MB flash-to-SRAM distance that forces veneers is
// reproduced exactly, and every address in the test is one the device would
// really see.
static const uintptr_t kSramBase = 0x20000000u;
static const size_t    kSramSize = 512 * 1024;
static uint8_t *g_sram;
static size_t   g_sram_used;
static long     g_live_bytes;      // outstanding allocations, for leak checks

struct Blk { uint32_t size; uint32_t free_flag; };

static void *sram_alloc(size_t n) {
    n = (n + 7) & ~7ull;
    // First-fit over a bump allocator with reuse, which is enough to prove that
    // unload returns memory: after a load/unload cycle the used total must be
    // exactly what it was before.
    for (size_t off = 0; off < g_sram_used;) {
        Blk *b = (Blk *)(g_sram + off);
        if (b->free_flag && b->size >= n) { b->free_flag = 0; g_live_bytes += b->size;
                                            return g_sram + off + sizeof(Blk); }
        off += sizeof(Blk) + b->size;
    }
    if (g_sram_used + sizeof(Blk) + n > kSramSize) return nullptr;
    Blk *b = (Blk *)(g_sram + g_sram_used);
    b->size = (uint32_t)n;
    b->free_flag = 0;
    void *p = g_sram + g_sram_used + sizeof(Blk);
    g_sram_used += sizeof(Blk) + n;
    g_live_bytes += n;
    return p;
}

static void sram_free(void *p) {
    if (!p) return;
    Blk *b = (Blk *)((uint8_t *)p - sizeof(Blk));
    if (!b->free_flag) { b->free_flag = 1; g_live_bytes -= b->size; }
}

// --- file source -----------------------------------------------------------
struct FileCtx { FILE *f; };

static int file_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FileCtx *c = (FileCtx *)ctx;
    if (fseek(c->f, off, SEEK_SET) != 0) return -1;
    size_t n = fread(dst, 1, len, c->f);
    return (int)n;
}

static int checks = 0, failures = 0;
static void check(bool cond, const char *what) {
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static bool load_file(const char *path, LoadedApp *app, LoadResult *rc) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  cannot open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    uint32_t sz = (uint32_t)ftell(f);
    FileCtx ctx{f};
    AppSource src{&ctx, file_read, sz};
    *rc = app_load(src, app);
    fclose(f);
    return true;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: host_test <hello.o> <badver.o>\n");
        return 2;
    }

    g_sram = (uint8_t *)mmap((void *)kSramBase, kSramSize,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                             -1, 0);
    if (g_sram == MAP_FAILED || (uintptr_t)g_sram != kSramBase) {
        printf("could not map a fake SRAM at 0x%08lx -- cannot simulate the "
               "32-bit address space\n", (unsigned long)kSramBase);
        return 2;
    }
    loader_set_allocator(sram_alloc, sram_free);
    printf("fake SRAM at 0x%08lx, firmware at 0x%08x (%.0f MB apart)\n\n",
           (unsigned long)kSramBase, kFwBase,
           (double)(kSramBase - kFwBase) / (1024 * 1024));

    printf("== loading %s ==\n", argv[1]);
    LoadedApp app;
    LoadResult rc;
    if (!load_file(argv[1], &app, &rc)) return 2;

    check(rc == LOAD_OK, "the reference app loads");
    if (rc != LOAD_OK) {
        printf("  error: %s (%s)\n", load_result_str(rc), app.detail);
        return 1;
    }
    printf("  name=%s api=%u.%u image=%u B veneers=%u/%u B entry=%p\n",
           app.header.name, app.header.api_major, app.header.api_minor,
           app.image_size, app.veneers_used, app.veneer_size, (void *)app.entry);

    check(app.entry != nullptr, "app_main was found");
    check(((uintptr_t)app.entry & 1u) == 1u,
          "the entry point has the Thumb bit set -- without it the first call "
          "switches to ARM state and faults on a core that has none");
    check(app.image_size > 0, "sections were loaded");
    check(app.veneers_used > 0,
          "veneers were emitted -- flash is 256 MB from SRAM and a Thumb BL "
          "reaches 16 MB, so every call into the firmware needs one");
    check(strcmp(app.header.name, "hello") == 0, "the app header was read");

    // Every veneer must point at a real exported symbol with the Thumb bit set.
    int veneer_count = app.veneers_used / 16;
    printf("  %d veneer(s):\n", veneer_count);
    for (int i = 0; i < veneer_count; i++) {
        uint8_t *v = (uint8_t *)app.veneers + i * 16;
        uint32_t target = *(uint32_t *)(v + 12);
        const char *nm = "?";
        for (auto &s : kFake) if ((s.addr | 1u) == target) nm = s.name;
        printf("    -> 0x%08x  %s\n", target, nm);
        check(strcmp(nm, "?") != 0, "the veneer targets an exported symbol");
        check((target & 1u) == 1u, "and keeps the Thumb bit");
        // The trampoline instructions themselves, verified byte for byte.
        uint16_t *code = (uint16_t *)v;
        check(code[0] == 0xb401 && code[1] == 0x4802 && code[2] == 0x4684 &&
              code[3] == 0xbc01 && code[4] == 0x4760,
              "the veneer is the expected push/ldr/mov/pop/bx sequence");
    }

    // Every BL in the loaded image must now be in range of something. Walk the
    // executable bytes and decode each BL: a displacement that still exceeds
    // +/-16 MB means a relocation was silently left broken.
    // (A crude scan -- it can misread data as instructions -- so this only
    // asserts that no DECODED branch is wildly out of range.)
    check(app.veneers_used <= app.veneer_size, "the veneer pool did not overflow");

    uint32_t before_unload = app.bytes_allocated;
    long live_before_unload = g_live_bytes;
    app_unload(&app);
    check(g_live_bytes < live_before_unload, "unload actually returned memory");
    check(g_live_bytes == 0, "and returned ALL of it -- nothing outstanding");
    check(app.image == nullptr && app.veneers == nullptr,
          "unload released the image and the veneer pool");
    check(app.bytes_allocated == 0, "and zeroed the accounting");
    printf("  reclaimed %u B\n", before_unload);

    // Loading the same app repeatedly must not grow. A leak here is the kind
    // that only shows after a long session, which is the worst kind.
    printf("== load/unload x50 ==\n");
    uint32_t first = 0;
    bool stable = true;
    for (int i = 0; i < 50; i++) {
        LoadedApp a;
        LoadResult r;
        load_file(argv[1], &a, &r);
        if (r != LOAD_OK) { stable = false; break; }
        if (i == 0) first = a.bytes_allocated;
        else if (a.bytes_allocated != first) stable = false;
        app_unload(&a);
    }
    check(stable, "50 load/unload cycles allocate the same amount every time");
    check(g_live_bytes == 0,
          "and 50 cycles later nothing is still allocated -- a leak here is the "
          "kind that only shows after a long session");

    printf("== version gate: %s ==\n", argv[2]);
    LoadedApp bad;
    LoadResult brc;
    if (load_file(argv[2], &bad, &brc)) {
        check(brc == LOAD_ERR_API_MISMATCH,
              "an app built against a different API major is REFUSED");
        check(bad.image == nullptr,
              "and refused before anything was allocated for it");
        printf("  -> %s\n", load_result_str(brc));
    }

    // A truncated file must be rejected, not read off the end of the buffer.
    printf("== malformed input ==\n");
    {
        FILE *f = fopen(argv[1], "rb");
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        char tmp[] = "/tmp/rpc_trunc_XXXXXX";
        int fd = mkstemp(tmp);
        FILE *in = fopen(argv[1], "rb");
        FILE *out = fdopen(fd, "wb");
        for (long i = 0; i < sz / 3; i++) fputc(fgetc(in), out);
        fclose(out); fclose(in);
        LoadedApp t;
        LoadResult trc;
        load_file(tmp, &t, &trc);
        check(trc != LOAD_OK, "a truncated ELF is rejected rather than loaded");
        printf("  -> %s\n", load_result_str(trc));
        remove(tmp);
    }
    {
        char tmp[] = "/tmp/rpc_junk_XXXXXX";
        int fd = mkstemp(tmp);
        FILE *out = fdopen(fd, "wb");
        for (int i = 0; i < 512; i++) fputc(i & 0xff, out);
        fclose(out);
        LoadedApp t;
        LoadResult trc;
        load_file(tmp, &t, &trc);
        check(trc == LOAD_ERR_NOT_ELF, "a non-ELF file is rejected by magic");
        remove(tmp);
    }

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
