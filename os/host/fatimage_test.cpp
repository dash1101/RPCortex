// The whole volume, checked by something that is not this code.
//
// fatview_test checks the pieces against what the specification says. This
// checks the assembled result against an independent implementation: it
// synthesises every sector of a volume exactly as the device would serve it,
// writes the image out, and hands it to fsck.fat from dosfstools.
//
// That distinction is the point. A test written from the same reading of the
// format as the code shares the code's misunderstandings, and the failure mode
// here is silent — a host mounts the drive and shows nothing, or shows nonsense,
// with no error anywhere to trace. An outside implementation does not share the
// misunderstanding, so a directory that does not terminate, a chain that loops,
// a cluster claimed twice or a size that disagrees with its chain all get named.
//
// If fsck.fat is not installed the volume is still built and its structure
// still checked; the run says plainly that the outside opinion was missing.
#include "../core/fatview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *w) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", w); fails++; }
}

// A tree shaped like a real device's: two directories off the root with a
// handful of files each, a file in the root, and an empty one — because a
// zero-length file is the case that has no cluster at all and is where a layout
// that assigns one anyway produces a lost cluster.
static FatNode nodes[64];
static uint32_t node_count;

static void add_long(const char *n83, const char *lname, bool is_dir,
                     uint32_t size, uint16_t parent);

static void add(const char *n83, bool is_dir, uint32_t size, uint16_t parent) {
    FatNode *f = &nodes[node_count++];
    memset(f, 0, sizeof(*f));
    memcpy(f->name, n83, 11);
    f->is_dir = is_dir ? 1 : 0;
    f->size = size;
    f->parent = parent;
    f->mtime = 1785801600u;      // 2026-08-04, so the date fields are not all zero
}

static void add_long(const char *n83, const char *lname, bool is_dir,
                     uint32_t size, uint16_t parent) {
    add(n83, is_dir, size, parent);
    nodes[node_count - 1].lname = lname;
}

int main(void) {
    printf("fatimage_test - a whole volume, checked by fsck.fat\n");

    FatGeom g;
    if (!fat_geom_init(&g, 2u * 1024 * 1024)) {
        printf("  FAIL: could not describe a 2 MB volume\n");
        return 1;
    }

    // Node 0 is the root and owns no cluster.
    add("           ", true, 0, FAT_NO_PARENT);
    add("OS         ", true, 0, 0);
    add("PKG        ", true, 0, 0);
    add("README  TXT", false, 1200, 0);
    add("EMPTY   TXT", false, 0, 0);
    add("CA      PEM", false, 4000, 1);
    add("USERS   CFG", false, 300, 1);
    add("STRESS  APP", false, 9000, 2);
    add("BENCH   APP", false, 7000, 2);
    add("CALC    APP", false, 5000, 2);

    // A name 8.3 cannot hold. Three characters of extension turns repo.json
    // into REPO.JSO, and a file copied off under that name is not the file, so
    // this is a correctness case rather than a cosmetic one.
    uint16_t longfile = (uint16_t)node_count;
    add_long("REPO    JSO", "repo.json", false, 3152, 0);
    // And one long enough to need more than a single long-name entry, since
    // thirteen characters is where the second one begins and the ordering of
    // the run is the part that is easy to get backwards.
    uint16_t longer = (uint16_t)node_count;
    add_long("MEASURE~TXT", "measurements-2026.txt", false, 900, 0);

    // A directory with more children than fit in one cluster.
    //
    // Sixteen entries fill a 512-byte sector, and "." and ".." take two of
    // them, so anything past fourteen children spans a second cluster. Without
    // a directory that does, the size calculation is never load-bearing: every
    // directory needs exactly one cluster whether it is computed from the child
    // count or assumed, and a wrong answer there is invisible.
    //
    // Fifteen rather than a rounder number, deliberately. Fifteen children plus
    // the two dot entries is seventeen, which needs two clusters, while fifteen
    // alone needs one — so a layout that forgets the dot entries is a different
    // answer rather than the same one. At twenty, both arithmetics give two
    // clusters and the mistake hides.
    uint16_t many = (uint16_t)node_count;
    add("MANY       ", true, 0, 0);
    for (int i = 0; i < 15; i++) {
        char n[12];
        snprintf(n, sizeof(n), "FILE%02d  DAT", i);
        add(n, false, 100 + i, many);
    }

    ck(fat_entries_for(&nodes[longfile]) == 2,
       "a nine-character name needs one long-name entry ahead of the 8.3 one");
    ck(fat_entries_for(&nodes[longer]) == 3,
       "and a twenty-one character name needs two");
    ck(fat_entries_for(&nodes[3]) == 1,
       "while a name 8.3 holds exactly needs none");
    ck(fat_name_fits_83("readme.txt") && !fat_name_fits_83("repo.json"),
       "which is decided by whether the 8.3 form IS the name");

    uint32_t used = fat_layout(nodes, node_count, g.real_clusters);
    ck(nodes[many].clusters == 2,
       "a directory with fifteen children spans two clusters, not one");
    ck(used > 0, "the tree lays out into clusters");
    ck(nodes[0].clusters == 0, "the root occupies no cluster of its own");
    ck(nodes[4].first_cluster == 0 && nodes[4].clusters == 0,
       "an empty file owns no cluster, so nothing is lost in the table");
    ck(nodes[3].clusters == 3, "a 1200-byte file occupies three 512-byte clusters");

    // No two nodes may claim the same cluster. fsck would say so too, but this
    // names the pair, which is what makes it debuggable.
    {
        bool overlap = false;
        for (uint32_t i = 1; i < node_count && !overlap; i++) {
            if (!nodes[i].clusters) continue;
            for (uint32_t j = i + 1; j < node_count; j++) {
                if (!nodes[j].clusters) continue;
                uint32_t a0 = nodes[i].first_cluster, a1 = a0 + nodes[i].clusters;
                uint32_t b0 = nodes[j].first_cluster, b1 = b0 + nodes[j].clusters;
                if (a0 < b1 && b0 < a1) {
                    printf("  (nodes %u and %u overlap)\n", i, j);
                    overlap = true; break;
                }
            }
        }
        ck(!overlap, "no two nodes claim the same cluster");
    }

    // --- write the image ----------------------------------------------------
    char tmpl[] = "/tmp/rpcortex-fatview-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { printf("  FAIL: no temporary file\n"); return 1; }
    FILE *f = fdopen(fd, "wb");

    uint8_t sec[FAT_SECTOR_SIZE];
    for (uint32_t lba = 0; lba < g.total_sectors; lba++) {
        uint32_t idx, sic;
        switch (fat_region(&g, lba, &idx, &sic)) {
        case FAT_RGN_BOOT:
            fat_build_boot(&g, sec, "RPCORTEX", 0x52504332);
            break;
        case FAT_RGN_FAT:
            fat_fat_sector(&g, idx, nodes, node_count, sec);
            break;
        case FAT_RGN_ROOT:
            fat_dir_sector(nodes, node_count, 0,
                           idx * (FAT_SECTOR_SIZE / FAT_DIRENT_SIZE), "RPCORTEX", sec);
            break;
        case FAT_RGN_DATA: {
            int32_t owner = fat_node_for_cluster(nodes, node_count, idx);
            if (owner < 0) { memset(sec, 0, sizeof(sec)); break; }
            uint32_t within = (idx - nodes[owner].first_cluster) * g.sectors_per_cluster + sic;
            if (nodes[owner].is_dir) {
                fat_dir_sector(nodes, node_count, (uint32_t)owner,
                               within * (FAT_SECTOR_SIZE / FAT_DIRENT_SIZE), "RPCORTEX", sec);
            } else {
                // Content nothing checks, but not zeroes — a bug that serves the
                // wrong sector is easier to see against a recognisable pattern.
                memset(sec, 'A' + (owner % 26), sizeof(sec));
            }
            break;
        }
        default:
            memset(sec, 0, sizeof(sec));
            break;
        }
        if (fwrite(sec, 1, sizeof(sec), f) != sizeof(sec)) {
            printf("  FAIL: short write building the image\n");
            fclose(f); remove(tmpl); return 1;
        }
    }
    fclose(f);

    // --- the outside opinion ------------------------------------------------
    //
    // -n is a read-only check: report, change nothing. dosfstools lives in
    // /sbin, which is not always on a user's PATH, so both are tried.
    const char *fsck = nullptr;
    if (system("test -x /sbin/fsck.fat") == 0)          fsck = "/sbin/fsck.fat";
    else if (system("test -x /usr/sbin/fsck.fat") == 0) fsck = "/usr/sbin/fsck.fat";
    else if (system("command -v fsck.fat >/dev/null 2>&1") == 0) fsck = "fsck.fat";

    if (!fsck) {
        printf("  NOTE: fsck.fat not installed -- the volume was built and its\n"
               "        structure checked, but nothing outside this code read it.\n"
               "        Install dosfstools to get the check that matters.\n");
    } else {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s -n -v %s > %s.log 2>&1", fsck, tmpl, tmpl);
        int rc = system(cmd);
        bool clean = (rc == 0);
        ck(clean, "fsck.fat reads the volume and finds nothing wrong");

        // The checker has to have understood it as FAT16. Reporting 12-bit
        // entries would mean the cluster count landed on the wrong side of the
        // boundary, and every entry after that is read at the wrong bit offset
        // — which fsck would not necessarily complain about, because a FAT12
        // reading of this table is not self-evidently broken. So the width is
        // asserted directly, in fsck's own words rather than ours.
        char logcmd[512];
        snprintf(logcmd, sizeof(logcmd), "grep -q '16 bit entries' %s.log", tmpl);
        ck(system(logcmd) == 0, "and reads it as 16-bit entries, not FAT12");

        // Every file laid out must be one fsck can see. A directory that does
        // not terminate, or a chain that ends early, shows up as a lower count
        // rather than as an error.
        snprintf(logcmd, sizeof(logcmd), "grep -q '%u files' %s.log", node_count, tmpl);
        ck(system(logcmd) == 0, "and finds every node the layout described");

        if (!clean) {
            printf("  --- fsck.fat said ---\n");
            snprintf(cmd, sizeof(cmd), "sed 's/^/    /' %s.log", tmpl);
            if (system(cmd) != 0) printf("    (could not read the log)\n");
        }
        snprintf(cmd, sizeof(cmd), "rm -f %s.log", tmpl);
        if (system(cmd) != 0) { /* the temporary file is not worth failing over */ }
    }

    remove(tmpl);
    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
