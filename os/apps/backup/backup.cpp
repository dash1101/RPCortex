// Backup — named snapshots of everything a person has decided about this
// device, and a way back to any of them. Ported from v1's Backup package.
//
// The OS already keeps ONE shadow copy of the registry and the account file, so
// a corrupt primary can be repaired at boot. That is a different job from this
// one. A shadow copy tracks the live file — it follows you into the mistake.
// A snapshot is a point somebody chose, kept until they say otherwise, and you
// can have as many as you like.
//
// What is covered is the same list v1 covered, translated to where v2 keeps it.
// v1 had six files because WiFi credentials and aliases had their own; in v2
// both live in the registry, so five files hold the lot.
//
// This streams, the way v1 did. fw_file_copy (API 1.23) hands the whole job to
// the firmware, which copies a few hundred bytes at a time and never lands the
// file in RAM. That is what removed the old 32 KB ceiling: the package used to
// read a file into one buffer and write it back out, so anything larger than a
// single allocation on a fragmented heap was refused by name rather than copied.
// The config files here are small by construction, but a snapshot of something
// large is now a copy rather than a "past the size this can do" message.
#include "rpc_app.h"
#include <stdio.h>       // snprintf - one of the handful of libc calls api.cpp exports

RPC_APP_VER("backup", "2.1");

#define BK_DIR      "/etc/backups"
#define BK_NAME_MAX 32
#define BK_PATH_MAX 96

// Every decision a person has made about this device, and where v2 keeps it.
// The registry is the big one: settings, WiFi networks, aliases and each
// package's own keys are all in there.
static const char *const bk_files[] = {
    "/os/registry.cfg",
    "/os/users.cfg",
    "/etc/startup.cfg",
    "/etc/tasks.cfg",
    "/etc/services.cfg",
};
#define BK_NFILES ((int)(sizeof(bk_files) / sizeof(bk_files[0])))

// --- small helpers ----------------------------------------------------------

static int bk_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

// The part after the last '/'. Used to name the copy inside the snapshot, so a
// restore knows which live file each one came from.
static const char *bk_leaf(const char *path) {
    const char *leaf = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') leaf = p + 1;
    return leaf;
}

// Join, refusing rather than truncating. A path that silently lost its tail
// would name a DIFFERENT file, which for a restore is the worst possible
// failure — it would overwrite something nobody asked about.
static int bk_join(char *out, unsigned cap, const char *dir, const char *leaf) {
    unsigned n = 0;
    for (const char *p = dir; *p; p++) {
        if (n + 2 >= cap) return 0;
        out[n++] = *p;
    }
    if (n + 2 >= cap) return 0;
    out[n++] = '/';
    for (const char *p = leaf; *p; p++) {
        if (n + 1 >= cap) return 0;
        out[n++] = *p;
    }
    out[n] = 0;
    return 1;
}

// A snapshot name becomes part of a path, so it is checked before it is used.
// "../.." would put the copy somewhere nobody asked for and, worse, a restore
// would read it back from there.
static int bk_name_ok(const char *name) {
    if (!name || !*name) return 0;
    unsigned n = 0;
    for (const char *p = name; *p; p++, n++) {
        char c = *p;
        int fine = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!fine) return 0;
    }
    if (n >= BK_NAME_MAX) return 0;
    // A leading dot, "." or ".." would all resolve somewhere other than here.
    if (name[0] == '.') return 0;
    return 1;
}

// The default name: the date and time, so snapshots sort themselves. A device
// that has never seen a time server has a year of zero, which would make every
// snapshot look like the same moment — so that case gets the uptime instead and
// says so by its shape.
static void bk_default_name(char *out, unsigned cap) {
    FwTime t;
    if (fw_time_get(&t) && t.year > 2000) {
        snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
        return;
    }
    snprintf(out, cap, "backup-%lu", (unsigned long)fw_millis());
}

static void bk_size_str(char *out, unsigned cap, unsigned long bytes) {
    if (bytes < 1024) snprintf(out, cap, "%lu B", bytes);
    else              snprintf(out, cap, "%lu.%lu KB", bytes / 1024,
                               (bytes % 1024) * 10 / 1024);
}

// --- copying ----------------------------------------------------------------

// Returns bytes copied, or -1 having already said what went wrong. Streamed
// through fw_file_copy, so there is no ceiling on the size and no buffer to run
// the heap out of. An empty source is a real answer: fw_file_copy makes it an
// empty file, because a config somebody has emptied on purpose must restore
// empty.
static long bk_copy(const char *src, const char *dst) {
    unsigned size = fw_file_size(src);
    if (!fw_file_copy(src, dst)) {
        // A streamed copy finds a full filesystem only when a write fails
        // partway, and it leaves the half it wrote behind. Take it out, so a
        // snapshot never holds a truncated config that a later restore would
        // then write over the good one.
        fw_file_remove(dst);
        fw_printf("Could not copy %s.\n", src);
        return -1;
    }
    return (long)size;
}

// Make a directory that may already be there. fw_mkdir reports failure for
// both reasons, so existence is asked about separately.
static int bk_ensure_dir(const char *path) {
    if (fw_file_exists(path)) return 1;
    if (fw_mkdir(path)) return 1;
    fw_printf("Could not create %s.\n", path);
    return 0;
}

// --- subcommands ------------------------------------------------------------

static void bk_usage(void) {
    fw_printf("Backup - snapshot and restore this device's configuration.\n\n");
    fw_printf("  backup create [name]      snapshot now (default name is the date)\n");
    fw_printf("  backup list               what has been saved\n");
    fw_printf("  backup restore <name> -y  copy a snapshot back over the live config\n");
    fw_printf("  backup remove <name>      delete a snapshot\n");
    fw_printf("  backup info               what is covered, and where it goes\n");
}

static void bk_info(void) {
    fw_printf("Snapshots live in %s.\n\n", BK_DIR);
    fw_printf("Covered:\n");
    for (int i = 0; i < BK_NFILES; i++) {
        unsigned sz = fw_file_size(bk_files[i]);
        if (fw_file_exists(bk_files[i]))
            fw_printf("  %-20s %u bytes\n", bk_files[i], sz);
        else
            fw_printf("  %-20s (not present)\n", bk_files[i]);
    }
    fw_printf("\nThe registry holds the settings, the saved WiFi networks, the\n");
    fw_printf("aliases and each package's own keys, so those all travel with it.\n");
    fw_printf("Home directories do not - this is configuration, not your files.\n");
}

static int bk_create(const char *name) {
    char chosen[BK_NAME_MAX];
    if (!name || !*name) {
        bk_default_name(chosen, sizeof(chosen));
        name = chosen;
    }
    if (!bk_name_ok(name)) {
        fw_printf("'%s' will not do as a name - letters, digits, dash, dot and\n", name);
        fw_printf("underscore only, under %d characters, not starting with a dot.\n",
                  BK_NAME_MAX);
        return 1;
    }
    if (!bk_ensure_dir(BK_DIR)) return 1;

    char dir[BK_PATH_MAX];
    if (!bk_join(dir, sizeof(dir), BK_DIR, name)) {
        fw_printf("That name makes too long a path.\n");
        return 1;
    }
    if (fw_file_exists(dir)) {
        fw_printf("A snapshot called '%s' is already there.\n", name);
        fw_printf("Pick another name, or: backup remove %s\n", name);
        return 1;
    }
    if (!bk_ensure_dir(dir)) return 1;

    int saved = 0;
    unsigned long total = 0;
    for (int i = 0; i < BK_NFILES; i++) {
        if (!fw_file_exists(bk_files[i])) continue;
        char dst[BK_PATH_MAX];
        if (!bk_join(dst, sizeof(dst), dir, bk_leaf(bk_files[i]))) continue;
        long n = bk_copy(bk_files[i], dst);
        if (n < 0) continue;
        saved++;
        total += (unsigned long)n;
    }
    if (saved == 0) {
        fw_printf("Nothing to save - none of the config files are there.\n");
        fw_file_remove(dir);
        return 1;
    }
    char sz[24];
    bk_size_str(sz, sizeof(sz), total);
    fw_printf("Snapshot '%s' saved - %d files, %s.\n", name, saved, sz);
    fw_printf("Back to it later with: backup restore %s -y\n", name);
    return 0;
}

static int bk_list(void) {
    int n = fw_dir_count(BK_DIR);
    if (n <= 0) {
        fw_printf("No snapshots yet. Make one with: backup create\n");
        return 0;
    }
    fw_printf("Snapshots in %s:\n", BK_DIR);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        FwDirEntry e;
        if (fw_dir_entry(BK_DIR, (unsigned)i, &e) != 1) continue;
        if (!e.is_dir) continue;

        char dir[BK_PATH_MAX];
        if (!bk_join(dir, sizeof(dir), BK_DIR, e.name)) continue;
        int files = fw_dir_count(dir);
        unsigned long bytes = 0;
        for (int j = 0; j < files; j++) {
            FwDirEntry f;
            if (fw_dir_entry(dir, (unsigned)j, &f) == 1 && !f.is_dir)
                bytes += f.size;
        }
        char sz[24];
        bk_size_str(sz, sizeof(sz), bytes);
        fw_printf("  %-24s %d files  %s\n", e.name, files < 0 ? 0 : files, sz);
        shown++;
    }
    if (!shown) fw_printf("No snapshots yet. Make one with: backup create\n");
    return 0;
}

static int bk_restore(const char *name, int confirmed) {
    if (!name || !*name) {
        fw_printf("Which one? Try: backup list\n");
        return 1;
    }
    if (!bk_name_ok(name)) {
        fw_printf("'%s' is not a snapshot name.\n", name);
        return 1;
    }
    char dir[BK_PATH_MAX];
    if (!bk_join(dir, sizeof(dir), BK_DIR, name)) {
        fw_printf("That name makes too long a path.\n");
        return 1;
    }
    int n = fw_dir_count(dir);
    if (n <= 0) {
        fw_printf("There is no snapshot called '%s'. See: backup list\n", name);
        return 1;
    }

    // Say what would happen before it happens. There is no way for a package to
    // read a line from the console, so the confirmation is an argument rather
    // than a question - which has the side benefit of being scriptable.
    if (!confirmed) {
        fw_printf("Restoring '%s' would overwrite:\n", name);
        for (int i = 0; i < n; i++) {
            FwDirEntry e;
            if (fw_dir_entry(dir, (unsigned)i, &e) != 1 || e.is_dir) continue;
            for (int k = 0; k < BK_NFILES; k++)
                if (bk_streq(e.name, bk_leaf(bk_files[k])))
                    fw_printf("  %s\n", bk_files[k]);
        }
        fw_printf("\nGo ahead with: backup restore %s -y\n", name);
        return 1;
    }

    int done = 0;
    for (int i = 0; i < n; i++) {
        FwDirEntry e;
        if (fw_dir_entry(dir, (unsigned)i, &e) != 1 || e.is_dir) continue;

        // Only back to where it came from. A file that found its way into a
        // snapshot directory by some other route is not something to copy into
        // /os on the strength of its name.
        const char *dst = 0;
        for (int k = 0; k < BK_NFILES; k++)
            if (bk_streq(e.name, bk_leaf(bk_files[k]))) dst = bk_files[k];
        if (!dst) {
            fw_printf("Skipping '%s' - not one of the files this covers.\n", e.name);
            continue;
        }
        char src[BK_PATH_MAX];
        if (!bk_join(src, sizeof(src), dir, e.name)) continue;
        if (bk_copy(src, dst) >= 0) done++;
    }
    if (!done) {
        fw_printf("Nothing was restored.\n");
        return 1;
    }
    fw_printf("Restored %d files from '%s'.\n", done, name);
    // Not politeness - the running OS holds the registry in memory, and the
    // next thing that saves a setting writes that copy back over the file just
    // put in place. Rebooting is what makes the restore stick.
    fw_printf("Reboot now to pick them up: anything that saves a setting first\n");
    fw_printf("will write the running configuration back over these.\n");
    return 0;
}

static int bk_remove(const char *name) {
    if (!name || !*name) {
        fw_printf("Which one? Try: backup list\n");
        return 1;
    }
    if (!bk_name_ok(name)) {
        fw_printf("'%s' is not a snapshot name.\n", name);
        return 1;
    }
    char dir[BK_PATH_MAX];
    if (!bk_join(dir, sizeof(dir), BK_DIR, name)) {
        fw_printf("That name makes too long a path.\n");
        return 1;
    }
    if (fw_dir_count(dir) < 0) {
        fw_printf("There is no snapshot called '%s'.\n", name);
        return 1;
    }
    // Empty it first: a directory can only be removed once nothing is in it.
    // Index 0 each time, because removing an entry shifts everything after it.
    for (int guard = 0; guard < 64; guard++) {
        FwDirEntry e;
        if (fw_dir_entry(dir, 0, &e) != 1) break;
        char path[BK_PATH_MAX];
        if (!bk_join(path, sizeof(path), dir, e.name)) break;
        if (!fw_file_remove(path)) break;
    }
    if (!fw_file_remove(dir)) {
        fw_printf("Could not remove '%s'.\n", name);
        return 1;
    }
    fw_printf("Removed snapshot '%s'.\n", name);
    return 0;
}

// --- the command ------------------------------------------------------------

static int bk_cmd(int argc, char **argv) {
    if (argc < 2) { bk_usage(); return 0; }

    const char *sub = argv[1];
    if (bk_streq(sub, "help") || bk_streq(sub, "-h") || bk_streq(sub, "--help") ||
        bk_streq(sub, "?")) {
        bk_usage();
        return 0;
    }
    if (bk_streq(sub, "info"))   return bk_info(), 0;
    if (bk_streq(sub, "list"))   return bk_list();
    if (bk_streq(sub, "create")) return bk_create(argc > 2 ? argv[2] : 0);
    if (bk_streq(sub, "remove") || bk_streq(sub, "rm") || bk_streq(sub, "delete"))
        return bk_remove(argc > 2 ? argv[2] : 0);
    if (bk_streq(sub, "restore")) {
        int confirmed = 0;
        for (int i = 3; i < argc; i++)
            if (bk_streq(argv[i], "-y") || bk_streq(argv[i], "--yes") ||
                bk_streq(argv[i], "yes")) confirmed = 1;
        return bk_restore(argc > 2 ? argv[2] : 0, confirmed);
    }

    fw_printf("'%s' is not a backup subcommand.\n", sub);
    bk_usage();
    return 1;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("backup", "snapshot and restore the configuration", bk_cmd);
    return 0;
}
