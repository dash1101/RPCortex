// The FAT12 transfer area, checked against itself and against fsck.fat.
//
// This is a real filesystem rather than a view of one, which changes what has
// to be proved. A synthesised view only had to be readable; this one is written
// to by a host that will then rely on what it wrote, so the round trip matters:
// a file written here has to come back byte for byte, a deleted one has to
// release its clusters, and the twelve-bit table has to survive entries that
// straddle a byte boundary and entries that straddle a sector boundary.
//
// The twelve-bit packing is the part with no prior coverage anywhere in this
// tree. Everything before it was FAT16, whose entries are two whole bytes; here
// they are a byte and a half, half of them split across a boundary, and an
// off-by-one nibble produces a volume that reads perfectly until it is nearly
// full. So the table is exercised directly as well as through files.
#include "../core/fat12.h"
#include "../core/fatview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *w) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", w); fails++; }
}

// A one-megabyte volume in memory, matching the region on the device.
#define SECTORS 2048
static uint8_t disk[SECTORS * F12_SECTOR];

static bool d_read(void *, uint32_t lba, void *buf) {
    if (lba >= SECTORS) return false;
    memcpy(buf, disk + (size_t)lba * F12_SECTOR, F12_SECTOR);
    return true;
}
static bool d_write(void *, uint32_t lba, const void *buf) {
    if (lba >= SECTORS) return false;
    memcpy(disk + (size_t)lba * F12_SECTOR, buf, F12_SECTOR);
    return true;
}

// A file's contents as a repeatable pattern, so a wrong sector is obvious.
struct Pattern { uint32_t seed; uint32_t size; };
static uint8_t pat_byte(uint32_t seed, uint32_t off) {
    return (uint8_t)((off * 31u + seed * 17u + (off >> 8)) & 0xFF);
}
static uint32_t pat_source(void *ctx, uint32_t off, void *buf, uint32_t len) {
    Pattern *p = (Pattern *)ctx;
    uint8_t *b = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) b[i] = pat_byte(p->seed, off + i);
    return len;
}

static int listed;
static char last_name[F12_MAXNAME];
static void count_cb(void *, const F12Entry *e) {
    listed++;
    snprintf(last_name, sizeof(last_name), "%s", e->name);
}

static bool roundtrip(F12 *f, const char *name, uint32_t seed, uint32_t size) {
    Pattern p{seed, size};
    if (!f12_write(f, name, size, 1785801600u, pat_source, &p)) return false;

    F12Entry e;
    if (!f12_find(f, name, &e)) return false;
    if (e.size != size) return false;

    // Read back in awkward chunks, so a boundary that only works when it is
    // also a sector boundary does not pass.
    static uint8_t got[210000];
    if (size > sizeof(got)) return false;
    uint32_t off = 0;
    const uint32_t steps[] = {1, 511, 512, 513, 1000};
    int si = 0;
    while (off < size) {
        uint32_t want = steps[si++ % 5];
        if (want > size - off) want = size - off;
        uint32_t n = f12_read(f, &e, off, got + off, want);
        if (n != want) return false;
        off += n;
    }
    for (uint32_t i = 0; i < size; i++)
        if (got[i] != pat_byte(seed, i)) return false;
    return true;
}

int main(void) {
    printf("fat12_test - the USB transfer area\n");

    F12Io io{nullptr, d_read, d_write, SECTORS};
    F12 f;

    memset(disk, 0xFF, sizeof(disk));    // erased flash, which is what it starts as
    ck(f12_format(&f, &io, "RPCORTEX"), "a one megabyte region formats");
    ck(f.clusters > 0 && f.clusters < 4085,
       "and lands in the FAT12 range -- 4085 or more would be FAT16, and every "
       "entry would then be read at the wrong bit offset");

    // Mounting what was just written has to agree with it.
    F12 g;
    ck(f12_mount(&g, &io), "the volume mounts back");
    ck(g.clusters == f.clusters && g.first_data == f.first_data,
       "with the same geometry it was formatted with");

    listed = 0;
    ck(f12_list(&g, count_cb, nullptr) && listed == 0, "and is empty");

    // --- files --------------------------------------------------------------
    ck(roundtrip(&f, "hello.txt", 1, 11), "a tiny file survives a round trip");
    ck(roundtrip(&f, "one.bin", 2, 512), "one whose length is exactly a sector");
    ck(roundtrip(&f, "two.bin", 3, 513), "and one a byte past it");
    ck(roundtrip(&f, "big.bin", 4, 40000),
       "a file spanning many clusters, read back in chunks that do not line up "
       "with sectors");

    // A name 8.3 cannot hold. This is the case that made repo.json unusable in
    // the synthesised view, so it is worth proving in the real one.
    ck(roundtrip(&f, "readings-2026.json", 5, 3000),
       "a long name survives, and the file it names reads back");
    {
        F12Entry e;
        ck(f12_find(&f, "readings-2026.json", &e), "and is found by its long name");
        ck(f12_find(&f, "READIN~1.JSO", &e) || true, "");
        checks--;    // the generated 8.3 form is not something to pin down
    }

    listed = 0;
    f12_list(&f, count_cb, nullptr);
    ck(listed == 5, "all five files are listed");

    // --- the twelve-bit table -----------------------------------------------
    //
    // The part with no prior coverage anywhere in this tree: entries are a byte
    // and a half, so every other one straddles a byte boundary, and one in
    // every 341 straddles a SECTOR boundary too. A nibble swapped, or a
    // straddling entry read from one sector instead of two, produces a volume
    // that is perfect until a file happens to land on the wrong cluster.
    //
    // A chain long enough to cross both kinds of boundary many times is what
    // proves it, and reading the file back byte for byte is what makes a wrong
    // link visible rather than merely possible.
    ck(roundtrip(&f, "chain.bin", 21, 400 * 512),
       "a four hundred cluster chain reads back exactly, so no table entry is "
       "packed into the wrong nibble or split across the wrong sector");

    // Many separate files, so allocation lands on both odd and even clusters
    // and every chain has to terminate in the right half of a shared byte.
    {
        bool all = true;
        char nm[32];
        for (int i = 0; i < 24 && all; i++) {
            snprintf(nm, sizeof(nm), "f%02d.dat", i);
            all = roundtrip(&f, nm, (uint32_t)(100 + i), (uint32_t)(300 + i * 37));
        }
        ck(all, "two dozen small files each keep their own contents");
        listed = 0;
        f12_list(&f, count_cb, nullptr);
        ck(listed == 5 + 1 + 24, "and every one of them is in the listing");

        for (int i = 0; i < 24; i++) {
            snprintf(nm, sizeof(nm), "f%02d.dat", i);
            f12_remove(&f, nm);
        }
        f12_remove(&f, "chain.bin");
        listed = 0;
        f12_list(&f, count_cb, nullptr);
        ck(listed == 5, "and removing them all leaves the original five");
    }

    // Replacing a file must not leave two entries with one name.
    ck(roundtrip(&f, "hello.txt", 9, 40), "a file can be replaced");
    listed = 0;
    f12_list(&f, count_cb, nullptr);
    ck(listed == 5, "and replacing does not duplicate the entry");

    // --- deletion releases space --------------------------------------------
    {
        uint32_t before = f12_free_bytes(&f);
        ck(f12_remove(&f, "big.bin"), "a file can be deleted");
        uint32_t after = f12_free_bytes(&f);
        ck(after > before + 39000,
           "and its clusters go back to the free list rather than leaking");
        F12Entry e;
        ck(!f12_find(&f, "big.bin", &e), "it is gone from the directory");
        listed = 0;
        f12_list(&f, count_cb, nullptr);
        ck(listed == 4, "and from the listing");

        // The space has to be reusable, not merely counted as free.
        ck(roundtrip(&f, "again.bin", 7, 39000),
           "and the freed space can be used again");
    }

    // --- filling it up ------------------------------------------------------
    //
    // Running out has to fail cleanly. A partial write that leaves a directory
    // entry pointing at clusters it never got is a file the host can see and
    // cannot read.
    {
        uint32_t free_now = f12_free_bytes(&f);
        Pattern p{11, free_now + 100000};
        ck(!f12_write(&f, "toobig.bin", free_now + 100000, 0, pat_source, &p),
           "a file larger than the volume is refused");
        F12Entry e;
        ck(!f12_find(&f, "toobig.bin", &e),
           "and leaves no entry behind pointing at clusters it never wrote");
        ck(f12_free_bytes(&f) == free_now,
           "and no clusters leaked from the attempt");
    }

    // --- the outside opinion ------------------------------------------------
    {
        char tmpl[] = "/tmp/rpcortex-fat12-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) { printf("  FAIL: no temporary file\n"); return 1; }
        FILE *out = fdopen(fd, "wb");
        fwrite(disk, 1, sizeof(disk), out);
        fclose(out);

        const char *fsck = nullptr;
        if (system("test -x /sbin/fsck.fat") == 0)          fsck = "/sbin/fsck.fat";
        else if (system("test -x /usr/sbin/fsck.fat") == 0) fsck = "/usr/sbin/fsck.fat";
        else if (system("command -v fsck.fat >/dev/null 2>&1") == 0) fsck = "fsck.fat";

        if (!fsck) {
            printf("  NOTE: fsck.fat not installed -- the volume was built and\n"
                   "        exercised, but nothing outside this code read it.\n");
        } else {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "%s -n -v %s > %s.log 2>&1", fsck, tmpl, tmpl);
            bool clean = system(cmd) == 0;
            ck(clean, "fsck.fat reads the volume and finds nothing wrong");

            // It has to have read it as FAT12. A wrong cluster count would make
            // it read every entry at the wrong bit offset -- and not
            // necessarily complain, since a FAT12 reading of a FAT16 table is
            // not self-evidently broken.
            snprintf(cmd, sizeof(cmd), "grep -q '12 bit entries' %s.log", tmpl);
            ck(system(cmd) == 0, "as FAT12, which is what the cluster count makes it");

            if (!clean) {
                printf("  --- fsck.fat said ---\n");
                snprintf(cmd, sizeof(cmd), "sed 's/^/    /' %s.log", tmpl);
                if (system(cmd) != 0) printf("    (could not read the log)\n");
            }
            snprintf(cmd, sizeof(cmd), "rm -f %s.log", tmpl);
            if (system(cmd) != 0) { /* not worth failing over */ }
        }
        remove(tmpl);
    }

    // --- a region holding something else ------------------------------------
    //
    // The area shares flash with the firmware staging slot, so after an update
    // it holds part of an image. That has to read as "not a volume" rather than
    // as a volume with impossible geometry.
    {
        memset(disk, 0xA5, sizeof(disk));
        F12 h;
        ck(!f12_mount(&h, &io), "a region holding something else does not mount");
        memset(disk, 0x00, sizeof(disk));
        ck(!f12_mount(&h, &io), "and neither does an erased one");
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
