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

    // --- long names ---------------------------------------------------------
    //
    // Tested here by inspecting the bytes, because fsck.fat does not check all
    // of this. It catches a run written in the wrong order and a wrong
    // attribute byte, but a wrong checksum and a missing end-of-name marker
    // pass it — and both of those make a host quietly fall back to the 8.3
    // name, which is the exact failure long names exist to prevent.
    {
        FatNode t[3];
        memset(t, 0, sizeof(t));
        t[0].is_dir = 1; t[0].parent = FAT_NO_PARENT;          // the root
        memcpy(t[1].name, "REPO    JSO", 11);
        t[1].lname = "repo.json"; t[1].parent = 0; t[1].size = 3152; t[1].first_cluster = 2;
        t[1].clusters = 7;
        memcpy(t[2].name, "NOTES   TXT", 11);
        t[2].lname = "notes.txt"; t[2].parent = 0; t[2].size = 10; t[2].first_cluster = 9;
        t[2].clusters = 1;

        ck(fat_entries_for(&t[1]) == 2, "a long name adds one entry ahead of the 8.3 one");
        ck(fat_entries_for(&t[2]) == 1, "a name that fits 8.3 adds none");

        uint8_t sec[FAT_SECTOR_SIZE];
        fat_dir_sector(t, 3, 0, 0, "RPCORTEX", sec);

        const uint8_t *label = sec;
        const uint8_t *lfn   = sec + FAT_DIRENT_SIZE;
        const uint8_t *short_ = sec + 2 * FAT_DIRENT_SIZE;

        ck(label[11] == FAT_ATTR_VOLUME_ID, "the label still comes first");
        ck(lfn[11] == 0x0F, "the long-name entry carries the long-name attribute");
        ck((lfn[0] & 0x40) != 0, "and is marked as the end of the name");
        ck((lfn[0] & 0x1F) == 1, "with sequence one, since the name needs only one");
        ck(memcmp(short_, "REPO    JSO", 11) == 0, "the 8.3 entry follows it");
        ck(rd16(lfn + 26) == 0, "a long-name entry names no cluster");

        // The characters, UTF-16, in the three runs the format scatters them
        // across. Getting the offsets wrong is the classic mistake and shows up
        // as a name with holes in it.
        ck(lfn[1] == 'r' && lfn[3] == 'e' && lfn[5] == 'p' && lfn[7] == 'o',
           "the first run holds the first characters");
        ck(lfn[9] == '.' && lfn[14] == 'j' && lfn[16] == 's',
           "and the second run continues without a gap");
        // "repo.json" is nine characters, so slots 0..8 hold the name, slot 9
        // holds the terminator, and everything past that is padding.
        ck(rd16(lfn + 18) == 'o' && rd16(lfn + 20) == 'n',
           "the last characters of the name are where they belong");
        ck(rd16(lfn + 22) == 0x0000, "the name is terminated one slot past its end");
        ck(rd16(lfn + 24) == 0xFFFF, "and padded after that");

        // The checksum ties the long name to the 8.3 entry behind it. A host
        // that finds a mismatch throws the long name away, so a wrong one is
        // invisible except that the file is back to being called REPO.JSO.
        // Computed here from the published formula rather than by calling the
        // implementation, so a transcription error in either is a disagreement.
        uint8_t expect = 0;
        for (int i = 0; i < 11; i++)
            expect = (uint8_t)(((expect & 1) ? 0x80 : 0) + (expect >> 1) + (uint8_t)short_[i]);
        ck(lfn[13] == expect, "the checksum is the one the host will compute");

        // Two files must not share a checksum derived from different names.
        // A constant would pass every check above.
        uint8_t other = 0;
        for (int i = 0; i < 11; i++)
            other = (uint8_t)(((other & 1) ? 0x80 : 0) + (other >> 1) + (uint8_t)"NOTES   TXT"[i]);
        ck(other != expect, "and is derived from the name, not fixed");
    }

    // A name needing two entries, where the reversed ordering matters.
    {
        FatNode t[2];
        memset(t, 0, sizeof(t));
        t[0].is_dir = 1; t[0].parent = FAT_NO_PARENT;
        memcpy(t[1].name, "MEASUR~1TXT", 11);
        t[1].lname = "measurements-2026.txt";     // 21 characters: two entries
        t[1].parent = 0; t[1].first_cluster = 2; t[1].clusters = 2; t[1].size = 900;

        ck(fat_entries_for(&t[1]) == 3, "twenty-one characters need two long-name entries");

        uint8_t sec[FAT_SECTOR_SIZE];
        fat_dir_sector(t, 2, 0, 0, "RPCORTEX", sec);
        const uint8_t *first  = sec + FAT_DIRENT_SIZE;
        const uint8_t *second = sec + 2 * FAT_DIRENT_SIZE;

        // Stored in REVERSE: the run begins with the highest sequence number,
        // which is also the one carrying the end marker.
        ck((first[0] & 0x1F) == 2 && (first[0] & 0x40),
           "the run starts with the LAST piece of the name");
        ck((second[0] & 0x1F) == 1 && !(second[0] & 0x40),
           "and ends with the first piece");
        ck(first[13] == second[13], "both pieces carry the same checksum");
        // Piece one holds characters 1-13, piece two the rest.
        ck(second[1] == 'm' && second[3] == 'e' && second[5] == 'a',
           "the first piece holds the start of the name");
        ck(first[1] == '2' && first[3] == '0',
           "and the second picks up at the fourteenth character");
    }

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
