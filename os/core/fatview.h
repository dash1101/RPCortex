// A FAT16 volume synthesised over something that is not FAT.
//
// littlefs is not FAT and never will be, so a host that mounts this device's
// flash raw sees an unformatted drive. What it can be shown instead is a FAT16
// volume computed on demand: a boot sector built from nothing, a file
// allocation table derived from a listing, directory entries built from names
// and sizes, and file data read through littlefs when the host asks for the
// cluster it was told about.
//
// Everything in this header is arithmetic and byte layout — no littlefs, no
// flash, no USB. That is deliberate: the subtle bugs in a synthesised
// filesystem are all in the geometry and the encoding, and those are exactly
// the parts a host test can cover. The glue that walks the real filesystem
// lives in usbmsc.cpp.
#ifndef RPC_FATVIEW_H
#define RPC_FATVIEW_H

#include <stdint.h>

#define FAT_SECTOR_SIZE 512
#define FAT_DIRENT_SIZE 32

// Directory entry attributes, from the FAT specification.
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20

// FAT16 cluster values with a meaning of their own.
#define FAT16_FREE  0x0000
#define FAT16_BAD   0xFFF7
#define FAT16_EOC   0xFFFF   // any value >= 0xFFF8 ends a chain

// The first cluster that can hold data. 0 and 1 are reserved by the format, so
// the data area starts at cluster 2 and the arithmetic has to keep saying so.
#define FAT_FIRST_DATA_CLUSTER 2

// What makes a volume FAT16 rather than FAT12.
//
// The distinction is not a field in the boot sector — it is computed by the
// host from the number of data clusters, and the boundary is exact: fewer than
// 4085 and the volume IS FAT12, whatever anything else says. Getting this wrong
// does not produce an error; it produces a host that reads every FAT entry at
// the wrong bit offset and sees garbage.
//
// This device lands uncomfortably close to it. A Pico 2 W has 2 MB of
// filesystem, which at one 512-byte sector per cluster is 4096 sectors, and
// after the boot sector, the table and the root directory that leaves about
// 4047 data clusters — just under the line. So the volume is padded up to a
// safe count and the clusters that do not exist are marked bad, which every
// host understands as "never allocate here" and excludes from free space.
//
// 4096 rather than 4085: a few clusters of margin costs 20 KB of apparent bad
// space and removes any dependence on a host implementing the boundary with the
// same comparison the specification uses.
#define FAT16_MIN_DATA_CLUSTERS 4096

struct FatGeom {
    uint32_t total_sectors;      // what the volume claims to be
    uint16_t reserved_sectors;   // the boot sector, and nothing else
    uint8_t  sectors_per_cluster;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint32_t fat_sectors;        // per copy of the table
    uint32_t root_sectors;
    uint32_t first_fat_sector;
    uint32_t first_root_sector;
    uint32_t first_data_sector;  // where cluster 2 begins
    uint32_t data_clusters;      // total, including any that are padding
    uint32_t real_clusters;      // how many are backed by actual storage
};

// Build a geometry that describes `real_bytes` of usable storage.
//
// The result may claim to be larger than what was asked for — see
// FAT16_MIN_DATA_CLUSTERS. Clusters from real_clusters upward are padding and
// must be reported bad, which fat_fat_sector does.
//
// Returns false only if the request is too small to describe at all.
bool fat_geom_init(FatGeom *g, uint32_t real_bytes);

enum FatRegion {
    FAT_RGN_BOOT,
    FAT_RGN_FAT,
    FAT_RGN_ROOT,
    FAT_RGN_DATA,
    FAT_RGN_BEYOND,   // past the end of the volume
};

// Which part of the volume an LBA falls in.
//
// `index` is filled with the offset within that region: the sector within the
// table for FAT_RGN_FAT, the sector within the root directory for FAT_RGN_ROOT,
// and the CLUSTER NUMBER for FAT_RGN_DATA — cluster, not sector, because that
// is what every caller actually wants. `sector_in_cluster` gets the rest.
FatRegion fat_region(const FatGeom *g, uint32_t lba,
                     uint32_t *index, uint32_t *sector_in_cluster);

// The boot sector, including the 0x55AA signature.
void fat_build_boot(const FatGeom *g, uint8_t sec[FAT_SECTOR_SIZE],
                    const char *label, uint32_t volume_id);

// One node of the synthesised tree: a file or a directory, occupying a
// contiguous run of clusters.
//
// Contiguous is the simplification that makes this tractable. Nothing here has
// to model fragmentation, because nothing here is a real allocator — the layout
// is computed fresh from the listing every time the volume is presented, so
// every file can be laid out end to end.
struct FatNode {
    uint32_t first_cluster;
    uint32_t clusters;       // how many it occupies; at least 1 for a directory
    uint32_t size;           // bytes, for a file; 0 for a directory, as FAT requires
    uint32_t mtime;          // Unix epoch; 0 if unknown
    uint16_t parent;         // index into the node table, or FAT_NO_PARENT for root
    uint8_t  is_dir;
    // Which halves of the name were lower case before 8.3 shouted them.
    //
    // The stored name is upper case because the format says so, and a host
    // renders exactly what it is given — which is why every file appears in
    // capitals. Two bits in the directory entry, set by Windows NT and honoured
    // by everything since, say "display this half lower case". It costs one
    // byte and turns CA.PEM back into ca.pem.
    //
    // It cannot express mixed case; a name like ReadMe stays shouted, because
    // the alternative is showing a name that is not the file's.
    uint8_t  case_flags;
    char     name[11];       // 8.3, space padded, no dot — the on-disk form

    // The real name, when 8.3 cannot hold it. Borrowed, not owned: it has to
    // outlive the node, which for the caller is the same table.
    //
    // Eight-point-three is not merely shouty, it is lossy in a way that breaks
    // files. Three characters of extension turns repo.json into REPO.JSO, and a
    // file copied off under that name is not the file. The format's answer is a
    // run of extra directory entries carrying the name in UTF-16 ahead of the
    // real one, which every host has understood since Windows 95.
    const char *lname;
};

// The case bits, as they sit in byte 12 of a directory entry.
#define FAT_CASE_BASE_LOWER 0x08
#define FAT_CASE_EXT_LOWER  0x10

#define FAT_NO_PARENT 0xFFFF

// One sector of the file allocation table.
//
// Chains are contiguous, so an entry points at its successor until the last
// cluster of a node, which ends the chain. Clusters that belong to no node are
// free; clusters past the real capacity are bad.
void fat_fat_sector(const FatGeom *g, uint32_t fat_sector,
                    const FatNode *nodes, uint32_t node_count,
                    uint8_t sec[FAT_SECTOR_SIZE]);

// Encode one 32-byte directory entry.
void fat_dirent(uint8_t e[FAT_DIRENT_SIZE], const char name[11], uint8_t attr,
                uint32_t first_cluster, uint32_t size, uint32_t mtime,
                uint8_t case_flags);

// Which node owns a cluster, or -1 for none.
int32_t fat_node_for_cluster(const FatNode *nodes, uint32_t count, uint32_t cluster);

// How many directory entries a node needs: the 8.3 one, plus any long-name
// entries in front of it.
//
// Thirteen characters per long-name entry, and none at all when the 8.3 form
// reproduces the name exactly — which is decided by reconstructing it and
// comparing, rather than by a rule about lengths that would have to stay in
// step with fat_shortname.
uint32_t fat_entries_for(const FatNode *n);

// Whether the 8.3 form, with its case bits applied, IS the name.
bool fat_name_fits_83(const char *name);

// Give every node a contiguous run of clusters.
//
// Node 0 must be the root, which occupies no cluster: FAT16 gives the root
// directory a fixed region outside the data area. A directory's size comes from
// how many children it has, a file's from its own; both are read from the table
// rather than passed in, so this is the one place the layout is decided.
//
// A node that does not fit in `max_clusters` is left with none, which presents
// it as empty rather than overlapping someone else's data. Returns the number
// of clusters used.
uint32_t fat_layout(FatNode *nodes, uint32_t count, uint32_t max_clusters);

// Fill one 512-byte sector of a directory's entries.
//
// Node 0 is the root, whose entries live in the volume's fixed root region and
// begin with the volume label. Every other directory occupies clusters and
// begins with "." and "..". `first_entry` counts from the start of that
// directory's entries either way.
//
// This lives here rather than beside the filesystem walk because it is pure
// byte layout over the node table, and because a whole volume can then be
// synthesised on the host and handed to a real FAT implementation to check.
void fat_dir_sector(const FatNode *nodes, uint32_t count, uint32_t dir_idx,
                    uint32_t first_entry, const char *label,
                    uint8_t sec[FAT_SECTOR_SIZE]);

// Encode the volume label entry, which lives in the root directory and is the
// only entry allowed to carry FAT_ATTR_VOLUME_ID.
void fat_dirent_label(uint8_t e[FAT_DIRENT_SIZE], const char *label);

// Encode the "." and ".." entries every subdirectory must begin with.
//
// A directory without them is one Windows treats as damaged. `parent_cluster`
// is 0 when the parent is the root, which is what the format requires even
// though the root is not at cluster 0.
void fat_dirent_dot(uint8_t e[FAT_DIRENT_SIZE], bool dotdot,
                    uint32_t self_cluster, uint32_t parent_cluster,
                    uint32_t mtime);

// Convert a filesystem name to the 8.3 form stored on disk.
//
// Returns false when the name cannot be represented — too long, or reduced to
// nothing by the character rules. A caller that wants such files visible has to
// generate a name rather than pass one through, because there is no encoding of
// "keep the long name" that does not involve long filename entries.
bool fat_shortname(const char *name, char out[11], uint8_t *case_flags);

// Whether a name is one a desktop operating system creates for its own
// bookkeeping the moment it mounts a volume.
//
// Windows writes System Volume Information, macOS writes .Spotlight-V100,
// .fseventsd and .Trashes, and both do it within seconds. None of it was asked
// for and none of it should reach the real filesystem, so the write path
// refuses these by name. Matching is case-insensitive because the host's case
// is not something to rely on.
bool fat_is_host_metadata(const char *name);

// Pack a Unix epoch into the FAT date and time fields.
//
// FAT counts years from 1980 and stores seconds in two-second units. A
// timestamp before 1980 — including the zero this device uses for "the clock
// was never set" — has no representation, and is written as the epoch's own
// start rather than as a wrapped value that would show as some date in 2100.
void fat_time_encode(uint32_t epoch, uint16_t *date, uint16_t *time);

#endif
