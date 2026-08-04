#include "fat12.h"
#include "fatview.h"

#include <stdio.h>
#include <string.h>

// Byte order. FAT is little-endian and so is the target, but a struct overlay
// would still depend on the compiler adding no padding to a layout that has
// none. Byte at a time is the version that cannot be wrong.
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define F12_FREE 0x000
#define F12_EOC  0xFFF          // anything >= 0xFF8 ends a chain
#define DIRENT   32

// --- the twelve-bit table ---------------------------------------------------
//
// The part of FAT12 that is genuinely different, and the part worth being
// careful about: entries are twelve bits, so they pack two into every three
// bytes and half of them straddle a byte boundary. An entry can also straddle a
// SECTOR boundary, which is the case that is easy to forget and produces a
// volume that is correct until it is nearly full.

static bool fat_byte(F12 *f, uint32_t off, uint8_t *out) {
    uint8_t sec[F12_SECTOR];
    uint32_t lba = f->first_fat + off / F12_SECTOR;
    if (!f->io.read(f->io.ctx, lba, sec)) return false;
    *out = sec[off % F12_SECTOR];
    return true;
}

static bool fat_byte_set(F12 *f, uint32_t off, uint8_t mask, uint8_t value) {
    uint8_t sec[F12_SECTOR];
    uint32_t lba = f->first_fat + off / F12_SECTOR;
    if (!f->io.read(f->io.ctx, lba, sec)) return false;
    uint8_t *b = &sec[off % F12_SECTOR];
    *b = (uint8_t)((*b & ~mask) | (value & mask));
    return f->io.write(f->io.ctx, lba, sec);
}

static uint16_t fat_get(F12 *f, uint32_t cluster) {
    uint32_t off = cluster + cluster / 2;      // cluster * 1.5
    uint8_t a = 0, b = 0;
    if (!fat_byte(f, off, &a) || !fat_byte(f, off + 1, &b)) return F12_EOC;
    // Even clusters take the low twelve bits of the pair, odd ones the high.
    return (cluster & 1) ? (uint16_t)((a >> 4) | (b << 4))
                         : (uint16_t)(a | ((b & 0x0F) << 8));
}

static bool fat_set(F12 *f, uint32_t cluster, uint16_t value) {
    uint32_t off = cluster + cluster / 2;
    value &= 0x0FFF;
    if (cluster & 1) {
        if (!fat_byte_set(f, off, 0xF0, (uint8_t)(value << 4))) return false;
        return fat_byte_set(f, off + 1, 0xFF, (uint8_t)(value >> 4));
    }
    if (!fat_byte_set(f, off, 0xFF, (uint8_t)value)) return false;
    return fat_byte_set(f, off + 1, 0x0F, (uint8_t)(value >> 8));
}

// --- mounting ---------------------------------------------------------------

bool f12_mount(F12 *f, const F12Io *io) {
    memset(f, 0, sizeof(*f));
    f->io = *io;

    uint8_t b[F12_SECTOR];
    if (!io->read(io->ctx, 0, b)) return false;
    if (b[510] != 0x55 || b[511] != 0xAA) return false;
    if (rd16(b + 11) != F12_SECTOR) return false;          // a different sector size

    f->spc          = b[13];
    f->reserved     = rd16(b + 14);
    f->num_fats     = b[16];
    f->root_entries = rd16(b + 17);
    f->fat_sectors  = rd16(b + 22);
    if (!f->spc || !f->reserved || !f->num_fats || !f->fat_sectors) return false;

    uint32_t total = rd16(b + 19);
    if (!total) total = rd32(b + 32);
    if (!total || total > io->sectors) return false;

    uint32_t root_sectors = ((uint32_t)f->root_entries * DIRENT + F12_SECTOR - 1) / F12_SECTOR;
    f->first_fat  = f->reserved;
    f->first_root = f->first_fat + f->fat_sectors * f->num_fats;
    f->first_data = f->first_root + root_sectors;
    if (f->first_data >= total) return false;
    f->clusters = (total - f->first_data) / f->spc;

    // The cluster count IS the format. A volume with 4085 or more clusters is
    // FAT16 and every entry in it would be read here at the wrong bit offset,
    // which is not an error so much as confident nonsense — so it is refused
    // rather than misread.
    if (!f->clusters || f->clusters >= 4085) return false;

    f->mounted = true;
    return true;
}

static bool dir_write_slot(F12 *f, uint32_t slot, const uint8_t entry[DIRENT]) {
    const uint32_t per = F12_SECTOR / DIRENT;
    uint8_t sec[F12_SECTOR];
    uint32_t lba = f->first_root + slot / per;
    if (!f->io.read(f->io.ctx, lba, sec)) return false;
    memcpy(sec + (slot % per) * DIRENT, entry, DIRENT);
    return f->io.write(f->io.ctx, lba, sec);
}

bool f12_format(F12 *f, const F12Io *io, const char *label) {
    memset(f, 0, sizeof(*f));
    f->io = *io;

    f->spc          = 1;          // 512-byte clusters: the most clusters per byte
    f->reserved     = 1;
    f->num_fats     = 1;          // a second copy buys redundancy nothing here reads
    f->root_entries = 128;        // 8 sectors; a transfer area is not a filing cabinet

    uint32_t total = io->sectors;
    uint32_t root_sectors = ((uint32_t)f->root_entries * DIRENT + F12_SECTOR - 1) / F12_SECTOR;

    // The table's size depends on the cluster count and the cluster count
    // depends on the table's size, so this settles rather than solving.
    uint32_t fat_sectors = 1, clusters = 0;
    for (int pass = 0; pass < 8; pass++) {
        uint32_t overhead = f->reserved + fat_sectors * f->num_fats + root_sectors;
        if (total <= overhead) return false;
        clusters = (total - overhead) / f->spc;
        // Three bytes per two entries, plus the two reserved entries.
        uint32_t need = (((clusters + 2) * 3 + 1) / 2 + F12_SECTOR - 1) / F12_SECTOR;
        if (need == fat_sectors) break;
        fat_sectors = need;
    }
    if (!clusters) return false;

    // FAT12 has an upper bound as well as the format being decided by it. A
    // region large enough to cross 4085 clusters would have to be described as
    // FAT16, so the volume is capped instead and the remainder left unused —
    // one megabyte is nowhere near this, but the arithmetic should not depend
    // on that staying true.
    if (clusters > 4084) clusters = 4084;

    f->fat_sectors = fat_sectors;
    f->first_fat   = f->reserved;
    f->first_root  = f->first_fat + fat_sectors * f->num_fats;
    f->first_data  = f->first_root + root_sectors;
    f->clusters    = clusters;
    total = f->first_data + clusters * f->spc;

    uint8_t sec[F12_SECTOR];

    // The boot sector.
    memset(sec, 0, sizeof(sec));
    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    memcpy(sec + 3, "MSWIN4.1", 8);
    wr16(sec + 11, F12_SECTOR);
    sec[13] = f->spc;
    wr16(sec + 14, f->reserved);
    sec[16] = f->num_fats;
    wr16(sec + 17, f->root_entries);
    if (total < 0x10000) { wr16(sec + 19, (uint16_t)total); wr32(sec + 32, 0); }
    else                 { wr16(sec + 19, 0);               wr32(sec + 32, total); }
    sec[21] = 0xF8;
    wr16(sec + 22, (uint16_t)fat_sectors);
    wr16(sec + 24, 63);
    wr16(sec + 26, 255);
    sec[36] = 0x80;
    sec[38] = 0x29;
    wr32(sec + 39, 0x52504332);          // "RPC2"
    memset(sec + 43, ' ', 11);
    for (int i = 0; i < 11 && label && label[i]; i++) sec[43 + i] = (uint8_t)label[i];
    memcpy(sec + 54, "FAT12   ", 8);
    sec[510] = 0x55; sec[511] = 0xAA;
    if (!io->write(io->ctx, 0, sec)) return false;

    // An empty table, then an empty root directory.
    memset(sec, 0, sizeof(sec));
    for (uint32_t i = 0; i < fat_sectors * f->num_fats; i++)
        if (!io->write(io->ctx, f->first_fat + i, sec)) return false;
    for (uint32_t i = 0; i < root_sectors; i++)
        if (!io->write(io->ctx, f->first_root + i, sec)) return false;

    f->mounted = true;
    // The two reserved entries. The first repeats the media byte.
    if (!fat_set(f, 0, 0xFF8) || !fat_set(f, 1, 0xFFF)) return false;

    // The volume label, as a directory entry as well as a boot sector field.
    //
    // Both are required and they have to agree: a boot sector naming a volume
    // whose root directory carries no label entry is an inconsistency, and a
    // host that repairs filesystems will offer to fix it — by erasing the name
    // rather than adding the entry.
    uint8_t label_ent[DIRENT];
    fat_dirent_label(label_ent, label ? label : "RPCORTEX");
    return dir_write_slot(f, 0, label_ent);
}

// --- reading the directory --------------------------------------------------

// Pull the characters of one long-name entry into place.
static void lfn_gather(const uint8_t *e, char *name, uint32_t cap, bool *ok) {
    static const uint8_t off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    uint32_t seq = e[0] & 0x1F;
    if (!seq) { *ok = false; return; }
    uint32_t base = (seq - 1) * 13;
    for (int i = 0; i < 13; i++) {
        uint32_t idx = base + (uint32_t)i;
        uint16_t c = rd16(e + off[i]);
        if (c == 0xFFFF || c == 0x0000) continue;
        if (idx >= cap - 1) { *ok = false; return; }
        // Anything outside ASCII has no place in a name the shell will have to
        // print and accept as an argument, so the long name is abandoned and
        // the 8.3 form used instead.
        if (c > 0x7F) { *ok = false; return; }
        name[idx] = (char)c;
    }
}

// The 8.3 name as something printable, honouring the case bits.
static void short_name(const uint8_t *e, char *out) {
    int o = 0;
    bool lb = (e[12] & FAT_CASE_BASE_LOWER) != 0;
    bool le = (e[12] & FAT_CASE_EXT_LOWER) != 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)e[i];
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
    if (!month || !day) return 0;

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

// Walk the root directory, handing each real entry to `cb`. `stop_at` lets the
// caller finish early; `slot_out` reports where the entry was found.
struct DirScan {
    F12WalkFn cb;
    void     *ctx;
    const char *want;          // when searching
    F12Entry *found;
    int32_t   slot;            // where `want` was found, or the first free slot
    int32_t   free_run;        // start of a run of free slots long enough
    uint32_t  free_needed;
};

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

static bool dir_scan(F12 *f, DirScan *s) {
    uint8_t sec[F12_SECTOR];
    const uint32_t per = F12_SECTOR / DIRENT;
    uint32_t root_sectors = ((uint32_t)f->root_entries * DIRENT + F12_SECTOR - 1) / F12_SECTOR;

    char lname[F12_MAXNAME];
    bool lok = false;
    memset(lname, 0, sizeof(lname));

    s->slot = -1;
    s->free_run = -1;
    int32_t run_start = -1;
    uint32_t run_len = 0;

    for (uint32_t sn = 0; sn < root_sectors; sn++) {
        if (!f->io.read(f->io.ctx, f->first_root + sn, sec)) return false;
        for (uint32_t i = 0; i < per; i++) {
            uint32_t slot = sn * per + i;
            const uint8_t *e = sec + i * DIRENT;

            if (e[0] == 0x00 || e[0] == 0xE5) {
                // Free. Runs matter because a long name needs several
                // CONSECUTIVE slots — a scattered set of free ones will not do.
                if (run_start < 0) { run_start = (int32_t)slot; run_len = 0; }
                run_len++;
                if (s->free_needed && run_len >= s->free_needed && s->free_run < 0)
                    s->free_run = run_start;

                if (e[0] == 0x00) {
                    // A zero first byte means this slot has never been used, and
                    // so has every slot after it — the directory ends here.
                    //
                    // Which makes the run open-ended, and that is the point that
                    // was missed: the scan stopped here having counted a single
                    // free slot, so a name needing three was told the directory
                    // was full while all but a handful of it stood empty. Every
                    // file with a long name was refused on an empty volume.
                    int32_t start = run_start >= 0 ? run_start : (int32_t)slot;
                    if (s->free_needed && s->free_run < 0 &&
                        f->root_entries - (uint32_t)start >= s->free_needed)
                        s->free_run = start;
                    return true;
                }
                lok = false;
                memset(lname, 0, sizeof(lname));
                continue;
            }
            run_start = -1; run_len = 0;

            uint8_t attr = e[11];
            if ((attr & 0x0F) == 0x0F) {
                if (e[0] & 0x40) { memset(lname, 0, sizeof(lname)); lok = true; }
                if (lok) lfn_gather(e, lname, sizeof(lname), &lok);
                continue;
            }
            if (attr & 0x08) { lok = false; continue; }     // the volume label

            F12Entry ent;
            memset(&ent, 0, sizeof(ent));
            if (lok && lname[0]) snprintf(ent.name, sizeof(ent.name), "%s", lname);
            else                 short_name(e, ent.name);
            lok = false;
            memset(lname, 0, sizeof(lname));

            ent.is_dir = (attr & 0x10) != 0;
            ent.size = rd32(e + 28);
            ent.first_cluster = rd16(e + 26);
            ent.mtime = fat_epoch(rd16(e + 24), rd16(e + 22));

            if (s->want) {
                char sn83[13];
                short_name(e, sn83);
                if (ieq(ent.name, s->want) || ieq(sn83, s->want)) {
                    if (s->found) *s->found = ent;
                    s->slot = (int32_t)slot;
                    return true;
                }
            } else if (s->cb) {
                s->cb(s->ctx, &ent);
            }
        }
    }
    return true;
}

bool f12_list(F12 *f, F12WalkFn cb, void *ctx) {
    if (!f->mounted) return false;
    DirScan s;
    memset(&s, 0, sizeof(s));
    s.cb = cb; s.ctx = ctx;
    return dir_scan(f, &s);
}

bool f12_find(F12 *f, const char *name, F12Entry *out) {
    if (!f->mounted || !name) return false;
    DirScan s;
    memset(&s, 0, sizeof(s));
    s.want = name; s.found = out;
    if (!dir_scan(f, &s)) return false;
    return s.slot >= 0;
}

uint32_t f12_read(F12 *f, const F12Entry *e, uint32_t off, void *buf, uint32_t len) {
    if (!f->mounted || !e || e->is_dir) return 0;
    if (off >= e->size) return 0;
    if (off + len > e->size) len = e->size - off;

    const uint32_t bytes_per_cluster = (uint32_t)f->spc * F12_SECTOR;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;

    // Walk the chain to the cluster the read starts in. A transfer area holds
    // few and small files, so walking rather than indexing costs nothing worth
    // the bookkeeping an index would need.
    uint32_t cluster = e->first_cluster;
    uint32_t skip = off / bytes_per_cluster;
    while (skip-- && cluster >= 2 && cluster < 0xFF8) cluster = fat_get(f, cluster);

    uint32_t within = off % bytes_per_cluster;
    uint8_t sec[F12_SECTOR];

    while (done < len && cluster >= 2 && cluster < 0xFF8) {
        uint32_t lba = f->first_data + (cluster - 2) * f->spc + within / F12_SECTOR;
        if (!f->io.read(f->io.ctx, lba, sec)) break;
        uint32_t in_sector = within % F12_SECTOR;
        uint32_t n = F12_SECTOR - in_sector;
        if (n > len - done) n = len - done;
        memcpy(dst + done, sec + in_sector, n);
        done += n;
        within += n;
        if (within >= bytes_per_cluster) { within = 0; cluster = fat_get(f, cluster); }
    }
    return done;
}

// --- writing ----------------------------------------------------------------

static uint32_t alloc_cluster(F12 *f, uint32_t after) {
    // Linear from the start rather than from a rotating hint. A transfer area
    // is rewritten constantly and wear levelling is the flash layer's business,
    // not this one's; predictable placement is worth more here.
    for (uint32_t c = 2; c < f->clusters + 2; c++) {
        if (fat_get(f, c) != F12_FREE) continue;
        if (!fat_set(f, c, F12_EOC)) return 0;
        if (after && !fat_set(f, after, (uint16_t)c)) return 0;
        return c;
    }
    return 0;
}

static void free_chain(F12 *f, uint32_t cluster) {
    while (cluster >= 2 && cluster < 0xFF8) {
        uint32_t next = fat_get(f, cluster);
        if (!fat_set(f, cluster, F12_FREE)) return;
        cluster = next;
    }
}


bool f12_remove(F12 *f, const char *name) {
    if (!f->mounted) return false;
    F12Entry e;
    DirScan s;
    memset(&s, 0, sizeof(s));
    s.want = name; s.found = &e;
    if (!dir_scan(f, &s) || s.slot < 0) return false;

    free_chain(f, e.first_cluster);

    // Mark the 8.3 entry deleted, and any long-name entries in front of it —
    // leaving those behind gives the next file created here a name assembled
    // from someone else's.
    uint8_t buf[DIRENT];
    const uint32_t per = F12_SECTOR / DIRENT;
    uint8_t sec[F12_SECTOR];
    for (int32_t slot = s.slot; slot >= 0; slot--) {
        uint32_t lba = f->first_root + (uint32_t)slot / per;
        if (!f->io.read(f->io.ctx, lba, sec)) return false;
        uint8_t *e8 = sec + ((uint32_t)slot % per) * DIRENT;
        bool is_lfn = (e8[11] & 0x0F) == 0x0F;
        if (slot != s.slot && !is_lfn) break;      // reached someone else's entry
        memcpy(buf, e8, DIRENT);
        buf[0] = 0xE5;
        if (!dir_write_slot(f, (uint32_t)slot, buf)) return false;
        if (slot != s.slot && (e8[0] & 0x40)) break;   // that was the start of the name
        if (slot == s.slot) continue;
    }
    return true;
}

bool f12_write(F12 *f, const char *name, uint32_t size, uint32_t mtime,
               F12SourceFn src, void *ctx) {
    if (!f->mounted || !name || !*name) return false;

    // Replacing rather than duplicating. Two entries with one name is a volume
    // that behaves differently depending on which the host finds first.
    F12Entry existing;
    if (f12_find(f, name, &existing)) f12_remove(f, name);

    char name83[11];
    uint8_t case_flags = 0;
    if (!fat_shortname(name, name83, &case_flags)) return false;

    // How many entry slots: the 8.3 one, plus long-name entries when 8.3
    // cannot hold the name.
    FatNode probe;
    memset(&probe, 0, sizeof(probe));
    memcpy(probe.name, name83, 11);
    probe.case_flags = case_flags;
    probe.lname = name;
    uint32_t slots = fat_entries_for(&probe);

    DirScan s;
    memset(&s, 0, sizeof(s));
    s.free_needed = slots;
    if (!dir_scan(f, &s) || s.free_run < 0) return false;    // the directory is full

    // The data first. A directory entry pointing at clusters that were never
    // written is a file the host can see and cannot read, which is worse than
    // one that failed to appear.
    uint32_t first = 0, prev = 0, done = 0;
    const uint32_t bytes_per_cluster = (uint32_t)f->spc * F12_SECTOR;
    uint8_t sec[F12_SECTOR];

    while (done < size) {
        uint32_t c = alloc_cluster(f, prev);
        if (!c) { if (first) free_chain(f, first); return false; }   // out of room
        if (!first) first = c;
        prev = c;

        for (uint32_t sn = 0; sn < f->spc && done < size; sn++) {
            memset(sec, 0, sizeof(sec));       // the tail of the last sector
            uint32_t want = size - done;
            if (want > F12_SECTOR) want = F12_SECTOR;
            if (src(ctx, done, sec, want) != want) { free_chain(f, first); return false; }
            if (!f->io.write(f->io.ctx, f->first_data + (c - 2) * f->spc + sn, sec)) {
                free_chain(f, first);
                return false;
            }
            done += want;
        }
        (void)bytes_per_cluster;
    }

    // Then the entries, long name first and in reverse, then the 8.3 one.
    uint8_t entry[DIRENT];
    uint32_t lfn_total = slots - 1;
    for (uint32_t i = 0; i < lfn_total; i++) {
        // fat_dir_sector builds these for the synthesised view; here they are
        // needed one at a time, so the same layout is written directly.
        uint32_t seq = lfn_total - i;
        memset(entry, 0, sizeof(entry));
        entry[0]  = (uint8_t)(seq | (i == 0 ? 0x40 : 0));
        entry[11] = 0x0F;
        uint8_t sum = 0;
        for (int k = 0; k < 11; k++)
            sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)name83[k]);
        entry[13] = sum;
        static const uint8_t off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        uint32_t len = (uint32_t)strlen(name);
        for (uint32_t k = 0; k < 13; k++) {
            uint32_t idx = (seq - 1) * 13 + k;
            uint16_t ch = idx < len ? (uint16_t)(uint8_t)name[idx]
                                    : (idx == len ? 0x0000 : 0xFFFF);
            entry[off[k]]     = (uint8_t)(ch & 0xFF);
            entry[off[k] + 1] = (uint8_t)(ch >> 8);
        }
        if (!dir_write_slot(f, (uint32_t)s.free_run + i, entry)) return false;
    }

    fat_dirent(entry, name83, FAT_ATTR_ARCHIVE, first, size, mtime, case_flags);
    return dir_write_slot(f, (uint32_t)s.free_run + lfn_total, entry);
}

uint32_t f12_total_bytes(F12 *f) {
    return f->mounted ? f->clusters * f->spc * F12_SECTOR : 0;
}

uint32_t f12_free_bytes(F12 *f) {
    if (!f->mounted) return 0;
    uint32_t n = 0;
    for (uint32_t c = 2; c < f->clusters + 2; c++)
        if (fat_get(f, c) == F12_FREE) n++;
    return n * f->spc * F12_SECTOR;
}
