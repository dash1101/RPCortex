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
    }
    n = (n + 7) & ~(size_t)7;
    if (g_used_bytes + n > ARENA) return nullptr;
    void *r = g_arena + g_used_bytes;
    g_used_bytes += n;
    return r;
}
static void free32(void *) { }      // one-shot; the arena goes away with us

static FILE *g_f;
static int file_read(void *, uint32_t off, void *dst, uint32_t len) {
    if (fseek(g_f, off, SEEK_SET) != 0) return -1;
    return (int)fread(dst, 1, len, g_f);
}
// A fake firmware symbol table: every fw_* resolves to a plausible address.
uint32_t api_lookup(const char *n) {
    static uint32_t next = 0x10001000;
    static char seen[64][40]; static uint32_t addr[64]; static int ns;
    for (int i = 0; i < ns; i++) if (!strcmp(seen[i], n)) return addr[i];
    if (ns < 64) { snprintf(seen[ns], 40, "%s", n); addr[ns] = next; next += 4; ns++; return addr[ns-1]; }
    return next;
}
uint32_t api_symbol_count(void) { return 64; }

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
    fclose(g_f);
    return bad;
}

int main(int argc, char **argv) {
    static const char *kApps[] = {
        "../build/apps/greet.app",
        "../build/apps/bench.app",
        "../build/apps/stress.app",
    };
    int fails = 0;
    if (argc > 1) {
        for (int i = 1; i < argc; i++) fails += load_one(argv[i]);
    } else {
        for (const char *a : kApps) fails += load_one(a);
    }
    printf("  realapp: %d failed\n", fails);
    return fails ? 1 : 0;
}
