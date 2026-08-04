// The synthesised FAT16 volume.
//
// This is worth testing hard because none of it fails loudly. A wrong field in
// the boot sector, an off-by-one in the cluster arithmetic or a cluster count
// that lands on the FAT12 side of the boundary all produce the same symptom:
// a host that mounts the drive and shows nonsense, or refuses it with no reason
// given. There is no error path to observe on the device — the bytes are simply
// wrong, and the only place to catch that is here.
#include "../core/fatview.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *w) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", w); fails++; }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read one FAT entry through the sector builder, the way a host would.
static uint16_t fat_entry(const FatGeom *g, const FatNode *nodes, uint32_t n, uint32_t cluster) {
    uint8_t sec[FAT_SECTOR_SIZE];
    uint32_t per = FAT_SECTOR_SIZE / 2;
    fat_fat_sector(g, cluster / per, nodes, n, sec);
    return rd16(sec + (cluster % per) * 2);
}

int main(void) {
    printf("fatview_test - the synthesised FAT16 volume\n");

    // --- geometry -----------------------------------------------------------
    //
    // 2 MB is the real case: a Pico 2 W's filesystem. It is also the case that
    // lands just under the FAT16 floor, which is the entire reason the padding
    // exists.
    FatGeom g;
    ck(fat_geom_init(&g, 2u * 1024 * 1024), "a 2 MB volume can be described");
    ck(g.real_clusters < FAT16_MIN_DATA_CLUSTERS,
       "2 MB really is short of the FAT16 floor -- if this ever stops being true "
       "the padding below is untested rather than unnecessary");
    ck(g.data_clusters >= FAT16_MIN_DATA_CLUSTERS, "so the volume is padded up to it");
    ck(g.data_clusters >= 4085,
       "and clears the boundary the host uses to tell FAT16 from FAT12");
    ck(g.data_clusters < 65525, "while staying below the FAT32 boundary");

    // The regions have to tile the volume exactly: no gap, no overlap.
    ck(g.first_fat_sector == g.reserved_sectors, "the table follows the boot sector");
    ck(g.first_root_sector == g.first_fat_sector + g.fat_sectors * g.num_fats,
       "the root directory follows the table");
    ck(g.first_data_sector == g.first_root_sector + g.root_sectors,
       "and the data area follows the root directory");
    ck(g.total_sectors == g.first_data_sector + g.data_clusters * g.sectors_per_cluster,
       "the volume ends where the last cluster does");

    // The table must be large enough to hold an entry for every cluster it
    // claims. Too small and the last clusters have no entry at all, which is a
    // host reading someone else's sector as allocation data.
    ck(g.fat_sectors * (FAT_SECTOR_SIZE / 2) >= g.data_clusters + FAT_FIRST_DATA_CLUSTER,
       "the table covers every cluster the volume claims");

    // --- region classification ----------------------------------------------
    uint32_t idx, sic;
    ck(fat_region(&g, 0, &idx, &sic) == FAT_RGN_BOOT, "LBA 0 is the boot sector");
    ck(fat_region(&g, g.first_fat_sector, &idx, &sic) == FAT_RGN_FAT, "then the table");
    ck(fat_region(&g, g.first_root_sector, &idx, &sic) == FAT_RGN_ROOT &&
       idx == 0, "then the root directory, from its first sector");
    ck(fat_region(&g, g.first_data_sector, &idx, &sic) == FAT_RGN_DATA &&
       idx == FAT_FIRST_DATA_CLUSTER,
       "and the data area starts at cluster 2, not cluster 0");
    ck(fat_region(&g, g.total_sectors, &idx, &sic) == FAT_RGN_BEYOND,
       "one past the end is out of bounds");
    ck(fat_region(&g, g.first_data_sector - 1, &idx, &sic) == FAT_RGN_ROOT,
       "the sector before the data area is still the root directory");

    // Every sector in the volume classifies as something, and the data area's
    // cluster numbers advance one per sector at this cluster size.
    {
        bool ok = true;
        uint32_t prev = FAT_FIRST_DATA_CLUSTER;
        for (uint32_t lba = g.first_data_sector; lba < g.first_data_sector + 64; lba++) {
            if (fat_region(&g, lba, &idx, &sic) != FAT_RGN_DATA) { ok = false; break; }
            if (lba > g.first_data_sector && idx != prev + 1) { ok = false; break; }
            prev = idx;
        }
        ck(ok, "cluster numbers advance one per sector across the data area");
    }

    // --- the boot sector ----------------------------------------------------
    uint8_t boot[FAT_SECTOR_SIZE];
    fat_build_boot(&g, boot, "RPCORTEX", 0x12345678);

    ck(boot[510] == 0x55 && boot[511] == 0xAA, "the boot sector carries its signature");
    ck(boot[0] == 0xEB, "and something that looks like a jump instruction");
    ck(rd16(boot + 11) == FAT_SECTOR_SIZE, "bytes per sector");
    ck(boot[13] == g.sectors_per_cluster, "sectors per cluster");
    ck(rd16(boot + 14) == g.reserved_sectors, "reserved sectors");
    ck(boot[16] == g.num_fats, "number of tables");
    ck(rd16(boot + 17) == g.root_entries, "root directory entries");
    ck(rd16(boot + 22) == g.fat_sectors, "sectors per table");
    ck(boot[38] == 0x29, "the extended boot signature, without which the label is not read");

    // Exactly one of the two total-sector fields carries the number. Both set,
    // or neither, and hosts disagree about the volume's size.
    {
        uint32_t small = rd16(boot + 19), large = rd32(boot + 32);
        ck((small == 0) != (large == 0), "exactly one total-sector field is used");
        ck((small ? small : large) == g.total_sectors, "and it holds the real total");
    }

    // --- the table ----------------------------------------------------------
    FatNode nodes[4];
    memset(nodes, 0, sizeof(nodes));
    // A three-cluster file starting at 2, and a one-cluster file at 10.
    nodes[0].first_cluster = 2;  nodes[0].clusters = 3; nodes[0].size = 1200;
    memcpy(nodes[0].name, "HELLO   TXT", 11);
    nodes[1].first_cluster = 10; nodes[1].clusters = 1; nodes[1].size = 10;
    memcpy(nodes[1].name, "A       BIN", 11);

    ck(fat_entry(&g, nodes, 2, 0) == 0xFFF8, "entry 0 repeats the media byte");
    ck(fat_entry(&g, nodes, 2, 1) == 0xFFFF, "entry 1 is reserved");
    ck(fat_entry(&g, nodes, 2, 2) == 3, "a chain points at its next cluster");
    ck(fat_entry(&g, nodes, 2, 3) == 4, "and keeps going");
    ck(fat_entry(&g, nodes, 2, 4) >= 0xFFF8, "until the last cluster ends it");
    ck(fat_entry(&g, nodes, 2, 10) >= 0xFFF8, "a one-cluster file is immediately the end");
    ck(fat_entry(&g, nodes, 2, 5) == FAT16_FREE, "a cluster no node owns is free");

    // The padding that lifts the volume over the FAT16 floor must read as bad,
    // so the host neither counts it as space nor writes into it.
    ck(fat_entry(&g, nodes, 2, FAT_FIRST_DATA_CLUSTER + g.real_clusters) == FAT16_BAD,
       "the first cluster past real storage is marked bad");
    ck(fat_entry(&g, nodes, 2, FAT_FIRST_DATA_CLUSTER + g.data_clusters - 1) == FAT16_BAD,
       "and so is the last one the volume claims");
    ck(fat_entry(&g, nodes, 2, FAT_FIRST_DATA_CLUSTER + g.real_clusters - 1) != FAT16_BAD,
       "while the last REAL cluster is not -- the boundary is not off by one");

    // With more than one copy of the table, every copy must answer identically.
    {
        FatGeom two = g;
        two.num_fats = 2;
        two.first_root_sector = two.first_fat_sector + two.fat_sectors * 2;
        two.first_data_sector = two.first_root_sector + two.root_sectors;
        uint32_t a_idx, b_idx;
        fat_region(&two, two.first_fat_sector, &a_idx, nullptr);
        fat_region(&two, two.first_fat_sector + two.fat_sectors, &b_idx, nullptr);
        ck(a_idx == b_idx, "the second copy of the table mirrors the first");
    }

    // --- directory entries --------------------------------------------------
    uint8_t e[FAT_DIRENT_SIZE];
    fat_dirent(e, "HELLO   TXT", FAT_ATTR_READ_ONLY, 2, 1200, 0, 0);
    ck(memcmp(e, "HELLO   TXT", 11) == 0, "the name occupies the first eleven bytes");
    ck(e[11] == FAT_ATTR_READ_ONLY, "the attribute byte follows it");
    ck(rd16(e + 26) == 2, "the first cluster is where the host will look");
    ck(rd32(e + 28) == 1200, "and the size is the file's, not the cluster run's");
    ck(rd16(e + 20) == 0, "the high cluster word is zero, which is what makes it FAT16");

    fat_dirent_label(e, "RPCORTEX");
    ck(e[11] == FAT_ATTR_VOLUME_ID, "the label entry is marked as the volume id");
    ck(rd32(e + 28) == 0 && rd16(e + 26) == 0, "and has neither size nor cluster");

    fat_dirent_dot(e, false, 7, 3, 0);
    ck(e[0] == '.' && e[1] == ' ', "the dot entry is a single dot, space padded");
    ck(e[11] == FAT_ATTR_DIRECTORY, "marked as a directory");
    ck(rd16(e + 26) == 7, "pointing at the directory itself");
    fat_dirent_dot(e, true, 7, 0, 0);
    ck(e[0] == '.' && e[1] == '.', "the dotdot entry is two dots");
    ck(rd16(e + 26) == 0, "and names the root as cluster 0, as the format requires");

    // --- short names --------------------------------------------------------
    char n[11];
    uint8_t cf = 0;
    ck(fat_shortname("hello.txt", n, nullptr) && memcmp(n, "HELLO   TXT", 11) == 0,
       "a plain name is upper-cased and padded");
    ck(fat_shortname("a.b", n, nullptr) && memcmp(n, "A       B  ", 11) == 0,
       "a short name pads both halves");
    ck(fat_shortname("noext", n, nullptr) && memcmp(n, "NOEXT      ", 11) == 0,
       "a name with no extension leaves it blank");
    ck(fat_shortname("archive.tar.gz", n, nullptr) && memcmp(n + 8, "GZ ", 3) == 0,
       "the extension comes from the LAST dot, not the first");
    ck(fat_shortname("averylongname.text", n, nullptr) && memcmp(n, "AVERYLON", 8) == 0,
       "an over-long base is truncated to eight");
    ck(memcmp(n + 8, "TEX", 3) == 0, "and an over-long extension to three");
    ck(!fat_shortname(".hidden", n, nullptr), "a leading dot has no 8.3 form and is refused");
    ck(!fat_shortname("", n, nullptr), "and neither does an empty name");
    ck(!fat_shortname("...", n, nullptr), "nor one that is nothing but dots");
    ck(fat_shortname("a+b=c.txt", n, nullptr) && memcmp(n, "ABC     TXT", 11) == 0,
       "characters the format forbids are dropped rather than passed through");

    // --- the case bits ------------------------------------------------------
    //
    // The stored name is always upper case, so without these every file on the
    // drive appears shouted. They are the only way to say otherwise short of
    // long filename entries.
    ck(fat_shortname("ca.pem", n, &cf) &&
       (cf & FAT_CASE_BASE_LOWER) && (cf & FAT_CASE_EXT_LOWER),
       "an all-lower-case name is marked lower case in both halves");
    ck(fat_shortname("CA.PEM", n, &cf) && cf == 0,
       "an all-upper-case name is marked in neither");
    ck(fat_shortname("ca.PEM", n, &cf) &&
       (cf & FAT_CASE_BASE_LOWER) && !(cf & FAT_CASE_EXT_LOWER),
       "the two halves are decided independently");
    // Mixed case within one half has no representation at all, and guessing
    // would show a name that is not the file's.
    ck(fat_shortname("ReadMe.txt", n, &cf) && !(cf & FAT_CASE_BASE_LOWER),
       "a mixed-case half is left shouted rather than guessed at");
    ck(fat_shortname("readme.TxT", n, &cf) && !(cf & FAT_CASE_EXT_LOWER),
       "including when it is the extension that is mixed");
    ck(fat_shortname("stress.app", n, &cf) && (cf & FAT_CASE_BASE_LOWER),
       "digits and letters together still count as lower case");

    // And the flags have to reach the directory entry, which is byte 12.
    fat_shortname("ca.pem", n, &cf);
    fat_dirent(e, n, FAT_ATTR_ARCHIVE, 8, 4890, 0, cf);
    ck(e[12] == (FAT_CASE_BASE_LOWER | FAT_CASE_EXT_LOWER),
       "and land in byte 12 of the entry, where the host reads them");

    // --- host metadata ------------------------------------------------------
    ck(fat_is_host_metadata("System Volume Information"), "Windows' own directory is known");
    ck(fat_is_host_metadata("SYSTEM VOLUME INFORMATION"), "case does not matter");
    ck(fat_is_host_metadata(".Spotlight-V100"), "macOS' index is known");
    ck(fat_is_host_metadata(".Trashes"), "and its wastebasket");
    ck(fat_is_host_metadata("._myfile.txt"), "as is a resource fork companion");
    ck(!fat_is_host_metadata("notes.txt"), "an ordinary file is not metadata");
    ck(!fat_is_host_metadata("system"), "and neither is a prefix of one");

    // --- timestamps ---------------------------------------------------------
    uint16_t d, t;
    fat_time_encode(0, &d, &t);
    ck(((d >> 9) & 0x7F) == 0 && ((d >> 5) & 0xF) == 1 && (d & 0x1F) == 1,
       "an unset clock encodes as 1980-01-01 rather than wrapping");

    // 2026-08-04 00:00:00 UTC.
    fat_time_encode(1785801600u, &d, &t);
    ck(((d >> 9) & 0x7F) == 46, "a real date lands in the right year");
    ck(((d >> 5) & 0xF) == 8, "the right month");
    ck((d & 0x1F) == 4, "and the right day");

    // Seconds are stored in two-second units, so the encoded value is half.
    fat_time_encode(1785801600u + 3600 * 13 + 60 * 45 + 30, &d, &t);
    ck(((t >> 11) & 0x1F) == 13, "hours survive the round trip");
    ck(((t >> 5) & 0x3F) == 45, "and minutes");
    ck((t & 0x1F) == 15, "with seconds in the two-second units FAT stores");

    // --- a volume large enough not to need padding --------------------------
    FatGeom big;
    ck(fat_geom_init(&big, 16u * 1024 * 1024), "a 16 MB volume can be described");
    ck(big.real_clusters == big.data_clusters,
       "a volume above the floor is described honestly, with no padding");
    ck(big.data_clusters < 65525, "and is still FAT16 rather than FAT32");
    ck(fat_entry(&big, nodes, 2, FAT_FIRST_DATA_CLUSTER + big.data_clusters - 1) != FAT16_BAD,
       "so none of its clusters are marked bad");

    // --- a volume too small to describe -------------------------------------
    FatGeom tiny;
    ck(!fat_geom_init(&tiny, 4096), "a volume with no room to describe is refused");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
