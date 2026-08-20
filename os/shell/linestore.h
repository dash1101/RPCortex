// A paged text buffer for the editor.
//
// The editor used to hold its text as one flat rectangle — ED_MAX_LINES rows of
// ED_MAX_COL bytes, malloc'd in a single 80,000-byte block. That was fine on a
// bare board, but on one with Nova D1 resident the largest free block is only
// 41-52 KB, so the one allocation always failed and `edit` refused to open on
// exactly the device it is most wanted on. The memory was there; it was just
// never in one piece.
//
// So the text is paged. The lines are still fixed width and still shift as whole
// rows — nothing a v1 user can see has changed — but they are grouped into small
// chunks allocated only as the file grows into them. The largest single
// allocation is now one chunk (LS_CHUNK_BYTES, 6.25 KB) instead of the whole
// buffer, and a five-line file allocates one chunk rather than reserving four
// hundred lines it will never use. Each chunk is its own malloc, so allocating a
// new one never moves an old one: a char* into a line stays valid across the
// growth of a later line, which the editor relies on when it splits a row.
//
// THE INVARIANT the editor holds, so that no read has to check for null: every
// index in [0, count) has its chunk allocated. Only two operations grow count —
// loading a file and pressing Enter — and both already have a refusal path for a
// file that will not fit. Allocation failure is routed through those same
// refusals (see ls_line returning null at the growth edge), so an out-of-memory
// midway through a huge file reads as "too big to hold", never as a crash.
#ifndef RPC_LINESTORE_H
#define RPC_LINESTORE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The editor's user-visible limits. A line too long is cut, a file too tall is
// truncated — both were true before paging and are kept, because v1's editor
// enforced the same ceilings and people have files that assume them.
#define ED_MAX_LINES 400
#define ED_MAX_COL   200

// Chunk geometry. Thirty-two lines to a chunk keeps the largest allocation at
// 6,400 bytes — comfortably inside the smallest free block a Nova-D1 board
// leaves — while a chunk still covers more than a screenful, so a normal edit
// touches one or two of them, not thirteen.
#define LS_LINES_PER_CHUNK 32
#define LS_CHUNKS      ((ED_MAX_LINES + LS_LINES_PER_CHUNK - 1) / LS_LINES_PER_CHUNK)
#define LS_CHUNK_BYTES (LS_LINES_PER_CHUNK * ED_MAX_COL)

// The shape guard. If a later change collapses the paging back into one flat
// buffer — the obvious way being to make a chunk hold every line — this stops
// compiling rather than shipping the 80 KB allocation that could not open. It is
// the compile-time half of the runtime max_alloc assertion the host test makes.
#if defined(__cplusplus) && __cplusplus >= 201103L
static_assert(LS_CHUNK_BYTES <= 8 * 1024,
              "a line-store chunk must stay small enough to allocate on a busy heap");
#endif

struct LineStore {
    char    *chunk[LS_CHUNKS];   // null until a line inside it is first needed
    uint32_t max_alloc;          // largest single allocation ever made, for the shape assertion
};

static inline void ls_init(LineStore *ls) {
    memset(ls, 0, sizeof(*ls));
}

static inline void ls_free(LineStore *ls) {
    for (int c = 0; c < LS_CHUNKS; c++) {
        free(ls->chunk[c]);
        ls->chunk[c] = 0;
    }
    ls->max_alloc = 0;
}

// The one accessor. Returns a pointer to line i's ED_MAX_COL-byte buffer,
// allocating (and zeroing) the containing chunk on first touch.
//
// Returns null only for an out-of-range index or a chunk that could not be
// allocated. Per the invariant above, a caller reading an index below count
// never sees null — the chunk was allocated when count grew past it. The null is
// reachable only at the growth edge, where the two callers that grow count check
// for it and refuse.
static inline char *ls_line(LineStore *ls, int i) {
    if (i < 0 || i >= ED_MAX_LINES) return 0;
    int c = i / LS_LINES_PER_CHUNK;
    if (!ls->chunk[c]) {
        char *p = (char *)malloc(LS_CHUNK_BYTES);
        if (!p) return 0;
        memset(p, 0, LS_CHUNK_BYTES);
        ls->chunk[c] = p;
        if ((uint32_t)LS_CHUNK_BYTES > ls->max_alloc) ls->max_alloc = LS_CHUNK_BYTES;
    }
    return ls->chunk[c] + (size_t)(i % LS_LINES_PER_CHUNK) * ED_MAX_COL;
}

#endif  // RPC_LINESTORE_H
