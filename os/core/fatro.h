// A READ-ONLY FAT12/FAT16/FAT32 reader, for volumes this device did not create.
//
// WHY THIS IS NOT core/fat12.cpp
//
// fat12.* is a real FAT12 volume the device FORMATS and WRITES — the USB
// transfer area. It is FAT12 by arithmetic rather than by choice: the area is
// one megabyte and the format is decided by the cluster count, so nothing else
// was available at that size. Its root directory is therefore a fixed linear
// region (`root_entries`, `first_root`), its cluster numbers are uint16_t, and
// its directory scanner is shared with the write path.
//
// A memory card is none of those things. It is FAT32 unless it is small, its
// root directory is an ordinary cluster chain with no fixed region at all, and
// its cluster numbers need 28 bits. Reaching that from fat12.* would mean
// restructuring the scanner the write path depends on and widening every
// cluster field — surgery on shipped, tested code, to add paths a transfer area
// will never take. So this is a separate reader, and fat12.*, fatview.* and
// their three host tests are untouched by it.
//
// It reads. It does not write, and there is no half-written state to get wrong
// as a result: a card is a thing you take out and put in a computer, and the
// two callers that matter — the file explorer and the media player — only ever
// list and read.
//
// WHAT IT UNDERSTANDS
//
//   * A partitioned card (an MBR at LBA 0, the volume at the partition's start
//     sector) and an unpartitioned one ("superfloppy", the BPB at LBA 0).
//     Nearly every card sold is the first; nearly every hand-made test image is
//     the second, which is exactly how this gets missed until hardware.
//   * FAT12, FAT16 and FAT32, decided by the DATA CLUSTER COUNT and nothing
//     else — see fatro_mount.
//   * Long file names, reassembled from the entries that precede the 8.3 one.
//   * Subdirectories, to any depth.
//
// It refuses exFAT rather than misreading it. exFAT shares the 0x55AA
// signature and the first three bytes of a boot sector and shares nothing else,
// so a reader that does not check produces confident nonsense.
//
// Everything here goes through a sector-read callback, so it runs against a
// real card on the device and against a buffer on a host — which is the whole
// of how fatro_test can cover it with no hardware attached.
//
// Grounding: Microsoft FAT32 File System Specification (fatgen103), "FAT Type
// Determination" and the BPB/directory-entry layouts; the same document fat12.*
// was written from.
#ifndef RPC_FATRO_H
#define RPC_FATRO_H

#include <stdint.h>

#define FATRO_SECTOR   512
#define FATRO_MAXNAME  64

// How the volume is reached. One 512-byte sector at a time, by ABSOLUTE LBA on
// the card — the partition offset is applied inside this reader, so the caller
// hands over the whole device and never has to know where the volume starts.
//
// `read` returning false is how a card that has been pulled out reports itself.
// Every path here treats that as fatal for the operation in hand and unwinds
// rather than retrying, so a removal costs an error and not a hang.
struct FatRoIo {
    void    *ctx;
    bool   (*read)(void *ctx, uint32_t lba, void *buf);
};

enum {
    FATRO_NONE = 0,
    FATRO_FAT12,
    FATRO_FAT16,
    FATRO_FAT32,
};

struct FatRo {
    FatRoIo  io;
    uint32_t part_lba;        // where the volume starts; 0 when unpartitioned
    uint8_t  type;            // FATRO_FAT12 | FATRO_FAT16 | FATRO_FAT32
    uint8_t  spc;             // sectors per cluster
    uint8_t  num_fats;
    uint16_t root_entries;    // FAT12/16 only; 0 on FAT32
    uint32_t fat_sectors;     // per copy of the table
    uint32_t first_fat;       // absolute LBA
    uint32_t first_root;      // absolute LBA of the fixed root; FAT12/16 only
    uint32_t first_data;      // absolute LBA of cluster 2
    uint32_t clusters;        // data clusters — the number that IS the format
    uint32_t root_cluster;    // FAT32's root chain
    uint32_t fsinfo_lba;      // absolute LBA of FSInfo, or 0
    char     label[12];       // volume label, "" when the volume carries none
    bool     mounted;

    // One cached FAT sector. A cluster chain walk asks for the same sector over
    // and over — a 4 KB file at 512-byte clusters is eight lookups, and without
    // this that is eight SPI block reads of the same 512 bytes.
    uint32_t cache_lba;
    uint8_t  cache[FATRO_SECTOR];
    // Scratch for directory and data sectors. In the struct rather than on the
    // stack because a device task's stack is measured in single-digit KB and
    // two of these nested would be a quarter of it.
    //
    // The consequence is that a FatRo is NOT reentrant. sdvfs holds a lock
    // across every call for exactly this reason.
    uint8_t  sec[FATRO_SECTOR];
};

struct FatRoEntry {
    char     name[FATRO_MAXNAME];   // the long name where there is one, else 8.3
    uint32_t size;                  // meaningless for a directory
    uint32_t mtime;                 // Unix epoch, 0 when the volume carried none
    uint32_t first_cluster;
    bool     is_dir;
};

// Read the volume's parameters. False when what is there is not a FAT volume
// this can read — an unformatted card, exFAT, a partition table pointing at
// nothing. That is a normal answer, not an error to shout about.
bool fatro_mount(FatRo *f, const FatRoIo *io);
void fatro_unmount(FatRo *f);

// "FAT12" / "FAT16" / "FAT32", or "none".
const char *fatro_type_name(const FatRo *f);

// Capacity of the DATA AREA, in bytes. Not the card's capacity — a card is
// usually slightly larger than the volume on it.
uint64_t fatro_total_bytes(const FatRo *f);

// Free bytes, or 0 for "not known".
//
// Counting free clusters honestly means reading the whole table, which on a
// 32 GB card is four megabytes over SPI for a number nobody asked for
// precisely. FAT32 keeps a cached count in its FSInfo sector and this returns
// that when it is present and plausible; otherwise it says nothing, which
// FwStorageRoot.free_kb explicitly allows.
uint64_t fatro_free_bytes(FatRo *f);

// Walk one directory. `path` is relative to the volume root: "/" or "/dir/sub".
// '.' and '..' are skipped, the volume label entry is skipped, and nothing is
// descended — a caller that wants a tree walks it itself.
typedef void (*FatRoWalkFn)(void *ctx, const FatRoEntry *e);
bool fatro_list(FatRo *f, const char *path, FatRoWalkFn cb, void *ctx);

// One path, resolved. The root resolves to a directory entry with an empty
// name, size 0 and first_cluster set to the root chain (0 on FAT12/16, where
// the root is not a chain at all).
bool fatro_stat(FatRo *f, const char *path, FatRoEntry *out);

// Where a sequential read got to, so the next one does not start over.
//
// Same reasoning as F12Cursor, and the same bug if it is missing: a file is a
// CHAIN, so reaching byte N by walking from the start is quadratic in the
// number of sectors read. On a card that is an SPI transaction per link.
struct FatRoCursor {
    uint32_t first;      // whose chain this is; a different file resets it
    uint32_t cluster;    // the cluster containing `at`
    uint32_t at;         // the file offset that cluster begins at
};

void fatro_cursor_init(FatRoCursor *c);

// Read part of a file. Returns bytes read, short at the end and 0 past it.
// `cur` may be null; when it is not, reads that move FORWARD resume from it.
uint32_t fatro_read(FatRo *f, const FatRoEntry *e, FatRoCursor *cur,
                    uint32_t off, void *buf, uint32_t len);

#endif  // RPC_FATRO_H
