// The write-back block cache the USB drive writes flash through.
//
// Small, and worth testing for the same reason the rest of the drive is: none
// of its failures announce themselves. A read that misses the unflushed copy
// returns the bytes that were there BEFORE the caller's own write, which looks
// like a write that silently did not happen; a block dropped without being
// flushed loses data the host was told was safe. Both surface as filesystem
// corruption somewhere else entirely.
//
// The counters matter as much as the contents: the whole reason this exists is
// that a naive implementation erases the same block eight times for one
// kilobyte, and a test that only checked correctness would pass on the naive
// version.
#include "../core/blockcache.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *w) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", w); fails++; }
}

#define BLOCK  4096
#define SECTOR 512
#define BLOCKS 8

static uint8_t flash[BLOCKS * BLOCK];
static int reads, writes;

static bool rd(void *, uint32_t block, void *dst) {
    if (block >= BLOCKS) return false;
    reads++;
    memcpy(dst, flash + (size_t)block * BLOCK, BLOCK);
    return true;
}
static bool wr(void *, uint32_t block, const void *src) {
    if (block >= BLOCKS) return false;
    writes++;
    memcpy(flash + (size_t)block * BLOCK, src, BLOCK);
    return true;
}

static uint8_t buf[BLOCK];
static BlockCache c;

static void fill(uint8_t *p, uint8_t v) { memset(p, v, SECTOR); }

int main(void) {
    printf("blockcache_test - one erase block, held between writes\n");

    memset(flash, 0xFF, sizeof(flash));
    bc_init(&c, buf, BLOCK, SECTOR, nullptr, rd, wr);

    uint8_t s[SECTOR], back[SECTOR];

    // --- a write is visible before it reaches flash -------------------------
    fill(s, 0xA1);
    ck(bc_write_sector(&c, 0, s), "a sector can be written");
    ck(writes == 0, "and nothing has reached flash yet, which is the point");
    ck(bc_read_sector(&c, 0, back) && memcmp(back, s, SECTOR) == 0,
       "reading it back gives what was written, not what flash still holds");
    ck(flash[0] == 0xFF, "flash really has not been touched");

    ck(bc_flush(&c), "the block flushes");
    ck(writes == 1, "as exactly one erase block");
    ck(flash[0] == 0xA1, "and flash now holds it");

    // --- a run of sectors in one block costs one write ----------------------
    writes = 0;
    for (uint32_t i = 0; i < 8; i++) { fill(s, (uint8_t)(0xB0 + i)); bc_write_sector(&c, i, s); }
    ck(writes == 0, "eight sectors of one block do not write eight times");
    bc_flush(&c);
    ck(writes == 1, "they write once");
    for (uint32_t i = 0; i < 8; i++)
        if (flash[i * SECTOR] != (uint8_t)(0xB0 + i)) { ck(false, "and all eight landed"); break; }
    ck(flash[7 * SECTOR] == 0xB7, "including the last one");

    // --- moving to another block flushes the first --------------------------
    writes = 0;
    fill(s, 0xC1);
    bc_write_sector(&c, 3, s);          // block 0
    ck(writes == 0, "a write to the held block does not flush");
    fill(s, 0xC2);
    bc_write_sector(&c, 8, s);          // block 1
    ck(writes == 1, "moving to another block flushes the one being left");
    ck(flash[3 * SECTOR] == 0xC1, "and what it held is in flash");
    bc_flush(&c);
    ck(flash[8 * SECTOR] == 0xC2, "then the new one follows");

    // --- reading a sector outside the held block ----------------------------
    //
    // The held block must be written out first. Reading around a dirty block
    // and leaving it held would be correct here and lose the data the moment
    // anything else claimed the cache.
    writes = 0;
    fill(s, 0xD1);
    bc_write_sector(&c, 16, s);         // block 2, now dirty
    ck(bc_read_sector(&c, 0, back), "a read outside the held block works");
    ck(writes == 1, "and flushes the dirty block rather than abandoning it");
    ck(flash[16 * SECTOR] == 0xD1, "so its contents survived the eviction");
    ck(back[0] == 0xB0, "while the read returned the right sector");

    // --- discard drops without writing --------------------------------------
    //
    // For when the region underneath is about to be overwritten wholesale. A
    // flush there would drop stale bytes into the middle of the new contents.
    writes = 0;
    fill(s, 0xE1);
    bc_write_sector(&c, 24, s);
    uint8_t was = flash[24 * SECTOR];
    bc_discard(&c);
    ck(writes == 0, "discard writes nothing");
    ck(flash[24 * SECTOR] == was, "and leaves flash as it was");
    ck(bc_flush(&c), "a later flush is harmless");
    ck(writes == 0, "and still writes nothing, because nothing is held");

    // --- a failed write does not retry itself forever -----------------------
    {
        BlockCache b;
        static uint8_t bb[BLOCK];
        bc_init(&b, bb, BLOCK, SECTOR, nullptr, rd,
                [](void *, uint32_t, const void *) { return false; });
        fill(s, 0xF1);
        bc_write_sector(&b, 0, s);
        ck(!bc_flush(&b), "a failing write is reported");
        ck(bc_flush(&b), "and is not retried from the same buffer forever");
    }

    // --- every sector of a block is addressed distinctly --------------------
    //
    // An offset computed from the LBA instead of from its position within the
    // block would put every sector at the start of the buffer, and a whole
    // block would read back as eight copies of its last sector.
    {
        memset(flash, 0, sizeof(flash));
        bc_init(&c, buf, BLOCK, SECTOR, nullptr, rd, wr);
        for (uint32_t i = 0; i < BLOCK / SECTOR; i++) { fill(s, (uint8_t)(1 + i)); bc_write_sector(&c, i, s); }
        bc_flush(&c);
        bool distinct = true;
        for (uint32_t i = 0; i < BLOCK / SECTOR; i++)
            if (flash[i * SECTOR] != (uint8_t)(1 + i)) distinct = false;
        ck(distinct, "each sector of a block lands at its own offset");
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
