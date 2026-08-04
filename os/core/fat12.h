// A small FAT12 volume, the real thing rather than a view of something else.
//
// This replaces synthesising a filesystem over littlefs, and the difference is
// the whole reason it exists. A synthesised view has to work out what a raw
// sector write MEANT — a new file, a rename, a deletion, an edit — from bytes
// that carry no such intent, and it can only ever recognise the cases it was
// taught. Everything else has to be refused, which is why edits did not stick,
// why the two sides drifted apart, and why the write path had to reach into
// littlefs from inside the USB stack and deadlocked against the console.
//
// A real volume has none of those problems. The host mounts it, owns it, and
// creates, edits, renames and deletes in it exactly as it would on a memory
// card, because nothing is interpreting anything: a sector write is a sector
// write. What the device needs in exchange is the ability to read the format
// back, which is this file.
//
// FAT12 rather than FAT16 because the area is one megabyte. The boundary is on
// the number of clusters and it is exact — under 4085 the volume IS FAT12,
// whatever any field claims — so a small volume has no choice, and the earlier
// code's trick of padding up over the line is not available at this size.
//
// Everything here goes through a sector interface, so it runs against flash on
// the device and against a buffer in a host test, and the host test can hand
// the result to fsck.fat for an opinion that is not this code's own.
#ifndef RPC_FAT12_H
#define RPC_FAT12_H

#include <stdint.h>

#define F12_SECTOR   512
#define F12_MAXNAME  64

// How the volume is reached. Sectors, because that is what both a flash region
// and a USB host deal in.
struct F12Io {
    void    *ctx;
    // Both return false on failure; a false from read is treated as fatal,
    // since a volume that cannot be read cannot be reasoned about.
    bool   (*read)(void *ctx, uint32_t lba, void *buf);
    bool   (*write)(void *ctx, uint32_t lba, const void *buf);
    uint32_t sectors;
};

struct F12 {
    F12Io    io;
    uint16_t reserved;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint8_t  spc;              // sectors per cluster
    uint32_t fat_sectors;
    uint32_t first_fat;
    uint32_t first_root;
    uint32_t first_data;
    uint32_t clusters;         // data clusters, which is what makes it FAT12
    bool     mounted;
};

// Lay down a fresh, empty volume. Everything already there is lost.
bool f12_format(F12 *f, const F12Io *io, const char *label);

// Read an existing volume's parameters. False when what is there is not a FAT
// volume this can work with — which on a region that has held something else
// is the normal case, and the caller's cue to format.
bool f12_mount(F12 *f, const F12Io *io);

struct F12Entry {
    char     name[F12_MAXNAME];   // the long name where there is one, else 8.3
    uint32_t size;
    uint32_t mtime;               // Unix epoch, 0 when the volume carried none
    uint16_t first_cluster;
    bool     is_dir;
};

// Walk the root directory. Long names are reassembled; subdirectories are
// reported but not descended, because a transfer area does not need a tree and
// every level of one is somewhere for files to hide.
typedef void (*F12WalkFn)(void *ctx, const F12Entry *e);
bool f12_list(F12 *f, F12WalkFn cb, void *ctx);

// Find one entry by name, matching the long and short forms, case-insensitively
// — which is what the format itself does.
bool f12_find(F12 *f, const char *name, F12Entry *out);

// Read part of a file. Returns bytes read, which is short at the end.
uint32_t f12_read(F12 *f, const F12Entry *e, uint32_t off, void *buf, uint32_t len);

// Create or replace a file, from a callback that supplies the bytes.
//
// A callback rather than a buffer because the device streams these out of
// littlefs and holding a whole one in RAM is exactly what it cannot afford.
typedef uint32_t (*F12SourceFn)(void *ctx, uint32_t off, void *buf, uint32_t len);
bool f12_write(F12 *f, const char *name, uint32_t size, uint32_t mtime,
               F12SourceFn src, void *ctx);

bool f12_remove(F12 *f, const char *name);

// Space, for a caller that wants to check before starting a long copy.
uint32_t f12_free_bytes(F12 *f);
uint32_t f12_total_bytes(F12 *f);

#endif
