// The SD card's filesystem reader, against volumes built here byte by byte.
//
// The card itself cannot be tested without a card. The FORMAT can, completely,
// and it is where the bugs that matter live: a FAT32 volume read as FAT16 does
// not fail, it returns confident rubbish, and a reader that only understands an
// unpartitioned volume passes every hand-made test and then mounts nothing at
// all on real hardware, because a card out of a packet has an MBR.
//
// So this builds three volumes — FAT12, FAT16 unpartitioned, FAT32 behind a
// partition table starting at LBA 2048, the way a card actually is — and reads
// them back. The FAT32 one is a legal FAT32 volume rather than a small one
// wearing the name: the format is decided by the cluster count and 65525 is the
// line, so anything smaller would be read as FAT16 and would prove nothing.
// That is 32 MB, which is why the block device below is SPARSE: only the
// sectors something was written to exist, and the rest read back as zeros.
//
// It also covers the case a card is in more often than anyone would like — gone
// mid-operation — by making the device start refusing reads and checking that
// every entry point gives up cleanly instead of hanging or handing back a
// half-filled buffer.
#include "../core/fatro.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <map>
#include <vector>
#include <string>

static int checks, fails;
static void ck(bool c, const char *w) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", w); fails++; }
}
static void cks(const char *got, const char *want, const char *w) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("  FAIL: %s (got '%s', wanted '%s')\n", w, got, want);
        fails++;
    }
}

// --- a sparse block device ---------------------------------------------------

struct Sector { uint8_t b[FATRO_SECTOR]; };

struct Dev {
    std::map<uint32_t, Sector> s;
    bool     gone = false;      // the card was pulled out
    uint32_t reads = 0;

    uint8_t *at(uint32_t lba) {
        auto it = s.find(lba);
        if (it == s.end()) {
            Sector z; memset(z.b, 0, sizeof(z.b));
            it = s.emplace(lba, z).first;
        }
        return it->second.b;
    }
    void put(uint32_t lba, const uint8_t *src, uint32_t off, uint32_t len) {
        memcpy(at(lba) + off, src, len);
    }
};

static bool dev_read(void *ctx, uint32_t lba, void *buf) {
    Dev *d = (Dev *)ctx;
    if (d->gone) return false;
    d->reads++;
    auto it = d->s.find(lba);
    if (it == d->s.end()) { memset(buf, 0, FATRO_SECTOR); return true; }
    memcpy(buf, it->second.b, FATRO_SECTOR);
    return true;
}

// --- building a volume -------------------------------------------------------

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

struct Vol {
    Dev     *dev;
    uint32_t base;            // partition start LBA, 0 for unpartitioned
    int      type;            // 12, 16 or 32
    uint32_t spc;
    uint32_t rsvd;
    uint32_t nfats;
    uint32_t root_entries;    // 0 on FAT32
    uint32_t fat_sectors;
    uint32_t clusters;
    uint32_t total;
    uint32_t first_fat, first_root, first_data;
    uint32_t root_cluster;
    uint32_t next_free;
    uint32_t used;            // clusters handed out, for an honest FSInfo
};

static void fat_set(Vol &v, uint32_t cl, uint32_t val) {
    if (v.type == 12) {
        uint32_t off = cl + cl / 2;
        for (uint32_t copy = 0; copy < v.nfats; copy++) {
            uint32_t fbase = v.first_fat + copy * v.fat_sectors;
            uint8_t *a = v.dev->at(fbase + off / FATRO_SECTOR);
            uint8_t *b = v.dev->at(fbase + (off + 1) / FATRO_SECTOR);
            uint8_t lo = a[off % FATRO_SECTOR], hi = b[(off + 1) % FATRO_SECTOR];
            uint16_t cur = (uint16_t)(lo | (hi << 8));
            cur = (cl & 1) ? (uint16_t)((cur & 0x000F) | (val << 4))
                           : (uint16_t)((cur & 0xF000) | (val & 0x0FFF));
            a[off % FATRO_SECTOR] = (uint8_t)cur;
            b[(off + 1) % FATRO_SECTOR] = (uint8_t)(cur >> 8);
        }
        return;
    }
    uint32_t width = (v.type == 16) ? 2 : 4;
    uint32_t off = cl * width;
    for (uint32_t copy = 0; copy < v.nfats; copy++) {
        uint8_t *p = v.dev->at(v.first_fat + copy * v.fat_sectors + off / FATRO_SECTOR);
        if (width == 2) wr16(p + off % FATRO_SECTOR, (uint16_t)val);
        else            wr32(p + off % FATRO_SECTOR, val);
    }
}

static uint32_t eoc(const Vol &v) {
    return v.type == 12 ? 0xFFF : v.type == 16 ? 0xFFFF : 0x0FFFFFFF;
}

// Allocate a chain of `n` clusters. `gap` leaves unallocated clusters between
// each link, so the chain is genuinely scattered rather than a range wearing a
// chain's clothes — reading it in order is then a real test of the FAT walk.
static uint32_t alloc_chain(Vol &v, uint32_t n, uint32_t gap = 0) {
    if (!n) return 0;
    std::vector<uint32_t> cls;
    for (uint32_t i = 0; i < n; i++) {
        cls.push_back(v.next_free);
        v.next_free += 1 + gap;
    }
    for (uint32_t i = 0; i + 1 < n; i++) fat_set(v, cls[i], cls[i + 1]);
    fat_set(v, cls.back(), eoc(v));
    v.used += n;
    return cls[0];
}

static uint32_t cluster_lba(const Vol &v, uint32_t cl) {
    return v.first_data + (cl - 2) * v.spc;
}

static void write_chain_data(Vol &v, uint32_t first, const std::string &data) {
    uint32_t cl = first, done = 0;
    const uint32_t csize = v.spc * FATRO_SECTOR;
    while (done < data.size() && cl >= 2) {
        for (uint32_t s = 0; s < v.spc && done < data.size(); s++) {
            uint32_t n = (uint32_t)data.size() - done;
            if (n > FATRO_SECTOR) n = FATRO_SECTOR;
            v.dev->put(cluster_lba(v, cl) + s, (const uint8_t *)data.data() + done, 0, n);
            done += n;
        }
        if (done >= data.size()) break;
        // Follow what alloc_chain laid down.
        uint32_t off, next = 0;
        if (v.type == 12) {
            off = cl + cl / 2;
            uint8_t *a = v.dev->at(v.first_fat + off / FATRO_SECTOR);
            uint8_t *b = v.dev->at(v.first_fat + (off + 1) / FATRO_SECTOR);
            uint16_t raw = (uint16_t)(a[off % FATRO_SECTOR] | (b[(off + 1) % FATRO_SECTOR] << 8));
            next = (cl & 1) ? (uint32_t)(raw >> 4) : (uint32_t)(raw & 0x0FFF);
        } else if (v.type == 16) {
            off = cl * 2;
            uint8_t *p = v.dev->at(v.first_fat + off / FATRO_SECTOR);
            next = (uint32_t)(p[off % FATRO_SECTOR] | (p[off % FATRO_SECTOR + 1] << 8));
        } else {
            off = cl * 4;
            uint8_t *p = v.dev->at(v.first_fat + off / FATRO_SECTOR);
            uint8_t *q = p + off % FATRO_SECTOR;
            next = ((uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                    ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24)) & 0x0FFFFFFF;
        }
        cl = next;
        (void)csize;
    }
}

// A directory, built as a flat run of 32-byte entries and flushed at the end.
struct DirBuf { std::vector<uint8_t> e; };

static uint8_t lfn_checksum(const char *n83) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)n83[i]);
    return sum;
}

static void dir_add(DirBuf &d, const char *n83, const char *lname, uint8_t attr,
                    uint32_t first_cluster, uint32_t size) {
    if (lname && *lname) {
        // The long-name entries go in immediately BEFORE the 8.3 one and in
        // reverse order, each numbered from 1, the last written carrying 0x40.
        uint32_t len = (uint32_t)strlen(lname);
        uint32_t nent = (len + 12) / 13;
        uint8_t sum = lfn_checksum(n83);
        static const uint8_t off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        for (uint32_t seq = nent; seq >= 1; seq--) {
            uint8_t ent[32];
            memset(ent, 0, sizeof(ent));
            ent[0] = (uint8_t)(seq | (seq == nent ? 0x40 : 0));
            ent[11] = 0x0F;
            ent[13] = sum;
            bool pad = false;
            for (uint32_t i = 0; i < 13; i++) {
                uint32_t idx = (seq - 1) * 13 + i;
                uint16_t c;
                if (idx < len)        c = (uint16_t)(uint8_t)lname[idx];
                else if (!pad)      { c = 0x0000; pad = true; }
                else                  c = 0xFFFF;
                wr16(ent + off[i], c);
            }
            d.e.insert(d.e.end(), ent, ent + 32);
        }
    }
    uint8_t ent[32];
    memset(ent, 0, sizeof(ent));
    memcpy(ent, n83, 11);
    ent[11] = attr;
    wr16(ent + 22, 0x6000);     // time 12:00:00
    wr16(ent + 24, 0x5CE4);     // date 2026-07-04
    wr16(ent + 20, (uint16_t)(first_cluster >> 16));
    wr16(ent + 26, (uint16_t)(first_cluster & 0xFFFF));
    wr32(ent + 28, size);
    d.e.insert(d.e.end(), ent, ent + 32);
}

// Write a directory into a cluster chain, allocating as many as it needs.
static uint32_t flush_dir_chain(Vol &v, DirBuf &d, uint32_t known_cluster = 0) {
    const uint32_t csize = v.spc * FATRO_SECTOR;
    uint32_t need = (uint32_t)((d.e.size() + csize) / csize);   // +1 for the 0x00 terminator
    uint32_t first = known_cluster ? known_cluster : alloc_chain(v, need);
    std::string bytes((const char *)d.e.data(), d.e.size());
    bytes.resize(need * csize, '\0');
    write_chain_data(v, first, bytes);
    return first;
}

static void flush_root_fixed(Vol &v, DirBuf &d) {
    uint32_t root_sectors = (v.root_entries * 32 + FATRO_SECTOR - 1) / FATRO_SECTOR;
    for (uint32_t i = 0; i * 32 < d.e.size() && i < v.root_entries; i++) {
        uint32_t lba = v.first_root + (i * 32) / FATRO_SECTOR;
        v.dev->put(lba, d.e.data() + i * 32, (i * 32) % FATRO_SECTOR, 32);
    }
    (void)root_sectors;
}

// Lay out the boot sector and the geometry for `clusters` data clusters.
static Vol make_vol(Dev &dev, int type, uint32_t clusters, uint32_t spc,
                    uint32_t base, const char *label) {
    Vol v{};
    v.dev = &dev;
    v.base = base;
    v.type = type;
    v.spc = spc;
    v.nfats = 2;
    v.rsvd = (type == 32) ? 32 : 1;
    v.root_entries = (type == 32) ? 0 : 512;
    v.clusters = clusters;

    uint32_t bytes_per_entry_x2 = (type == 12) ? 3 : (type == 16) ? 4 : 8;
    uint32_t fat_bytes = ((clusters + 2) * bytes_per_entry_x2 + 1) / 2;
    v.fat_sectors = (fat_bytes + FATRO_SECTOR - 1) / FATRO_SECTOR;

    uint32_t root_sectors = (v.root_entries * 32 + FATRO_SECTOR - 1) / FATRO_SECTOR;
    v.first_fat  = base + v.rsvd;
    v.first_root = v.first_fat + v.nfats * v.fat_sectors;
    v.first_data = v.first_root + root_sectors;
    v.total = (v.first_data - base) + clusters * spc;
    v.next_free = 2;

    uint8_t *b = dev.at(base);
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);
    wr16(b + 11, FATRO_SECTOR);
    b[13] = (uint8_t)spc;
    wr16(b + 14, (uint16_t)v.rsvd);
    b[16] = (uint8_t)v.nfats;
    wr16(b + 17, (uint16_t)v.root_entries);
    if (v.total < 0x10000) wr16(b + 19, (uint16_t)v.total);
    else                   wr32(b + 32, v.total);
    b[21] = 0xF8;
    if (type == 32) {
        wr32(b + 36, v.fat_sectors);
        wr32(b + 44, 2);                 // BPB_RootClus
        wr16(b + 48, 1);                 // BPB_FSInfo, one sector in
        wr16(b + 50, 6);                 // BPB_BkBootSec
        b[66] = 0x29;
        memcpy(b + 71, "           ", 11);
        memcpy(b + 71, label, strlen(label));
        memcpy(b + 82, "FAT32   ", 8);
    } else {
        wr16(b + 22, (uint16_t)v.fat_sectors);
        b[38] = 0x29;
        memcpy(b + 43, "           ", 11);
        memcpy(b + 43, label, strlen(label));
        memcpy(b + 54, type == 12 ? "FAT12   " : "FAT16   ", 8);
    }
    b[510] = 0x55; b[511] = 0xAA;

    // The reserved entries. Cluster 0 carries the media byte, cluster 1 is an
    // end-of-chain marker; every implementation writes them and one that does
    // not is a red flag to fsck.
    fat_set(v, 0, type == 12 ? 0xFF8 : type == 16 ? 0xFFF8 : 0x0FFFFFF8);
    fat_set(v, 1, eoc(v));
    v.next_free = 2;
    v.used = 0;

    // FAT32 keeps a second copy of the boot sector at BPB_BkBootSec, and a host
    // repair tool treats its absence as damage. Copied here, after the sector is
    // finished, so the two cannot disagree.
    if (type == 32) memcpy(dev.at(base + 6), dev.at(base), FATRO_SECTOR);
    return v;
}

static void add_fsinfo(Vol &v, uint32_t free_clusters) {
    uint8_t *p = v.dev->at(v.base + 1);
    wr32(p + 0, 0x41615252);
    wr32(p + 484, 0x61417272);
    wr32(p + 488, free_clusters);
    wr32(p + 492, 0xFFFFFFFF);
    wr32(p + 508, 0xAA550000);
}

static void add_mbr(Dev &dev, uint32_t start, uint32_t count, uint8_t type) {
    uint8_t *b = dev.at(0);
    memset(b, 0, FATRO_SECTOR);
    uint8_t *p = b + 0x1BE;
    p[0] = 0x80;          // bootable
    p[4] = type;
    wr32(p + 8, start);
    wr32(p + 12, count);
    b[510] = 0x55; b[511] = 0xAA;
}

// --- an opinion that is not this code's own ----------------------------------
//
// The same argument fatimage_test makes: a volume built from the same reading
// of the format as the reader shares the reader's misunderstandings. If this
// hand-made FAT32 image is subtly illegal, both sides of the test agree and
// both are wrong — and the failure on hardware is silent. fsck.fat has no such
// sympathy. Skipped, loudly, when it is not installed.
static int g_fsck_runs;

// dosfstools installs as fsck.fat and, on Debian, also as fsck.vfat — and
// /sbin is not on every user's PATH, which is how the same tool goes missing on
// a machine that has it. Same search fatimage_test does, plus the vfat name.
static const char *find_fsck(void) {
    static const char *found = nullptr;
    static bool looked = false;
    if (looked) return found;
    looked = true;
    static const char *tries[] = {
        "/sbin/fsck.fat", "/usr/sbin/fsck.fat", "/sbin/fsck.vfat", "/usr/sbin/fsck.vfat",
    };
    for (const char *t : tries) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "test -x %s", t);
        if (system(cmd) == 0) { found = t; return found; }
    }
    if (system("command -v fsck.fat >/dev/null 2>&1") == 0) found = "fsck.fat";
    return found;
}

static void fsck_partition(Dev &dev, uint32_t base, uint32_t sectors, const char *what) {
    char path[] = "/tmp/fatro_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("    (could not make a temp file, skipping fsck)\n"); return; }
    FILE *fp = fdopen(fd, "wb");
    if (!fp) { close(fd); unlink(path); return; }
    // Sparse: seek past the sectors nothing wrote, so a 32 MB volume costs the
    // few kilobytes that actually have anything in them.
    for (auto &kv : dev.s) {
        if (kv.first < base || kv.first >= base + sectors) continue;
        if (fseek(fp, (long)(kv.first - base) * FATRO_SECTOR, SEEK_SET) != 0) break;
        fwrite(kv.second.b, 1, FATRO_SECTOR, fp);
    }
    fseek(fp, (long)sectors * FATRO_SECTOR - 1, SEEK_SET);
    fputc(0, fp);
    fclose(fp);

    const char *fsck = find_fsck();
    if (!fsck) {
        printf("    (fsck.fat not installed - %s unverified by an outsider)\n", what);
    } else {
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "%s -n %s > %s.out 2>&1", fsck, path, path);
        int rc = system(cmd);
        g_fsck_runs++;
        checks++;
        if (!WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
            printf("  FAIL: fsck.fat rejects the %s volume this test builds\n", what);
            char out[128];
            snprintf(out, sizeof(out), "%s.out", path);
            FILE *o = fopen(out, "r");
            if (o) { char line[200]; while (fgets(line, sizeof(line), o)) printf("      %s", line); fclose(o); }
            fails++;
        }
    }
    char rm[300];
    snprintf(rm, sizeof(rm), "rm -f %s %s.out", path, path);
    if (system(rm)) { /* nothing to do about it */ }
}

// --- collecting a listing ----------------------------------------------------

struct Listing { std::vector<std::string> names; std::vector<uint32_t> sizes;
                 std::vector<int> dirs; };

static void list_cb(void *ctx, const FatRoEntry *e) {
    Listing *l = (Listing *)ctx;
    l->names.push_back(e->name);
    l->sizes.push_back(e->size);
    l->dirs.push_back(e->is_dir ? 1 : 0);
}

static int index_of(const Listing &l, const char *name) {
    for (size_t i = 0; i < l.names.size(); i++) if (l.names[i] == name) return (int)i;
    return -1;
}

static std::string read_all(FatRo &f, const FatRoEntry &e) {
    std::string out;
    FatRoCursor cur; fatro_cursor_init(&cur);
    char buf[300];
    uint32_t off = 0;
    while (true) {
        uint32_t n = fatro_read(&f, &e, &cur, off, buf, sizeof(buf));
        if (!n) break;
        out.append(buf, n);
        off += n;
    }
    return out;
}

// --- the volumes ------------------------------------------------------------

static std::string pattern(uint32_t n, char seed) {
    std::string s;
    s.reserve(n);
    for (uint32_t i = 0; i < n; i++) s.push_back((char)('a' + ((i + seed) % 26)));
    return s;
}

static void test_fat16(void) {
    printf("  FAT16, unpartitioned\n");
    Dev dev;
    // 5000 clusters: over 4084 so it is FAT16 and not FAT12, and comfortably
    // under 65525 so it is FAT16 and not FAT32.
    Vol v = make_vol(dev, 16, 5000, 1, 0, "CARD16");

    std::string readme = pattern(1200, 3);
    uint32_t readme_cl = alloc_chain(v, 3, 1);       // 3 clusters, scattered
    write_chain_data(v, readme_cl, readme);

    // The cluster comes FIRST, because '.' has to point at the directory it is
    // in and '..' at its parent — 0 when the parent is the root. fsck.fat calls
    // anything else an invalid entry, and it is right to.
    uint32_t sub_cl = alloc_chain(v, 1);
    DirBuf sub;
    dir_add(sub, ".          ", nullptr, 0x10, sub_cl, 0);
    dir_add(sub, "..         ", nullptr, 0x10, 0, 0);
    std::string note = "inside a subdirectory\n";
    uint32_t note_cl = alloc_chain(v, 1);
    write_chain_data(v, note_cl, note);
    dir_add(sub, "NOTE    TXT", nullptr, 0x20, note_cl, (uint32_t)note.size());
    flush_dir_chain(v, sub, sub_cl);

    DirBuf root;
    dir_add(root, "CARD16     ", nullptr, 0x08, 0, 0);       // volume label
    dir_add(root, "README  TXT", nullptr, 0x20, readme_cl, (uint32_t)readme.size());
    dir_add(root, "EMPTY   TXT", nullptr, 0x20, 0, 0);
    dir_add(root, "SUB        ", nullptr, 0x10, sub_cl, 0);
    flush_root_fixed(v, root);

    fsck_partition(dev, 0, v.total, "FAT16");

    FatRo f;
    FatRoIo io{&dev, dev_read};
    ck(fatro_mount(&f, &io), "FAT16 volume mounts");
    cks(fatro_type_name(&f), "FAT16", "reported as FAT16");
    cks(f.label, "CARD16", "label comes from the root directory entry");
    ck(f.part_lba == 0, "unpartitioned: the volume starts at LBA 0");
    ck(fatro_total_bytes(&f) == 5000ull * 512, "capacity is clusters x cluster size");

    Listing l;
    ck(fatro_list(&f, "/", list_cb, &l), "root lists");
    ck(l.names.size() == 3, "the volume label is not a file");
    ck(index_of(l, "README.TXT") >= 0, "README.TXT is listed");
    ck(index_of(l, "EMPTY.TXT") >= 0, "EMPTY.TXT is listed");
    int si = index_of(l, "SUB");
    ck(si >= 0 && l.dirs[(size_t)si] == 1, "SUB is listed as a directory");

    FatRoEntry e;
    ck(fatro_stat(&f, "/README.TXT", &e), "stat finds a root file");
    ck(e.size == 1200, "size comes from the directory entry");
    ck(read_all(f, e) == readme, "a three-cluster scattered file reads back whole");

    Listing sl;
    ck(fatro_list(&f, "/SUB", list_cb, &sl), "a subdirectory lists");
    ck(sl.names.size() == 1, "'.' and '..' are not entries anybody wants");
    ck(fatro_stat(&f, "/sub/note.txt", &e), "paths are case-insensitive");
    ck(read_all(f, e) == note, "a file inside a subdirectory reads back");

    ck(!fatro_stat(&f, "/nope.txt", &e), "a missing file is not found");
    ck(!fatro_list(&f, "/README.TXT", list_cb, &sl), "a file is not a directory");

    // The empty file has no cluster at all, which is the case a reader that
    // assumes every file owns one walks off the end of.
    ck(fatro_stat(&f, "/EMPTY.TXT", &e), "an empty file stats");
    ck(fatro_read(&f, &e, nullptr, 0, (void *)&checks, 1) == 0, "and reads as nothing");
}

static void test_fat32(void) {
    printf("  FAT32, partitioned, volume at LBA 2048\n");
    Dev dev;
    // 65600 clusters is a LEGAL FAT32 volume: the line is 65525 and below it
    // the format is FAT16 whatever the boot sector claims. At 512-byte clusters
    // that is 32 MB of addresses, which only exist as far as anything writes to
    // them — the device is sparse.
    const uint32_t part = 2048;
    Vol v = make_vol(dev, 32, 65600, 1, part, "NOVACARD");
    add_mbr(dev, part, v.total, 0x0C);
    // On FAT32 the root directory OWNS cluster 2 — BPB_RootClus above says so —
    // so allocation starts after it. Handing cluster 2 out to a file writes the
    // file and the root on top of each other, which is a test bug that looks
    // exactly like a reader following the wrong chain.
    fat_set(v, 2, eoc(v));
    v.used = 1;
    v.next_free = 3;

    std::string big = pattern(4000, 7);              // eight clusters
    uint32_t big_cl = alloc_chain(v, 8, 2);          // deliberately scattered
    write_chain_data(v, big_cl, big);

    uint32_t dcim_cl = alloc_chain(v, 1);
    uint32_t sub_cl  = alloc_chain(v, 1);

    DirBuf sub;
    dir_add(sub, ".          ", nullptr, 0x10, sub_cl, 0);
    dir_add(sub, "..         ", nullptr, 0x10, dcim_cl, 0);
    std::string deep = "two levels down\n";
    uint32_t deep_cl = alloc_chain(v, 1);
    write_chain_data(v, deep_cl, deep);
    dir_add(sub, "DEEP    TXT", nullptr, 0x20, deep_cl, (uint32_t)deep.size());
    flush_dir_chain(v, sub, sub_cl);

    DirBuf dcim;
    dir_add(dcim, ".          ", nullptr, 0x10, dcim_cl, 0);
    // The root is cluster 2 here, and a '..' pointing INTO the root is still
    // written as 0. The format says so and fsck.fat checks it.
    dir_add(dcim, "..         ", nullptr, 0x10, 0, 0);
    dir_add(dcim, "SUB        ", nullptr, 0x10, sub_cl, 0);
    flush_dir_chain(v, dcim, dcim_cl);

    DirBuf root;
    dir_add(root, "NOVACARD   ", nullptr, 0x08, 0, 0);
    dir_add(root, "BIG     BIN", nullptr, 0x20, big_cl, (uint32_t)big.size());
    dir_add(root, "DCIM       ", nullptr, 0x10, dcim_cl, 0);
    // A name 8.3 cannot hold. "capture-2026-07-04.wav" truncated to CAPTUR~1.WAV
    // is not the file anyone asked for, so the long form has to come back.
    std::string wav = pattern(700, 11);
    uint32_t wav_cl = alloc_chain(v, 2);
    write_chain_data(v, wav_cl, wav);
    dir_add(root, "CAPTUR~1WAV", "capture-2026-07-04.wav", 0x20, wav_cl,
            (uint32_t)wav.size());
    // The root of a FAT32 volume is cluster 2 by the boot sector above, so it
    // is written there rather than allocated.
    flush_dir_chain(v, root, 2);

    // FSInfo LAST, once the true count is known. A stale free count is not
    // cosmetic — it is what fw_storage_roots reports as free space, and fsck
    // treats a wrong one as damage.
    add_fsinfo(v, v.clusters - v.used);

    fsck_partition(dev, part, v.total, "FAT32");

    FatRo f;
    FatRoIo io{&dev, dev_read};
    ck(fatro_mount(&f, &io), "a partitioned FAT32 card mounts");
    cks(fatro_type_name(&f), "FAT32", "reported as FAT32");
    ck(f.part_lba == part, "the volume was found through the partition table");
    ck(f.root_cluster == 2, "the root is a cluster chain, not a fixed region");
    cks(f.label, "NOVACARD", "label from the root directory");
    ck(fatro_total_bytes(&f) == 65600ull * 512, "capacity");
    ck(fatro_free_bytes(&f) == (uint64_t)(65600 - v.used) * 512,
       "free space comes from FSInfo");

    Listing l;
    ck(fatro_list(&f, "/", list_cb, &l), "root lists");
    ck(l.names.size() == 3, "three entries, the label not among them");
    ck(index_of(l, "capture-2026-07-04.wav") >= 0, "the long name is reassembled");

    FatRoEntry e;
    ck(fatro_stat(&f, "/BIG.BIN", &e), "stat a root file");
    ck(read_all(f, e) == big, "an eight-cluster scattered file reads back whole");

    // Off-cursor and mid-file, which is what a media player seeking does.
    char part_buf[100];
    uint32_t n = fatro_read(&f, &e, nullptr, 1500, part_buf, sizeof(part_buf));
    ck(n == 100 && memcmp(part_buf, big.data() + 1500, 100) == 0,
       "a read from the middle of a chain lands in the right place");
    n = fatro_read(&f, &e, nullptr, 3950, part_buf, sizeof(part_buf));
    ck(n == 50, "a read past the end is short, not wrong");
    ck(fatro_read(&f, &e, nullptr, 4000, part_buf, sizeof(part_buf)) == 0,
       "and past the end entirely, nothing");

    ck(fatro_stat(&f, "/DCIM/SUB/DEEP.TXT", &e), "two levels of subdirectory");
    ck(read_all(f, e) == deep, "and the file down there reads");
    ck(fatro_stat(&f, "/capture-2026-07-04.wav", &e), "a long name resolves as a path");
    ck(read_all(f, e) == wav, "and reads");

    // The cursor is the difference between linear and quadratic on a card, so
    // it has to give the same bytes as walking from the start every time.
    FatRoCursor cur; fatro_cursor_init(&cur);
    std::string seq;
    char sbuf[64];
    for (uint32_t off = 0; off < 4000; off += 64) {
        uint32_t got = fatro_read(&f, &e, &cur, off, sbuf, sizeof(sbuf));
        (void)got;
    }
    fatro_cursor_init(&cur);
    ck(fatro_stat(&f, "/BIG.BIN", &e), "re-stat");
    uint32_t off = 0;
    while (true) {
        uint32_t got = fatro_read(&f, &e, &cur, off, sbuf, sizeof(sbuf));
        if (!got) break;
        seq.append(sbuf, got);
        off += got;
    }
    ck(seq == big, "a cursored sequential read matches a cold one");

    uint32_t before = dev.reads;
    fatro_read(&f, &e, &cur, 0, sbuf, sizeof(sbuf));
    fatro_cursor_init(&cur);
    uint32_t cold = dev.reads - before;
    ck(cold > 0, "reads actually reach the device");
}

static void test_fat12(void) {
    printf("  FAT12, the small-card case\n");
    Dev dev;
    Vol v = make_vol(dev, 12, 1000, 1, 0, "TINY");
    // A 12-bit entry can straddle a sector boundary, so the chain is made long
    // enough and scattered enough to cross one.
    std::string data = pattern(3000, 5);
    uint32_t cl = alloc_chain(v, 6, 170);      // spreads across FAT sectors
    write_chain_data(v, cl, data);

    DirBuf root;
    dir_add(root, "TINY       ", nullptr, 0x08, 0, 0);
    dir_add(root, "DATA    BIN", nullptr, 0x20, cl, (uint32_t)data.size());
    flush_root_fixed(v, root);

    fsck_partition(dev, 0, v.total, "FAT12");

    FatRo f;
    FatRoIo io{&dev, dev_read};
    ck(fatro_mount(&f, &io), "FAT12 mounts");
    cks(fatro_type_name(&f), "FAT12", "under 4085 clusters it IS FAT12");
    FatRoEntry e;
    ck(fatro_stat(&f, "/DATA.BIN", &e), "stat");
    ck(read_all(f, e) == data, "a chain whose entries straddle sector boundaries reads");
    ck(fatro_free_bytes(&f) == (uint64_t)(1000 - v.used) * 512,
       "a small table is cheap enough to count honestly");
    cks(f.label, "TINY", "label from the root directory");

    // And the fallback, for a volume whose root carries no label entry: the
    // copy in the boot sector. Done by deleting the entry AFTER fsck has had
    // its look, since fsck considers a boot label with no root entry a defect.
    dev.at(v.first_root)[0] = 0xE5;
    FatRo f2;
    ck(fatro_mount(&f2, &io), "still mounts with the label entry gone");
    cks(f2.label, "TINY", "label falls back to BS_VolLab in the boot sector");
}

static void test_refusals(void) {
    printf("  what it refuses\n");
    FatRo f;

    Dev blank;
    FatRoIo io1{&blank, dev_read};
    ck(!fatro_mount(&f, &io1), "an unformatted card does not mount");

    Dev ex;
    uint8_t *b = ex.at(0);
    b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
    memcpy(b + 3, "EXFAT   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    FatRoIo io2{&ex, dev_read};
    ck(!fatro_mount(&f, &io2), "exFAT is refused rather than misread");

    // A partition table pointing at nothing. The signature is there and the
    // entry looks plausible, and there is no volume where it says.
    Dev bad;
    add_mbr(bad, 4096, 100000, 0x0C);
    FatRoIo io3{&bad, dev_read};
    ck(!fatro_mount(&f, &io3), "a partition entry pointing at nothing does not mount");
}

static void test_removal(void) {
    printf("  a card pulled out mid-operation\n");
    Dev dev;
    Vol v = make_vol(dev, 16, 5000, 1, 0, "GONE");
    std::string data = pattern(2000, 1);
    uint32_t cl = alloc_chain(v, 4, 1);
    write_chain_data(v, cl, data);
    DirBuf root;
    dir_add(root, "FILE    BIN", nullptr, 0x20, cl, (uint32_t)data.size());
    flush_root_fixed(v, root);

    FatRo f;
    FatRoIo io{&dev, dev_read};
    ck(fatro_mount(&f, &io), "mounts while the card is in");
    FatRoEntry e;
    ck(fatro_stat(&f, "/FILE.BIN", &e), "stats while the card is in");

    dev.gone = true;
    Listing l;
    ck(!fatro_list(&f, "/", list_cb, &l), "listing a removed card fails");
    ck(l.names.empty(), "and reports no entries rather than partial ones");
    FatRoEntry e2;
    ck(!fatro_stat(&f, "/FILE.BIN", &e2), "stat on a removed card fails");
    char buf[64];
    ck(fatro_read(&f, &e, nullptr, 0, buf, sizeof(buf)) == 0,
       "reading a removed card returns nothing");
    // Every one of those has to RETURN. A retry loop with no way out is the
    // failure mode here and it presents as the device hanging.
    ck(true, "and every one of them returned");

    dev.gone = false;
    ck(fatro_stat(&f, "/FILE.BIN", &e2), "and it works again when the card is back");
    ck(read_all(f, e2) == data, "with the same bytes");
}

int main(void) {
    printf("fatro_test - the SD card's filesystem reader\n");
    test_fat16();
    test_fat32();
    test_fat12();
    test_refusals();
    test_removal();
    if (g_fsck_runs) printf("  fsck.fat agreed about %d volume(s)\n", g_fsck_runs);
    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
