// One erase block, held in memory between writes.
//
// Flash erases in 4 KB blocks and a USB host writes in 512-byte sectors, so
// writing each sector straight through would erase and rewrite the same block
// eight times for one contiguous kilobyte — slow, and eight times the wear for
// no reason. Holding the block and pushing it out when the host moves on turns
// that back into one operation.
//
// Pure, and separate from the drive that uses it, because the interesting
// behaviour is not obvious and none of it fails loudly: a read that misses the
// unflushed copy hands back stale bytes, and a block that is dropped without
// being flushed loses a write the host was told had succeeded. Both look like
// filesystem corruption several steps later.
#ifndef RPC_BLOCKCACHE_H
#define RPC_BLOCKCACHE_H

#include <stdint.h>

#define BC_INVALID 0xFFFFFFFFu

struct BlockCache {
    uint8_t  *buf;          // block_size bytes, supplied by the caller
    uint32_t  block_size;
    uint32_t  sector_size;
    uint32_t  block;        // which block is held, or BC_INVALID
    bool      dirty;

    void     *ctx;
    // Whole-block read and write. The write is an erase followed by a program
    // on a device; here it is just "make flash match this buffer".
    bool    (*read_block)(void *ctx, uint32_t block, void *dst);
    bool    (*write_block)(void *ctx, uint32_t block, const void *src);
};

void bc_init(BlockCache *c, uint8_t *buf, uint32_t block_size, uint32_t sector_size,
             void *ctx,
             bool (*read_block)(void *, uint32_t, void *),
             bool (*write_block)(void *, uint32_t, const void *));

// Push the held block out if it has changed. Safe to call at any time.
bool bc_flush(BlockCache *c);

// Read one sector. Answers from the held block when it covers that sector, so a
// write that has not reached flash is still visible to whoever wrote it.
bool bc_read_sector(BlockCache *c, uint32_t lba, void *dst);

// Write one sector into the held block, loading and flushing as the block
// changes.
bool bc_write_sector(BlockCache *c, uint32_t lba, const void *src);

// Forget what is held WITHOUT writing it back. For when the region underneath
// is about to be overwritten by something else entirely — an update staging a
// firmware image over it — and flushing would put stale bytes in the middle of
// the new contents.
void bc_discard(BlockCache *c);

#endif
