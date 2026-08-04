// A heap for one sandboxed package. See arena.cpp.
#ifndef RPC_ARENA_H
#define RPC_ARENA_H

#include <stdint.h>

struct Arena {
    uint8_t *base;
    uint32_t size;      // zero means unusable, and every call then does nothing
};

void arena_init(Arena *a, void *base, uint32_t size);

// Null when the arena is full, which a package must be able to survive — it is
// the same answer malloc gives the OS, and for the same reason.
void *arena_alloc(Arena *a, uint32_t n);

// Ignores anything that is not a block this arena handed out. A package can
// pass any pointer at all to fw_free, and following one blindly would write a
// header over whatever it pointed at.
void arena_free(Arena *a, void *p);

uint32_t arena_used(const Arena *a);
// How big it is at all — so a request that could never have fitted can be told
// apart from one that merely arrived too late.
uint32_t arena_size(const Arena *a);
uint32_t arena_largest(const Arena *a);

#endif  // RPC_ARENA_H
