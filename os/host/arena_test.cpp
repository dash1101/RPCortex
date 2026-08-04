// A package's own heap.
//
// It matters that this is dull and correct rather than clever: it is the only
// memory a sandboxed package can allocate from, a package cannot reach the OS's
// heap to fall back on, and a corrupted header here is a fault inside the
// package with no explanation attached.
#include "../core/arena.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *w) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", w); fails++; }
}

static uint8_t backing[2048];

int main(void) {
    printf("arena_test - a package's own heap\n");
    Arena a;

    arena_init(&a, backing, sizeof(backing));
    ck(arena_used(&a) == 0, "a fresh arena has nothing in it");
    ck(arena_largest(&a) > 1900, "and nearly all of it available");

    void *p = arena_alloc(&a, 100);
    ck(p != nullptr, "a small request is served");
    ck(((uintptr_t)p & 7u) == 0, "eight-byte aligned, which a double will assume");
    ck(arena_used(&a) >= 100, "and accounted for");

    // Writing the whole block must stay inside the backing store. If the header
    // arithmetic is wrong this is where it shows, and ASan is what says so.
    memset(p, 0xA5, 100);
    ck(((uint8_t *)p) + 100 <= backing + sizeof(backing), "the block is inside the arena");

    void *q = arena_alloc(&a, 200);
    ck(q != nullptr && q != p, "a second request gets different memory");
    ck((uint8_t *)q >= (uint8_t *)p + 100, "which does not overlap the first");

    uint32_t used = arena_used(&a);
    arena_free(&a, p);
    ck(arena_used(&a) < used, "freeing gives it back");

    // Alloc/free in a loop is what a package actually does, and an allocator
    // that fragments under it fails after a while rather than at once — which
    // is the hardest kind of failure to attribute.
    bool all = true;
    for (int i = 0; i < 500; i++) {
        void *t = arena_alloc(&a, 64);
        if (!t) { all = false; break; }
        arena_free(&a, t);
    }
    ck(all, "five hundred rounds of allocate-and-free do not exhaust it");

    arena_free(&a, q);
    ck(arena_used(&a) == 0, "and everything comes back at the end");
    ck(arena_largest(&a) > 1900, "with the arena whole again, not in pieces");

    // --- what it refuses ----------------------------------------------------
    ck(arena_alloc(&a, 0) == nullptr, "a zero-byte request is not an allocation");
    ck(arena_alloc(&a, 100000) == nullptr, "and one bigger than the arena is refused");
    ck(arena_alloc(&a, 0xFFFFFFF0u) == nullptr,
       "including one whose rounding would wrap");

    // A package can pass fw_free anything at all. None of these may write.
    void *keep = arena_alloc(&a, 64);
    uint32_t before = arena_used(&a);
    arena_free(&a, nullptr);
    arena_free(&a, backing - 64);                 // before the arena
    arena_free(&a, backing + sizeof(backing) + 8);// past it
    arena_free(&a, (uint8_t *)keep + 3);          // inside, but not a block
    ck(arena_used(&a) == before,
       "a pointer that is not a block this arena handed out is ignored");
    arena_free(&a, keep);
    arena_free(&a, keep);
    ck(arena_used(&a) == 0, "and freeing the same block twice does not merge it away");

    // An arena too small to hold anything must say so rather than pretend.
    Arena tiny;
    arena_init(&tiny, backing, 4);
    ck(arena_alloc(&tiny, 1) == nullptr, "an arena with no room serves nothing");
    arena_init(&tiny, nullptr, 1024);
    ck(arena_alloc(&tiny, 1) == nullptr, "and neither does one with no memory");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
