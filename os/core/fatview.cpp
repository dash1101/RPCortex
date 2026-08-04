#include "fatview.h"

#include <string.h>

// Little-endian stores. FAT is a little-endian format and the target is a
// little-endian machine, but a struct overlay would still depend on the
// compiler not inserting padding into a layout that has none. Byte at a time is
// the version that cannot be wrong.
static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

bool fat_geom_init(FatGeom *g, uint32_t real_bytes) {
    if (!g) return false;
    memset(g, 0, sizeof(*g));

    g->reserved_sectors    = 1;
    g->sectors_per_cluster = 1;    // 512-byte clusters: the most clusters per byte
    g->num_fats            = 1;    // one copy; a second buys redundancy nothing here reads
    g->root_entries        = 512;
    g->root_sectors        = (uint32_t)g->root_entries * FAT_DIRENT_SIZE / FAT_SECTOR_SIZE;

    uint32_t real_sectors = real_bytes / FAT_SECTOR_SIZE;

    // How many clusters the real storage can back, once the overhead is taken
    // out. The table's size depends on the cluster count and the cluster count
    // depends on the table's size, so this converges rather than solving: each
    // pass uses the previous estimate, and it settles in two or three.
    uint32_t clusters = 0;
    uint32_t fat_sectors = 1;
    for (int pass = 0; pass < 8; pass++) {
        uint32_t overhead = g->reserved_sectors + fat_sectors * g->num_fats + g->root_sectors;
        if (real_sectors <= overhead) { clusters = 0; break; }
        clusters = (real_sectors - overhead) / g->sectors_per_cluster;
        // Two bytes per entry, plus the two reserved entries at the start.
        uint32_t need = ((clusters + FAT_FIRST_DATA_CLUSTER) * 2 + FAT_SECTOR_SIZE - 1)
                        / FAT_SECTOR_SIZE;
        if (need == fat_sectors) break;
        fat_sectors = need;
    }

    g->real_clusters = clusters;

    // Below the FAT16 floor the volume is padded rather than described
    // honestly, because a volume just under the line is read as FAT12 and every
    // entry comes out at the wrong bit offset. The padding is reported bad, so
    // the host neither counts it as free nor writes to it.
    g->data_clusters = clusters < FAT16_MIN_DATA_CLUSTERS ? FAT16_MIN_DATA_CLUSTERS
                                                          : clusters;

    // Size the table for what is actually claimed, which after padding may be
    // more than the loop above settled on.
    g->fat_sectors = ((g->data_clusters + FAT_FIRST_DATA_CLUSTER) * 2 + FAT_SECTOR_SIZE - 1)
                     / FAT_SECTOR_SIZE;

    g->first_fat_sector  = g->reserved_sectors;
    g->first_root_sector = g->first_fat_sector + g->fat_sectors * g->num_fats;
    g->first_data_sector = g->first_root_sector + g->root_sectors;
    g->total_sectors     = g->first_data_sector + g->data_clusters * g->sectors_per_cluster;

    // Padding lifts a volume that is nearly big enough over the FAT16 floor. It
    // is not a way to invent a volume out of nothing: with no real clusters at
    // all, every cluster would be padding, and the result is a drive that
    // mounts and can hold no file. That is refused rather than presented.
    return g->real_clusters > 0 && g->data_clusters >= FAT16_MIN_DATA_CLUSTERS;
}

FatRegion fat_region(const FatGeom *g, uint32_t lba,
                     uint32_t *index, uint32_t *sector_in_cluster) {
    uint32_t idx = 0, sic = 0;
    FatRegion r;

    if (lba >= g->total_sectors) {
        r = FAT_RGN_BEYOND;
    } else if (lba < g->reserved_sectors) {
        r = FAT_RGN_BOOT;
        idx = lba;
    } else if (lba < g->first_root_sector) {
        r = FAT_RGN_FAT;
        // Modulo, not subtract: with two copies of the table the second must
        // answer identically to the first, and the host does compare them.
        idx = (lba - g->first_fat_sector) % g->fat_sectors;
    } else if (lba < g->first_data_sector) {
        r = FAT_RGN_ROOT;
        idx = lba - g->first_root_sector;
    } else {
        uint32_t off = lba - g->first_data_sector;
        r = FAT_RGN_DATA;
        idx = off / g->sectors_per_cluster + FAT_FIRST_DATA_CLUSTER;
        sic = off % g->sectors_per_cluster;
    }

    if (index) *index = idx;
    if (sector_in_cluster) *sector_in_cluster = sic;
    return r;
}

void fat_build_boot(const FatGeom *g, uint8_t sec[FAT_SECTOR_SIZE],
                    const char *label, uint32_t volume_id) {
    memset(sec, 0, FAT_SECTOR_SIZE);

    // A jump instruction. Nothing executes it, but a host that finds something
    // else here decides the sector is not a boot sector at all.
    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    memcpy(sec + 3, "MSWIN4.1", 8);   // the OEM name every implementation accepts

    put16(sec + 11, FAT_SECTOR_SIZE);          // bytes per sector
    sec[13] = g->sectors_per_cluster;
    put16(sec + 14, g->reserved_sectors);
    sec[16] = g->num_fats;
    put16(sec + 17, g->root_entries);

    // The 16-bit total-sector count, which is zero when the volume needs the
    // 32-bit one at offset 32. Exactly one of the two carries the number.
    if (g->total_sectors < 0x10000) {
        put16(sec + 19, (uint16_t)g->total_sectors);
        put32(sec + 32, 0);
    } else {
        put16(sec + 19, 0);
        put32(sec + 32, g->total_sectors);
    }

    sec[21] = 0xF8;                            // fixed disk
    put16(sec + 22, (uint16_t)g->fat_sectors);
    put16(sec + 24, 63);                       // sectors per track, geometry theatre
    put16(sec + 26, 255);                      // heads, likewise
    put32(sec + 28, 0);                        // hidden sectors: not a partition

    sec[36] = 0x80;                            // drive number
    sec[38] = 0x29;                            // an extended boot signature follows
    put32(sec + 39, volume_id);

    // Both label fields are space padded rather than NUL terminated, and the
    // filesystem type is documentation rather than something a host trusts —
    // FAT16 is decided by the cluster count, not by this string.
    memset(sec + 43, ' ', 11);
    for (int i = 0; i < 11 && label && label[i]; i++) sec[43 + i] = (uint8_t)label[i];
    memcpy(sec + 54, "FAT16   ", 8);

    sec[510] = 0x55; sec[511] = 0xAA;
}

int32_t fat_node_for_cluster(const FatNode *nodes, uint32_t count, uint32_t cluster) {
    for (uint32_t i = 0; i < count; i++) {
        // A node with no clusters owns nothing. Without this an empty file,
        // whose first cluster is 0, would appear to own every cluster below its
        // own — the range test alone is not enough.
        if (!nodes[i].clusters) continue;
        if (cluster >= nodes[i].first_cluster &&
            cluster <  nodes[i].first_cluster + nodes[i].clusters)
            return (int32_t)i;
    }
    return -1;
}

static const FatNode *node_for_cluster(const FatNode *nodes, uint32_t count, uint32_t cluster) {
    int32_t i = fat_node_for_cluster(nodes, count, cluster);
    return i < 0 ? nullptr : &nodes[i];
}

void fat_fat_sector(const FatGeom *g, uint32_t fat_sector,
                    const FatNode *nodes, uint32_t node_count,
                    uint8_t sec[FAT_SECTOR_SIZE]) {
    memset(sec, 0, FAT_SECTOR_SIZE);

    const uint32_t per_sector = FAT_SECTOR_SIZE / 2;
    uint32_t first = fat_sector * per_sector;

    for (uint32_t i = 0; i < per_sector; i++) {
        uint32_t cluster = first + i;
        uint16_t value;

        if (cluster < FAT_FIRST_DATA_CLUSTER) {
            // The two reserved entries. The first repeats the media byte in its
            // low bits; the second is a plain end-of-chain.
            value = cluster == 0 ? 0xFFF8 : 0xFFFF;
        } else if (cluster >= FAT_FIRST_DATA_CLUSTER + g->data_clusters) {
            value = FAT16_FREE;             // past the table's meaningful end
        } else if (cluster >= FAT_FIRST_DATA_CLUSTER + g->real_clusters) {
            // Padding, so that the volume clears the FAT16 floor. Bad rather
            // than free: the host must neither count it as space nor write to
            // it, and bad is the only value that says both.
            value = FAT16_BAD;
        } else {
            const FatNode *n = node_for_cluster(nodes, node_count, cluster);
            if (!n) value = FAT16_FREE;
            else if (cluster + 1 < n->first_cluster + n->clusters) value = (uint16_t)(cluster + 1);
            else value = FAT16_EOC;
        }

        put16(sec + i * 2, value);
    }
}

void fat_time_encode(uint32_t epoch, uint16_t *date, uint16_t *time) {
    // Days from 1970-01-01 to 1980-01-01, which is where FAT starts counting.
    const uint32_t FAT_EPOCH = 315532800u;

    // Before 1980 there is nothing to encode. Zero in particular means the
    // clock was never set, and the honest answer to that is the earliest date
    // the format can hold — not a wrapped value that would show as some
    // confident date far in the future.
    if (epoch < FAT_EPOCH) {
        if (date) *date = (1 << 5) | 1;   // 1980-01-01, month and day are 1-based
        if (time) *time = 0;
        return;
    }

    uint32_t secs = epoch - FAT_EPOCH;
    uint32_t days = secs / 86400u;
    uint32_t rem  = secs % 86400u;

    uint32_t hour = rem / 3600u;
    uint32_t min  = (rem % 3600u) / 60u;
    uint32_t sec  = rem % 60u;

    // Walk the calendar. A closed form exists; this is a handful of iterations
    // on a device with no date library, and it is obviously right.
    uint32_t year = 1980;
    while (true) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        uint32_t len = leap ? 366u : 365u;
        if (days < len) break;
        days -= len;
        year++;
    }
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static const uint8_t mlen[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t month = 0;
    while (month < 12) {
        uint32_t len = mlen[month] + ((month == 1 && leap) ? 1u : 0u);
        if (days < len) break;
        days -= len;
        month++;
    }

    // The year field is only seven bits, so 2107 is the last one representable.
    uint32_t y = year - 1980;
    if (y > 127) y = 127;

    if (date) *date = (uint16_t)((y << 9) | ((month + 1) << 5) | (days + 1));
    // Seconds are stored in two-second units, which is why a FAT timestamp is
    // always even.
    if (time) *time = (uint16_t)((hour << 11) | (min << 5) | (sec / 2));
}

void fat_dirent(uint8_t e[FAT_DIRENT_SIZE], const char name[11], uint8_t attr,
                uint32_t first_cluster, uint32_t size, uint32_t mtime,
                uint8_t case_flags) {
    memset(e, 0, FAT_DIRENT_SIZE);
    memcpy(e, name, 11);
    e[11] = attr;
    e[12] = case_flags;        // the NT case bits: which halves to show lower case

    uint16_t d, t;
    fat_time_encode(mtime, &d, &t);
    put16(e + 14, t);          // creation time
    put16(e + 16, d);          // creation date
    put16(e + 18, d);          // last access date
    put16(e + 22, t);          // write time
    put16(e + 24, d);          // write date

    put16(e + 20, 0);          // high cluster word: always zero on FAT16
    put16(e + 26, (uint16_t)first_cluster);
    put32(e + 28, size);
}

void fat_dirent_label(uint8_t e[FAT_DIRENT_SIZE], const char *label) {
    char name[11];
    memset(name, ' ', sizeof(name));
    for (int i = 0; i < 11 && label && label[i]; i++) name[i] = label[i];
    // No cluster and no size: the label is a name and nothing else. The label
    // is never case-folded — it is shown exactly as given.
    fat_dirent(e, name, FAT_ATTR_VOLUME_ID, 0, 0, 0, 0);
}

void fat_dirent_dot(uint8_t e[FAT_DIRENT_SIZE], bool dotdot,
                    uint32_t self_cluster, uint32_t parent_cluster,
                    uint32_t mtime) {
    char name[11];
    memset(name, ' ', sizeof(name));
    name[0] = '.';
    if (dotdot) name[1] = '.';
    // ".." names the root as cluster 0, which is the one place the format uses
    // a cluster number that is not a real cluster.
    fat_dirent(e, name, FAT_ATTR_DIRECTORY,
               dotdot ? parent_cluster : self_cluster, 0, mtime, 0);
}

// Rebuild the displayable name from the 8.3 form and the case bits — exactly
// what a host without long-name support would show.
static void render_83(const char name[11], uint8_t case_flags, char out[13]) {
    int o = 0;
    bool lb = (case_flags & FAT_CASE_BASE_LOWER) != 0;
    bool le = (case_flags & FAT_CASE_EXT_LOWER) != 0;
    for (int i = 0; i < 8 && name[i] != ' '; i++) {
        char c = name[i];
        if (lb && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[o++] = c;
    }
    if (name[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && name[i] != ' '; i++) {
            char c = name[i];
            if (le && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out[o++] = c;
        }
    }
    out[o] = 0;
}

bool fat_name_fits_83(const char *name) {
    if (!name || !*name) return true;
    char sn[11], shown[13];
    uint8_t flags = 0;
    if (!fat_shortname(name, sn, &flags)) return false;
    render_83(sn, flags, shown);
    return strcmp(shown, name) == 0;
}

// Thirteen UTF-16 code units per long-name entry.
#define LFN_CHARS 13

uint32_t fat_entries_for(const FatNode *n) {
    if (!n || !n->lname || fat_name_fits_83(n->lname)) return 1;
    uint32_t len = (uint32_t)strlen(n->lname);
    return 1 + (len + LFN_CHARS - 1) / LFN_CHARS;
}

// How many entry slots a directory's children occupy in total.
static uint32_t child_entries(const FatNode *nodes, uint32_t count, uint32_t idx) {
    uint32_t c = 0;
    for (uint32_t i = 1; i < count; i++)
        if (i != idx && nodes[i].parent == idx) c += fat_entries_for(&nodes[i]);
    return c;
}

// The checksum a long-name entry carries, computed over the 8.3 name it
// belongs to. It is what ties the two together: a host that finds a mismatch
// discards the long name and falls back to the short one.
static uint8_t lfn_checksum(const char name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)name[i]);
    return sum;
}

// One long-name entry. `seq` is 1-based and counts from the START of the name;
// entries are written to disk in reverse, so the highest sequence comes first
// and carries the "last" marker.
static void fat_lfn_entry(uint8_t e[FAT_DIRENT_SIZE], const char *lname,
                          uint32_t seq, bool last, const char name83[11]) {
    memset(e, 0, FAT_DIRENT_SIZE);
    e[0]  = (uint8_t)(seq | (last ? 0x40 : 0));
    e[11] = 0x0F;                       // the attribute combination that means "long name"
    e[12] = 0;
    e[13] = lfn_checksum(name83);
    e[26] = 0; e[27] = 0;               // no cluster: this is not a file

    static const uint8_t off[LFN_CHARS] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    uint32_t len = (uint32_t)strlen(lname);
    uint32_t base = (seq - 1) * LFN_CHARS;
    for (uint32_t i = 0; i < LFN_CHARS; i++) {
        uint32_t idx = base + i;
        uint16_t c;
        if (idx < len)       c = (uint8_t)lname[idx];   // ASCII widened to UTF-16
        else if (idx == len) c = 0x0000;                // the terminator, if it fits
        else                 c = 0xFFFF;                // and padding after it
        e[off[i]]     = (uint8_t)(c & 0xFF);
        e[off[i] + 1] = (uint8_t)(c >> 8);
    }
}

uint32_t fat_layout(FatNode *nodes, uint32_t count, uint32_t max_clusters) {
    if (!nodes || !count) return 0;

    // The root occupies no cluster, so the data area starts with whatever comes
    // after it. Setting this explicitly matters: a root left with a stale
    // cluster run would own part of the data area and shadow a real file.
    nodes[0].first_cluster = 0;
    nodes[0].clusters = 0;

    uint32_t next = FAT_FIRST_DATA_CLUSTER;
    for (uint32_t i = 1; i < count; i++) {
        FatNode *f = &nodes[i];
        uint32_t bytes = f->is_dir
            // "." and ".." first, then however many entries the children need
            // — which is more than one apiece for anything with a long name.
            ? (2 * FAT_DIRENT_SIZE) + child_entries(nodes, count, i) * FAT_DIRENT_SIZE
            : f->size;
        uint32_t clusters = (bytes + FAT_SECTOR_SIZE - 1) / FAT_SECTOR_SIZE;

        // An empty file owns no cluster, and FAT says so with a first cluster
        // of zero. Giving it one would put a cluster in the table that no chain
        // ever reaches, which is what a host reports as a lost cluster.
        //
        // A node that does not fit is emptied rather than truncated: a file
        // whose run overlaps the next one's is silent corruption of both.
        if (clusters == 0 || next + clusters > FAT_FIRST_DATA_CLUSTER + max_clusters) {
            f->first_cluster = 0;
            f->clusters = 0;
            if (!f->is_dir) f->size = 0;
            continue;
        }
        f->first_cluster = next;
        f->clusters = clusters;
        next += clusters;
    }
    return next - FAT_FIRST_DATA_CLUSTER;
}

void fat_dir_sector(const FatNode *nodes, uint32_t count, uint32_t dir_idx,
                    uint32_t first_entry, const char *label,
                    uint8_t sec[FAT_SECTOR_SIZE]) {
    memset(sec, 0, FAT_SECTOR_SIZE);
    const uint32_t per_sector = FAT_SECTOR_SIZE / FAT_DIRENT_SIZE;   // 16

    // What comes before the children: the volume label in the root, "." and
    // ".." everywhere else. A subdirectory without those two is one Windows
    // reports as damaged.
    const uint32_t lead = (dir_idx == 0) ? 1u : 2u;

    for (uint32_t s = 0; s < per_sector; s++) {
        uint32_t e = first_entry + s;
        uint8_t *slot = sec + s * FAT_DIRENT_SIZE;

        if (e < lead) {
            if (dir_idx == 0) {
                fat_dirent_label(slot, label);
            } else {
                uint16_t parent = nodes[dir_idx].parent;
                uint32_t parent_cluster =
                    (parent == FAT_NO_PARENT || parent == 0 || parent >= count)
                        ? 0u : nodes[parent].first_cluster;
                fat_dirent_dot(slot, e == 1, nodes[dir_idx].first_cluster,
                               parent_cluster, nodes[dir_idx].mtime);
            }
            continue;
        }

        // Which child owns entry `e`, and which of ITS entries this is.
        //
        // No longer one slot per child: a long name is a run of entries ending
        // with the 8.3 one. Scanned rather than indexed, because children are
        // not contiguous in the table and a second structure to make them so is
        // one more thing that can disagree with this one.
        uint32_t want = e - lead, slot_base = 0;
        const FatNode *child = nullptr;
        uint32_t need = 0;
        for (uint32_t i = 1; i < count; i++) {
            if (i == dir_idx || nodes[i].parent != dir_idx) continue;
            need = fat_entries_for(&nodes[i]);
            if (want < slot_base + need) { child = &nodes[i]; break; }
            slot_base += need;
        }
        // Past the last entry. The rest of the sector stays zeroed, and a zero
        // first byte is what tells the host the directory ends here.
        if (!child) return;

        uint32_t within = want - slot_base;
        if (within + 1 < need) {
            // A long-name entry. They are stored in reverse, so the first one
            // on disk carries the highest sequence number and the end marker.
            uint32_t lfn_total = need - 1;
            uint32_t seq = lfn_total - within;
            fat_lfn_entry(slot, child->lname, seq, within == 0, child->name);
            continue;
        }

        // Everything already on the device is read-only. The write path accepts
        // new files and nothing else, and saying so in the attribute byte is
        // what makes a refused edit show up as the host declining rather than
        // as a write silently discarded.
        uint8_t attr = child->is_dir ? FAT_ATTR_DIRECTORY
                                     : (uint8_t)(FAT_ATTR_READ_ONLY | FAT_ATTR_ARCHIVE);
        fat_dirent(slot, child->name, attr, child->first_cluster,
                   child->size, child->mtime, child->case_flags);
    }
}

// Characters the format forbids in a short name.
static bool illegal_83(char c) {
    if ((unsigned char)c < 0x20) return true;
    return strchr("\"*+,./:;<=>?[\\]|", c) != nullptr;
}

bool fat_shortname(const char *name, char out[11], uint8_t *case_flags) {
    memset(out, ' ', 11);
    if (case_flags) *case_flags = 0;
    if (!name || !*name) return false;

    // A leading dot means a name that is all extension, which 8.3 cannot
    // express. These are the dotfiles the host itself tends to create, and they
    // are refused rather than mangled.
    if (name[0] == '.') return false;

    // Split at the LAST dot: "a.tar.gz" has extension "gz", not "tar.gz".
    const char *dot = nullptr;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;

    int base_len = dot ? (int)(dot - name) : (int)strlen(name);
    if (base_len <= 0) return false;

    // Each half is foldable only if it holds no upper-case letter at all.
    // Mixed case has no representation, and showing one half of a name in the
    // wrong case is worse than showing all of it shouted.
    bool base_upper = false, base_lower = false;
    bool ext_upper = false, ext_lower = false;

    int o = 0;
    for (int i = 0; i < base_len && o < 8; i++) {
        char c = name[i];
        if (illegal_83(c)) continue;
        if (c >= 'a' && c <= 'z') { base_lower = true; c = (char)(c - 'a' + 'A'); }
        else if (c >= 'A' && c <= 'Z') base_upper = true;
        out[o++] = c;
    }
    if (o == 0) return false;   // nothing survived the character rules

    if (dot) {
        int e = 0;
        for (const char *p = dot + 1; *p && e < 3; p++) {
            char c = *p;
            if (illegal_83(c)) continue;
            if (c >= 'a' && c <= 'z') { ext_lower = true; c = (char)(c - 'a' + 'A'); }
            else if (c >= 'A' && c <= 'Z') ext_upper = true;
            out[8 + e++] = c;
        }
    }

    if (case_flags) {
        uint8_t f = 0;
        if (base_lower && !base_upper) f |= FAT_CASE_BASE_LOWER;
        if (ext_lower  && !ext_upper)  f |= FAT_CASE_EXT_LOWER;
        *case_flags = f;
    }

    // 0xE5 in the first byte marks a deleted entry, so a name that genuinely
    // starts with that byte has to be stored as the substitute the format
    // reserves for it.
    if ((uint8_t)out[0] == 0xE5) out[0] = (char)0x05;
    return true;
}

bool fat_is_host_metadata(const char *name) {
    static const char *const junk[] = {
        "System Volume Information",
        ".Spotlight-V100",
        ".fseventsd",
        ".Trashes",
        ".TemporaryItems",
        ".DS_Store",
        "$RECYCLE.BIN",
        "RECYCLER",
        "found.000",
        "autorun.inf",
        "desktop.ini",
        "Thumbs.db",
        "._.Trashes",
        "IndexerVolumeGuid",
        "WPSettings.dat",
    };
    if (!name) return false;
    for (unsigned i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        const char *a = name, *b = junk[i];
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            a++; b++;
        }
        if (!*a && !*b) return true;
    }
    // Anything beginning "._" is a macOS resource fork companion, of which
    // there is one per file copied and none worth keeping.
    if (name[0] == '.' && name[1] == '_') return true;
    return false;
}
