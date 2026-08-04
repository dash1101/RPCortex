#include "blockcache.h"

#include <string.h>

void bc_init(BlockCache *c, uint8_t *buf, uint32_t block_size, uint32_t sector_size,
             void *ctx,
             bool (*read_block)(void *, uint32_t, void *),
             bool (*write_block)(void *, uint32_t, const void *)) {
    c->buf = buf;
    c->block_size = block_size;
    c->sector_size = sector_size;
    c->block = BC_INVALID;
    c->dirty = false;
    c->ctx = ctx;
    c->read_block = read_block;
    c->write_block = write_block;
}

bool bc_flush(BlockCache *c) {
    if (c->block == BC_INVALID || !c->dirty) return true;
    bool ok = c->write_block(c->ctx, c->block, c->buf);
    // Cleared either way. A write that failed will not succeed by being retried
    // from the same buffer at some arbitrary later moment, and leaving it dirty
    // means every future flush retries it and fails again.
    c->dirty = false;
    return ok;
}

static bool load(BlockCache *c, uint32_t block) {
    if (c->block == block) return true;
    if (!bc_flush(c)) return false;
    if (!c->read_block(c->ctx, block, c->buf)) { c->block = BC_INVALID; return false; }
    c->block = block;
    return true;
}

bool bc_read_sector(BlockCache *c, uint32_t lba, void *dst) {
    uint32_t per = c->block_size / c->sector_size;
    uint32_t block = lba / per;

    // The held block answers for its own sectors even when it has not reached
    // flash. Reading around it would hand back the bytes that were there before
    // the caller's own write — which is not a stale read so much as a write
    // that silently did not happen.
    if (block == c->block) {
        memcpy(dst, c->buf + (lba % per) * c->sector_size, c->sector_size);
        return true;
    }
    // Not held: read the block in rather than reading the sector directly, so
    // there is one path to flash and one place for it to be wrong.
    if (!load(c, block)) return false;
    memcpy(dst, c->buf + (lba % per) * c->sector_size, c->sector_size);
    return true;
}

bool bc_write_sector(BlockCache *c, uint32_t lba, const void *src) {
    uint32_t per = c->block_size / c->sector_size;
    if (!load(c, lba / per)) return false;
    memcpy(c->buf + (lba % per) * c->sector_size, src, c->sector_size);
    c->dirty = true;
    return true;
}

void bc_discard(BlockCache *c) {
    c->dirty = false;
    c->block = BC_INVALID;
}
