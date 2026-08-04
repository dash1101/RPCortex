// A heap for one sandboxed package.
//
// A package that cannot reach the OS's memory cannot be handed a pointer into
// the OS's heap, so fw_malloc has to give it something out of a block of its
// own — one block, because there is exactly one protection region to describe
// it with.
//
// This is a first-fit allocator over an implicit list: every block carries an
// eight-byte header, free blocks merge with the one after them, and there is no
// list threading to corrupt. Small and dull on purpose. The interesting
// property is not speed but that it can be exhausted safely: a package that
// asks for more than its arena holds gets null, which is a package that stops
// working rather than a device that does.
#include "arena.h"

#include <string.h>

// Header: the block's total size including this header, and whether it is free.
// Eight bytes so payloads stay eight-aligned, which is what AAPCS wants and
// what a double inside a package will assume.
struct Block {
    uint32_t size;
    uint32_t free;
};

#define HDR  ((uint32_t)sizeof(Block))

static inline Block *first(const Arena *a) { return (Block *)a->base; }
static inline Block *next(const Arena *a, Block *b) {
    uint8_t *n = (uint8_t *)b + b->size;
    return n < a->base + a->size ? (Block *)n : nullptr;
}

void arena_init(Arena *a, void *base, uint32_t size) {
    if (!a) return;
    a->base = (uint8_t *)base;
    a->size = 0;
    if (!base || size < HDR + 8) return;      // too small to hold anything
    a->size = size & ~7u;
    Block *b = first(a);
    b->size = a->size;
    b->free = 1;
}

void *arena_alloc(Arena *a, uint32_t n) {
    if (!a || !a->size || !n) return nullptr;
    uint32_t want = (n + HDR + 7u) & ~7u;
    if (want < n) return nullptr;             // the rounding wrapped

    for (Block *b = first(a); b; b = next(a, b)) {
        if (!b->free || b->size < want) continue;
        // Split, but only if what is left could hold anything at all. A
        // remainder smaller than a header is not a block, and treating it as
        // one puts a header past the end of the arena.
        if (b->size >= want + HDR + 8) {
            Block *rest = (Block *)((uint8_t *)b + want);
            rest->size = b->size - want;
            rest->free = 1;
            b->size = want;
        }
        b->free = 0;
        return (uint8_t *)b + HDR;
    }
    return nullptr;
}

void arena_free(Arena *a, void *p) {
    if (!a || !p || !a->size) return;
    uint8_t *q = (uint8_t *)p - HDR;
    // Refuse anything that is not a block boundary in THIS arena. A package can
    // pass any pointer it likes to fw_free, and following one that is not a
    // block header would write a size and a flag over whatever it points at.
    if (q < a->base || q >= a->base + a->size) return;
    for (Block *b = first(a); b; b = next(a, b)) {
        if ((uint8_t *)b != q) continue;
        if (b->free) return;                  // freed twice: ignore, do not merge
        b->free = 1;
        // Merge forward as far as it goes. Backward merging would need a second
        // pass or a footer; forward alone keeps a repeating alloc/free cycle
        // from fragmenting, which is what a package actually does.
        for (Block *n = next(a, b); n && n->free; n = next(a, b)) b->size += n->size;
        return;
    }
}

uint32_t arena_size(const Arena *a) { return a ? a->size : 0; }

uint32_t arena_used(const Arena *a) {
    if (!a || !a->size) return 0;
    uint32_t used = 0;
    for (Block *b = first(a); b; b = next(a, b)) if (!b->free) used += b->size;
    return used;
}

uint32_t arena_largest(const Arena *a) {
    if (!a || !a->size) return 0;
    uint32_t best = 0;
    for (Block *b = first(a); b; b = next(a, b))
        if (b->free && b->size > best) best = b->size;
    return best > HDR ? best - HDR : 0;
}
