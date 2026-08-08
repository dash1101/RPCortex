// The read-only FAT12/16/32 reader. See fatro.h for why it is not fat12.cpp.
//
// Field offsets and the type rule come from the Microsoft FAT32 File System
// Specification (fatgen103) — "Boot Sector and BPB Structure", "FAT Type
// Determination", "FAT Directory Structure" and "FAT Long Directory Entries".
// The MBR layout is the conventional one: a 64-byte partition table at 0x1BE,
// four 16-byte entries, each carrying a type byte and a 32-bit start LBA.
#include "fatro.h"

#include <string.h>

#define DIRENT 32

// Directory entry attributes.
#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LONG_NAME 0x0F   // the four low bits together, not a bit of its own

// The two case bits Windows writes into a short name's reserved byte, so
// "readme.txt" comes back lower case rather than shouting.
#define CASE_BASE_LOWER 0x08
#define CASE_EXT_LOWER  0x10

// Byte-wise, never a cast into the sector buffer. A uint16_t* aimed at an odd
// offset is undefined behaviour, the host tests run under UBSan, and this is
// the exact shape it catches.
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    return !*a && !*b;
}

// Compare against a path COMPONENT, which ends at '/' or the end of the string.
static bool ieq_comp(const char *name, const char *comp, uint32_t len) {
    uint32_t i = 0;
    for (; i < len && name[i]; i++) {
        char ca = name[i], cb = comp[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return i == len && name[i] == 0;
}

// --- the table --------------------------------------------------------------

// One byte of the first FAT copy, through the one-sector cache.
static bool fat_byte(FatRo *f, uint32_t off, uint8_t *out) {
    uint32_t lba = f->first_fat + off / FATRO_SECTOR;
    if (lba != f->cache_lba) {
        if (!f->io.read(f->io.ctx, lba, f->cache)) { f->cache_lba = 0xFFFFFFFFu; return false; }
        f->cache_lba = lba;
    }
    *out = f->cache[off % FATRO_SECTOR];
    return true;
}

// The next cluster in a chain, or 0 when the chain cannot be followed.
//
// A FAT12 entry is twelve bits and CAN STRADDLE A SECTOR BOUNDARY, which is why
// this is byte-wise rather than a 16-bit read: the two halves may live in
// different sectors, and the cache above fetches each one on demand.
static uint32_t fat_next(FatRo *f, uint32_t cluster) {
    if (cluster < 2 || cluster > f->clusters + 1) return 0;

    if (f->type == FATRO_FAT12) {
        uint32_t off = cluster + (cluster / 2);       // 1.5 bytes per entry
        uint8_t lo, hi;
        if (!fat_byte(f, off, &lo) || !fat_byte(f, off + 1, &hi)) return 0;
        uint32_t v = (uint32_t)lo | ((uint32_t)hi << 8);
        v = (cluster & 1) ? (v >> 4) : (v & 0x0FFF);
        return (v >= 0x0FF8) ? 0 : v;                 // >= 0xFF8 ends the chain
    }
    if (f->type == FATRO_FAT16) {
        uint32_t off = cluster * 2;
        uint8_t b0, b1;
        if (!fat_byte(f, off, &b0) || !fat_byte(f, off + 1, &b1)) return 0;
        uint32_t v = (uint32_t)b0 | ((uint32_t)b1 << 8);
        return (v >= 0xFFF8) ? 0 : v;
    }
    uint32_t off = cluster * 4;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) if (!fat_byte(f, off + (uint32_t)i, &b[i])) return 0;
    // The top four bits of a FAT32 entry belong to the implementation, not to
    // the cluster number, and must be masked off before the value is compared
    // against anything.
    uint32_t v = ((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                  ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24)) & 0x0FFFFFFFu;
    return (v >= 0x0FFFFFF8u) ? 0 : v;
}

static uint32_t cluster_lba(const FatRo *f, uint32_t cluster) {
    return f->first_data + (cluster - 2) * f->spc;
}

// --- mounting ---------------------------------------------------------------

// Does this sector carry a BIOS Parameter Block we can use? Checked before the
// partition table, because an unpartitioned card has a BPB at LBA 0 and a
// partitioned one has an MBR there, and both end in 0x55 0xAA.
static bool looks_like_bpb(const uint8_t *b) {
    if (b[510] != 0x55 || b[511] != 0xAA) return false;
    if (rd16(b + 11) != FATRO_SECTOR) return false;          // BPB_BytsPerSec
    uint8_t spc = b[13];                                     // BPB_SecPerClus
    if (!spc || (spc & (spc - 1)) != 0 || spc > 128) return false;
    if (!rd16(b + 14)) return false;                         // BPB_RsvdSecCnt
    uint8_t nfat = b[16];                                    // BPB_NumFATs
    if (nfat < 1 || nfat > 2) return false;
    return true;
}

// exFAT shares the signature and the leading jump instruction and shares
// nothing else. It is named in BS_OEMName, at offset 3.
static bool is_exfat(const uint8_t *b) {
    return memcmp(b + 3, "EXFAT   ", 8) == 0;
}

static void read_label_field(FatRo *f, const uint8_t *b) {
    // BS_VolLab: offset 43 on FAT12/16, 71 on FAT32. Eleven bytes, space
    // padded. Only used when the root directory carries no label entry, which
    // is the one a host actually shows.
    const uint8_t *p = (f->type == FATRO_FAT32) ? b + 71 : b + 43;
    int n = 11;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0)) n--;
    for (int i = 0; i < n; i++) f->label[i] = (char)p[i];
    f->label[n] = 0;
    if (ieq(f->label, "NO NAME")) f->label[0] = 0;
}

static bool find_root_label(FatRo *f);

// Parse the BPB at `base` and fill in the geometry.
static bool mount_at(FatRo *f, uint32_t base) {
    uint8_t *b = f->sec;
    if (!f->io.read(f->io.ctx, base, b)) return false;
    if (is_exfat(b)) return false;
    if (!looks_like_bpb(b)) return false;

    f->part_lba     = base;
    f->spc          = b[13];
    uint16_t rsvd   = rd16(b + 14);
    f->num_fats     = b[16];
    f->root_entries = rd16(b + 17);                 // BPB_RootEntCnt, 0 on FAT32

    uint32_t total = rd16(b + 19);                  // BPB_TotSec16
    if (!total) total = rd32(b + 32);               // BPB_TotSec32
    f->fat_sectors = rd16(b + 22);                  // BPB_FATSz16
    if (!f->fat_sectors) f->fat_sectors = rd32(b + 36);   // BPB_FATSz32
    if (!total || !f->fat_sectors) return false;

    uint32_t root_sectors =
        ((uint32_t)f->root_entries * DIRENT + FATRO_SECTOR - 1) / FATRO_SECTOR;

    f->first_fat  = base + rsvd;
    f->first_root = f->first_fat + f->fat_sectors * f->num_fats;
    f->first_data = f->first_root + root_sectors;
    if (f->first_data <= base || f->first_data - base >= total) return false;
    f->clusters = (total - (f->first_data - base)) / f->spc;
    if (!f->clusters) return false;

    // THE CLUSTER COUNT IS THE FORMAT. Not BS_FilSysType, which is a comment —
    // fatgen103 says so in as many words, and a volume mislabelled there is
    // common. The boundaries are exact and reading a volume at the wrong entry
    // width produces plausible rubbish rather than an error.
    if      (f->clusters < 4085)  f->type = FATRO_FAT12;
    else if (f->clusters < 65525) f->type = FATRO_FAT16;
    else                          f->type = FATRO_FAT32;

    if (f->type == FATRO_FAT32) {
        // A FAT32 volume has no fixed root region at all: BPB_RootEntCnt must
        // be 0 and the root is an ordinary chain starting at BPB_RootClus.
        if (f->root_entries) return false;
        f->root_cluster = rd32(b + 44) & 0x0FFFFFFFu;
        if (f->root_cluster < 2 || f->root_cluster > f->clusters + 1) return false;
        uint16_t fsi = rd16(b + 48);                // BPB_FSInfo
        f->fsinfo_lba = fsi ? base + fsi : 0;
    } else {
        if (!f->root_entries) return false;
        f->root_cluster = 0;
        f->fsinfo_lba = 0;
    }

    read_label_field(f, b);
    f->cache_lba = 0xFFFFFFFFu;
    f->mounted = true;
    // A label in the root directory is what a host shows, so it wins over the
    // boot sector's copy. Failing to find one is not a failure to mount.
    find_root_label(f);
    return true;
}

bool fatro_mount(FatRo *f, const FatRoIo *io) {
    memset(f, 0, sizeof(*f));
    f->io = *io;
    f->cache_lba = 0xFFFFFFFFu;

    // Unpartitioned first. A card formatted by a camera or by `mkfs.vfat` onto
    // the whole device has its BPB at sector 0 and no partition table, and
    // trying to read a partition table out of a boot sector finds four entries
    // of boot code.
    if (!f->io.read(f->io.ctx, 0, f->sec)) return false;
    if (is_exfat(f->sec)) return false;
    if (looks_like_bpb(f->sec)) return mount_at(f, 0);
    if (f->sec[510] != 0x55 || f->sec[511] != 0xAA) return false;

    // Otherwise an MBR. THIS IS THE CASE REAL CARDS ARE IN — a card out of a
    // packet is partitioned, and its volume usually starts at LBA 2048 or 8192,
    // never at 0. A reader that only handles the unpartitioned layout passes
    // every hand-made host test and mounts nothing on hardware.
    //
    // The four start sectors are copied out first because mount_at reads
    // through the same scratch buffer this table is sitting in.
    uint32_t start[4];
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = f->sec + 0x1BE + i * 16;
        uint8_t type = p[4];
        // 0 is an unused slot; 0xEE is a GPT protective entry, whose "partition"
        // is the whole device and is not ours to read.
        start[i] = (!type || type == 0xEE || !rd32(p + 12)) ? 0 : rd32(p + 8);
    }
    for (int i = 0; i < 4; i++) {
        if (!start[i]) continue;
        if (mount_at(f, start[i])) return true;
        FatRoIo keep = f->io;
        memset(f, 0, sizeof(*f));
        f->io = keep;
        f->cache_lba = 0xFFFFFFFFu;
    }
    return false;
}

void fatro_unmount(FatRo *f) {
    memset(f, 0, sizeof(*f));
    f->cache_lba = 0xFFFFFFFFu;
}

const char *fatro_type_name(const FatRo *f) {
    switch (f->type) {
        case FATRO_FAT12: return "FAT12";
        case FATRO_FAT16: return "FAT16";
        case FATRO_FAT32: return "FAT32";
        default:          return "none";
    }
}

uint64_t fatro_total_bytes(const FatRo *f) {
    if (!f->mounted) return 0;
    return (uint64_t)f->clusters * f->spc * FATRO_SECTOR;
}

uint64_t fatro_free_bytes(FatRo *f) {
    if (!f->mounted) return 0;

    if (f->type == FATRO_FAT32 && f->fsinfo_lba) {
        uint8_t *b = f->sec;
        if (f->io.read(f->io.ctx, f->fsinfo_lba, b) &&
            rd32(b + 0)   == 0x41615252u &&        // FSI_LeadSig
            rd32(b + 484) == 0x61417272u &&        // FSI_StrucSig
            rd32(b + 508) == 0xAA550000u) {        // FSI_TrailSig
            uint32_t free_clusters = rd32(b + 488);   // FSI_Free_Count
            // 0xFFFFFFFF means "unknown", and a count larger than the volume
            // means the field is stale — both are "do not know", not zero.
            if (free_clusters != 0xFFFFFFFFu && free_clusters <= f->clusters)
                return (uint64_t)free_clusters * f->spc * FATRO_SECTOR;
        }
        return 0;
    }

    // On a small volume the whole table is a few sectors, so counting is
    // cheaper than the answer is useful. Past that it is not, and 0 means
    // "unknown" to every caller.
    if (f->fat_sectors > 32) return 0;
    uint32_t free_clusters = 0;
    for (uint32_t c = 2; c <= f->clusters + 1; c++) {
        uint32_t off = (f->type == FATRO_FAT12) ? c + (c / 2) : c * 2;
        uint8_t lo, hi;
        if (!fat_byte(f, off, &lo) || !fat_byte(f, off + 1, &hi)) return 0;
        uint32_t v = (uint32_t)lo | ((uint32_t)hi << 8);
        if (f->type == FATRO_FAT12) v = (c & 1) ? (v >> 4) : (v & 0x0FFF);
        if (v == 0) free_clusters++;
    }
    return (uint64_t)free_clusters * f->spc * FATRO_SECTOR;
}

// --- reading a directory ----------------------------------------------------

// Pull the characters of one long-name entry into place. The entries come
// BEFORE the 8.3 entry and in reverse order, each carrying its position in the
// low five bits of byte 0, so they are gathered by index rather than appended.
static void lfn_gather(const uint8_t *e, char *name, bool *ok) {
    static const uint8_t off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    uint32_t seq = e[0] & 0x1F;
    if (!seq) { *ok = false; return; }
    uint32_t base = (seq - 1) * 13;
    for (int i = 0; i < 13; i++) {
        uint32_t idx = base + (uint32_t)i;
        uint16_t c = rd16(e + off[i]);
        if (c == 0xFFFF || c == 0x0000) continue;
        if (idx >= FATRO_MAXNAME - 1) { *ok = false; return; }
        // Anything outside ASCII has no place in a name the shell must print
        // and take as an argument, so the long name is abandoned and the 8.3
        // form used instead. A file is still reachable; it is just SHOUTED.
        if (c > 0x7F) { *ok = false; return; }
        name[idx] = (char)c;
    }
}

static void short_name(const uint8_t *e, char *out) {
    int o = 0;
    bool lb = (e[12] & CASE_BASE_LOWER) != 0;
    bool le = (e[12] & CASE_EXT_LOWER) != 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)e[i];
        // 0x05 in the first byte stands for 0xE5, which would otherwise mark
        // the entry deleted. Kanji-era rule, still in the format.
        if (i == 0 && e[0] == 0x05) c = (char)0xE5;
        if (lb && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[o++] = c;
    }
    if (e[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            if (le && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out[o++] = c;
        }
    }
    out[o] = 0;
}

static uint32_t fat_epoch(uint16_t date, uint16_t time) {
    if (!date) return 0;
    uint32_t year = 1980 + ((date >> 9) & 0x7F);
    uint32_t month = (date >> 5) & 0x0F;
    uint32_t day = date & 0x1F;
    if (!month || month > 12 || !day) return 0;

    static const uint16_t cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    uint32_t days = 0;
    for (uint32_t y = 1970; y < year; y++)
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366u : 365u;
    days += cum[month - 1];
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (leap && month > 2) days++;
    days += day - 1;

    uint32_t secs = ((time >> 11) & 0x1F) * 3600u + ((time >> 5) & 0x3F) * 60u +
                    (time & 0x1F) * 2u;
    return days * 86400u + secs;
}

// Hand every real 32-byte entry to `fn`, with whatever long name preceded it.
// `fn` returns false to stop. Returns false only on an I/O failure — a card
// pulled out mid-walk — so "stopped early" and "the card went away" stay
// distinguishable.
typedef bool (*RawFn)(void *ctx, const uint8_t *ent, const char *lname);

static bool dir_raw(FatRo *f, uint32_t start_cluster, bool fixed_root,
                    RawFn fn, void *ctx) {
    char lname[FATRO_MAXNAME];
    bool lok = false;
    memset(lname, 0, sizeof(lname));

    uint32_t cluster = start_cluster;
    uint32_t root_sectors =
        ((uint32_t)f->root_entries * DIRENT + FATRO_SECTOR - 1) / FATRO_SECTOR;
    uint32_t sector_in_cluster = 0;
    uint32_t fixed_index = 0;
    // A corrupt chain that points at itself would otherwise spin here forever.
    uint32_t budget = f->clusters + 2;

    while (true) {
        uint32_t lba;
        if (fixed_root) {
            if (fixed_index >= root_sectors) return true;
            lba = f->first_root + fixed_index++;
        } else {
            if (cluster < 2 || cluster > f->clusters + 1) return true;
            lba = cluster_lba(f, cluster) + sector_in_cluster;
        }

        if (!f->io.read(f->io.ctx, lba, f->sec)) return false;

        for (uint32_t i = 0; i < FATRO_SECTOR / DIRENT; i++) {
            const uint8_t *e = f->sec + i * DIRENT;
            if (e[0] == 0x00) return true;              // nothing beyond here
            if (e[0] == 0xE5) { lok = false; memset(lname, 0, sizeof(lname)); continue; }
            uint8_t attr = e[11];
            if ((attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
                if (!lok) { memset(lname, 0, sizeof(lname)); lok = true; }
                lfn_gather(e, lname, &lok);
                continue;
            }
            bool cont = fn(ctx, e, lok && lname[0] ? lname : "");
            lok = false;
            memset(lname, 0, sizeof(lname));
            if (!cont) return true;
            // fn is free to use f->sec for its own reads, so the sector this
            // loop is walking may no longer be the one it started with. It does
            // not, today, and this is the note for the day somebody makes it.
        }

        if (!fixed_root) {
            if (++sector_in_cluster >= f->spc) {
                sector_in_cluster = 0;
                if (!budget--) return true;
                cluster = fat_next(f, cluster);
            }
        }
    }
}

static void fill_entry(FatRoEntry *out, const uint8_t *e, const char *lname) {
    memset(out, 0, sizeof(*out));
    if (lname[0]) {
        uint32_t n = 0;
        while (lname[n] && n < FATRO_MAXNAME - 1) { out->name[n] = lname[n]; n++; }
        out->name[n] = 0;
    } else {
        short_name(e, out->name);
    }
    out->is_dir = (e[11] & ATTR_DIRECTORY) != 0;
    out->size   = out->is_dir ? 0 : rd32(e + 28);
    out->first_cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
    out->mtime  = fat_epoch(rd16(e + 24), rd16(e + 22));
}

// Is this an entry a listing should show? The volume label is not a file, and
// '.' and '..' are the directory itself and its parent.
static bool listable(const uint8_t *e, const char *name) {
    if (e[11] & ATTR_VOLUME_ID) return false;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) return false;
    return true;
}

struct LabelCtx { FatRo *f; bool found; };

static bool label_cb(void *ctx, const uint8_t *e, const char *lname) {
    (void)lname;
    LabelCtx *c = (LabelCtx *)ctx;
    if (!(e[11] & ATTR_VOLUME_ID)) return true;
    int n = 11;
    while (n > 0 && (e[n - 1] == ' ' || e[n - 1] == 0)) n--;
    for (int i = 0; i < n; i++) c->f->label[i] = (char)e[i];
    c->f->label[n] = 0;
    c->found = true;
    return false;
}

static bool find_root_label(FatRo *f) {
    LabelCtx c{f, false};
    dir_raw(f, f->root_cluster, f->type != FATRO_FAT32, label_cb, &c);
    return c.found;
}

// --- resolving a path -------------------------------------------------------

struct FindCtx {
    const char *comp;
    uint32_t    len;
    FatRoEntry *out;
    bool        found;
};

static bool find_cb(void *ctx, const uint8_t *e, const char *lname) {
    FindCtx *c = (FindCtx *)ctx;
    char sfn[16];
    short_name(e, sfn);
    if (!listable(e, lname[0] ? lname : sfn)) return true;
    // Both forms match, case-insensitively, because that is what the format
    // itself does and because a file copied on from Windows may only have one.
    if (!ieq_comp(lname[0] ? lname : sfn, c->comp, c->len) &&
        !ieq_comp(sfn, c->comp, c->len)) return true;
    fill_entry(c->out, e, lname);
    c->found = true;
    return false;
}

// The root, as an entry. It has no directory entry of its own anywhere on the
// volume, so one is made up: no name, no size, and the root chain as its
// cluster (0 on FAT12/16, where the root is a fixed region and not a chain).
static void root_entry(const FatRo *f, FatRoEntry *out) {
    memset(out, 0, sizeof(*out));
    out->is_dir = true;
    out->first_cluster = (f->type == FATRO_FAT32) ? f->root_cluster : 0;
}

// Walk `path` from the root. Returns false when a component does not exist, is
// not a directory when it must be, or the card stops answering.
static bool resolve(FatRo *f, const char *path, FatRoEntry *out) {
    if (!f->mounted || !path) return false;
    root_entry(f, out);

    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        uint32_t len = 0;
        while (p[len] && p[len] != '/') len++;
        if (!len) break;
        if (!out->is_dir) return false;

        bool fixed = (f->type != FATRO_FAT32) && (out->first_cluster == 0);
        FindCtx c{p, len, out, false};
        FatRoEntry parent = *out;
        if (!dir_raw(f, parent.first_cluster, fixed, find_cb, &c)) return false;
        if (!c.found) return false;

        p += len;
        while (*p == '/') p++;
    }
    return true;
}

struct ListCtx { FatRoWalkFn cb; void *ctx; };

static bool list_cb(void *ctx, const uint8_t *e, const char *lname) {
    ListCtx *c = (ListCtx *)ctx;
    FatRoEntry ent;
    fill_entry(&ent, e, lname);
    if (!listable(e, ent.name)) return true;
    c->cb(c->ctx, &ent);
    return true;
}

bool fatro_list(FatRo *f, const char *path, FatRoWalkFn cb, void *ctx) {
    FatRoEntry dir;
    if (!resolve(f, path, &dir) || !dir.is_dir) return false;
    bool fixed = (f->type != FATRO_FAT32) && (dir.first_cluster == 0);
    ListCtx c{cb, ctx};
    return dir_raw(f, dir.first_cluster, fixed, list_cb, &c);
}

bool fatro_stat(FatRo *f, const char *path, FatRoEntry *out) {
    return resolve(f, path, out);
}

// --- reading a file ---------------------------------------------------------

void fatro_cursor_init(FatRoCursor *c) {
    c->first = 0; c->cluster = 0; c->at = 0;
}

uint32_t fatro_read(FatRo *f, const FatRoEntry *e, FatRoCursor *cur,
                    uint32_t off, void *buf, uint32_t len) {
    if (!f->mounted || !e || e->is_dir || !buf || !len) return 0;
    if (off >= e->size) return 0;
    if (len > e->size - off) len = e->size - off;

    const uint32_t csize = (uint32_t)f->spc * FATRO_SECTOR;
    uint32_t cluster = e->first_cluster;
    uint32_t at = 0;

    // Resume where the last read left off when this is the same file and the
    // read is moving forward. Anything else walks from the start and re-seats
    // the cursor, which is correct if slower.
    if (cur && cur->first == e->first_cluster && cur->cluster >= 2 &&
        off >= cur->at) {
        cluster = cur->cluster;
        at = cur->at;
    }

    uint32_t budget = f->clusters + 2;
    while (at + csize <= off) {
        cluster = fat_next(f, cluster);
        if (cluster < 2 || !budget--) return 0;
        at += csize;
    }
    if (cur) { cur->first = e->first_cluster; cur->cluster = cluster; cur->at = at; }

    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < len) {
        if (cluster < 2 || cluster > f->clusters + 1) break;
        uint32_t in_cluster = (off + done) - at;
        uint32_t sec_in = in_cluster / FATRO_SECTOR;
        uint32_t sec_off = in_cluster % FATRO_SECTOR;
        if (!f->io.read(f->io.ctx, cluster_lba(f, cluster) + sec_in, f->sec)) break;

        uint32_t n = FATRO_SECTOR - sec_off;
        if (n > len - done) n = len - done;
        memcpy(dst + done, f->sec + sec_off, n);
        done += n;

        if ((in_cluster + n) >= csize) {
            uint32_t next = fat_next(f, cluster);
            at += csize;
            cluster = next;
            if (cur) { cur->cluster = cluster; cur->at = at; }
        }
    }
    return done;
}
