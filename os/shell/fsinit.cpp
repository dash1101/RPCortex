// The filesystem layout, and putting it back when it goes missing.
//
//   /os          the OS's own data — registry, accounts, logs
//   /os/pkg      installed packages and their index
//   /etc         startup, task and service lists
//   /home/<user> one per account
//   /tmp         scratch, cleared at boot
//
// Every one of these is recreated if it is gone. That is not defensive
// programming for its own sake: `rm -r` exists, flash wears out, and a half
// finished update leaves gaps. An OS that will not boot because a directory is
// missing is an OS that turns a small mistake into a reflash — and the person
// who deleted /os is exactly the person least able to recover from it.
//
// Recreating is not the same as restoring. A missing registry comes back EMPTY,
// with defaults, and says so — pretending the settings are still there would be
// worse than admitting they are gone.

#include "command.h"
#include "out.h"
#include "storage.h"
#include "registry.h"
#include "users.h"
#include "persist.h"
#include "task.h"
#include "logring.h"

#include <stdio.h>
#include <string.h>

static const char *kDirs[] = { "/os", "/os/pkg", "/etc", "/home", "/tmp", "/usb" };
#define N_DIRS (sizeof(kDirs) / sizeof(kDirs[0]))

// Make a directory if it is not already there. Returns true if it had to be
// created, so the caller can report what was missing rather than listing
// everything it checked.
static bool ensure_dir(const char *path) {
    bool is_dir = false;
    if (storage_stat(path, &is_dir, nullptr) && is_dir) return false;
    storage_mkdir(path);
    return true;
}

// Every account gets a home. Created here rather than at mkacct time so an
// account that predates this layout, or whose directory was deleted, still gets
// one back.
static bool ensure_homes(bool report) {
    bool made = false;
    for (uint32_t i = 0; i < users_count(); i++) {
        const char *name = users_name_at(i);
        char path[USR_HOME_MAX + 8];
        snprintf(path, sizeof(path), "/home/%s", name);
        if (ensure_dir(path)) {
            made = true;
            if (report) out_warnp("fs", "Recreated %s", path);
        }
    }
    return made;
}

// Called at every boot, before anything reads a file.
void fs_layout_check(bool verbose) {
    uint32_t repaired = 0;
    for (unsigned i = 0; i < N_DIRS; i++)
        if (ensure_dir(kDirs[i])) {
            repaired++;
            if (verbose) out_warnp("fs", "Recreated %s", kDirs[i]);
        }

    if (ensure_homes(verbose)) repaired++;

    // /tmp is scratch by definition, so anything left in it is from a run that
    // did not finish. Clearing it at boot is the difference between scratch
    // space and a directory that silently fills up forever.
    storage_walk("/tmp", [](void *, const char *name, bool is_dir, uint32_t) {
        if (is_dir) return;
        char p[96];
        snprintf(p, sizeof(p), "/tmp/%s", name);
        storage_remove(p);
    }, nullptr);

    if (repaired && !verbose)
        out_warnp("fs", "Repaired %u missing director%s.",
                  (unsigned)repaired, repaired == 1 ? "y" : "ies");
}

// --- reading everything, to find what cannot be read -------------------------
//
// littlefs checksums every block it writes, so a block that has gone bad does
// not come back as wrong data — it comes back as a read ERROR. That makes
// corruption detectable the only way it ever is on flash: by reading, all of
// it, and seeing what refuses.
//
// Which is also why this is bounded to the OS's own directories at boot. They
// are small, they are the files whose corruption stops the device working, and
// they are the only ones the OS can honestly repair: a damaged registry has a
// backup beside it, a damaged package index can be rebuilt, and a damaged file
// in somebody's home directory is theirs and nobody else's business.
//
// `fscheck --scan` reads the rest, and only reports.

static bool file_reads_clean(const char *path, uint32_t size) {
    AppSource src{}; void *h = nullptr;
    if (!storage_open_source(path, &src, &h)) return false;

    uint8_t buf[256];
    uint32_t at = 0;
    bool ok = true;
    while (at < size && ok) {
        uint32_t n = size - at > sizeof(buf) ? (uint32_t)sizeof(buf) : size - at;
        ok = src.read(src.ctx, at, buf, n) >= 0;
        at += n;
        task_alive();
        // The hardware watchdog directly as well: this runs at boot, before
        // there is a scheduler for task_alive to reach, and reading a megabyte
        // takes longer than the watchdog will wait.
        task_watchdog_feed();
    }
    storage_close_source(h);
    return ok;
}

// Names are collected first and read afterwards, never from inside the walk.
// storage_walk holds the filesystem lock for the whole iteration, and a file
// opened underneath it would be reading a directory it is also holding open.
#define SCAN_MAX 40
struct ScanList {
    char     name[SCAN_MAX][40];
    uint32_t size[SCAN_MAX];
    uint32_t n;
    bool     overflow;
};

static void collect(void *ctx, const char *name, bool is_dir, uint32_t size) {
    ScanList *l = (ScanList *)ctx;
    if (is_dir) return;
    if (l->n >= SCAN_MAX) { l->overflow = true; return; }
    snprintf(l->name[l->n], sizeof(l->name[0]), "%s", name);
    l->size[l->n] = size;
    l->n++;
}

// Returns the number of unreadable files found. With `repair`, each one is
// deleted — which is the fix, not a giveup: every file this runs over is one
// the OS knows how to produce again, and a corrupt one that stays on disk is
// read again at every boot forever.
uint32_t fs_scan_dir(const char *dir, bool repair, bool verbose, uint32_t max_bytes) {
    static ScanList list;
    list.n = 0; list.overflow = false;
    if (!storage_walk(dir, collect, &list)) return 0;

    // Said whether or not anyone asked for detail. A scan that quietly checked
    // half a directory and then reported it clean is worse than not scanning:
    // it is the same output as a healthy device, and it is not true.
    if (list.overflow)
        out_warnp("fs", "%s has more than %u files; only the first %u were checked.",
                  dir, SCAN_MAX, SCAN_MAX);

    uint32_t bad = 0;
    for (uint32_t i = 0; i < list.n; i++) {
        // Big files are skipped at boot, and only at boot. The one that matters
        // is the saved firmware image: nearly a megabyte, read at every single
        // start for no benefit, because it is checked against its own hash
        // before it is ever written anywhere. `fscheck --scan` passes no limit
        // and reads everything.
        if (max_bytes && list.size[i] > max_bytes) continue;

        char path[128];
        snprintf(path, sizeof(path), "%s/%s", !strcmp(dir, "/") ? "" : dir, list.name[i]);
        task_alive();
        if (file_reads_clean(path, list.size[i])) continue;

        bad++;
        out_errp("fs", "%s cannot be read — the flash under it has gone bad.", path);
        if (!repair) continue;

        if (storage_remove(path))
            out_warnp("fs", "Removed it. %s", "The OS rebuilds this one at boot.");
        else
            out_errp("fs", "It could not be removed either.");
    }
    return bad;
}

// The boot pass: the OS's own data, and nothing else.
uint32_t fs_integrity_check(bool verbose) {
    static const char *kOwn[] = { "/os", "/os/pkg", "/etc" };
    uint32_t bad = 0;
    for (const char *d : kOwn)
        bad += fs_scan_dir(d, /*repair*/true, verbose, /*max_bytes*/128 * 1024);

    if (bad) {
        out_errp("fs", "%u file%s was damaged and has been removed.",
                 (unsigned)bad, bad == 1 ? "" : "s");
        out_multi("  Settings and accounts come back from their backups. Anything");
        out_multi("  else is rebuilt at the next boot. If this keeps happening the");
        out_multi("  flash is wearing out, and 'fscheck --scan' checks the rest.");
        log_addf(LOG_K_ERR, "fs: removed %u unreadable file(s)", (unsigned)bad);

        // Put back whatever was just deleted that can be put back. This matters
        // most for the case that is easiest to miss: a damaged BACKUP. The
        // primary loaded fine, so nothing else notices, and the scan removes the
        // shadow — leaving a device one bad file from losing everything, with no
        // sign that its safety net is gone. Writing both copies restores it.
        persist_save_registry();
        persist_save_users();
    }
    return bad;
}

// The deeper repair: accounts. If user.cfg is gone there is no way to log in at
// all, which is unrecoverable without a reflash — so a missing or empty account
// file is rebuilt with root and guest, and the person is told plainly that the
// password is back to being set on first run.
bool fs_accounts_check(void) {
    if (users_count() > 0) return false;
    out_err("No accounts found — user data is missing or damaged.");
    out_multi("  Rebuilding. You will be asked to set a root password, as on a");
    out_multi("  first run. Any other accounts that existed are gone.");
    return true;
}

static int cmd_fscheck(int argc, char **argv) {
    bool fix  = (argc >= 2 && (!strcmp(argv[1], "--fix")  || !strcmp(argv[1], "fix")));
    bool scan = (argc >= 2 && (!strcmp(argv[1], "--scan") || !strcmp(argv[1], "scan")));

    // The deep check, on request only. It reads every file on the device, which
    // is the only way to find a block that has gone bad — and on a full
    // filesystem that is megabytes of flash and takes a while, which is why it
    // is not what plain `fscheck` does.
    if (scan) {
        out_info("=== Reading every file ===");
        out_multi("  A file that cannot be read has damaged flash under it.");
        out_blank();

        uint32_t bad = 0;
        for (unsigned i = 0; i < N_DIRS; i++) {
            out_infop("scan", "%s", kDirs[i]);
            bad += fs_scan_dir(kDirs[i], /*repair*/false, /*verbose*/true, /*no limit*/0);
        }
        for (uint32_t i = 0; i < users_count(); i++) {
            char path[USR_HOME_MAX + 8];
            snprintf(path, sizeof(path), "/home/%s", users_name_at(i));
            out_infop("scan", "%s", path);
            bad += fs_scan_dir(path, /*repair*/false, /*verbose*/true, /*no limit*/0);
        }
        bad += fs_scan_dir("/", /*repair*/false, /*verbose*/true, /*no limit*/0);

        out_blank();
        if (!bad) { out_ok("Every file read back cleanly."); return 0; }
        out_err("%u file%s could not be read.", (unsigned)bad, bad == 1 ? "" : "s");
        out_multi("  Delete them and put them back from a copy. If the same files");
        out_multi("  keep failing, that part of the flash is worn out.");
        return 1;
    }

    out_info("=== Filesystem check ===");
    uint32_t missing = 0;
    for (unsigned i = 0; i < N_DIRS; i++) {
        bool is_dir = false;
        bool ok = storage_stat(kDirs[i], &is_dir, nullptr) && is_dir;
        out_multi("  %-12s %s%s%s", kDirs[i],
                  ok ? C_CYAN : C_FAIL, ok ? "ok" : "MISSING", C_RESET);
        if (!ok) missing++;
    }
    for (uint32_t i = 0; i < users_count(); i++) {
        char path[USR_HOME_MAX + 8];
        snprintf(path, sizeof(path), "/home/%s", users_name_at(i));
        bool is_dir = false;
        bool ok = storage_stat(path, &is_dir, nullptr) && is_dir;
        out_multi("  %-12s %s%s%s", path,
                  ok ? C_CYAN : C_FAIL, ok ? "ok" : "MISSING", C_RESET);
        if (!ok) missing++;
    }

    out_blank();
    out_multi("  Accounts   : %u", (unsigned)users_count());
    out_multi("  Settings   : %u", (unsigned)reg_count());

    // THE SHADOW COPIES, because their whole value is being there BEFORE they
    // are needed. A backup nobody has looked at is a backup nobody knows is
    // missing, and the moment it matters is the moment it is too late to check.
    {
        static const char *kPairs[][2] = {
            { "/os/registry.cfg", "/os/registry.bak" },
            { "/os/users.cfg",    "/os/users.bak"    },
        };
        for (const auto &pair : kPairs) {
            uint32_t live = 0, back = 0;
            bool d = false;
            storage_stat(pair[0], &d, &live);
            storage_stat(pair[1], &d, &back);
            const char *name = strrchr(pair[0], '/');
            out_multi("  %-11s %s%s%s  (backup %s%s%s)",
                      name ? name + 1 : pair[0],
                      live ? C_CYAN : C_FAIL, live ? "ok" : "MISSING", C_RESET,
                      back ? C_CYAN : C_WARN,
                      back ? "present" : "none yet", C_RESET);
            if (!back) missing++;
        }
        const PersistRepair *r = persist_repair_report();
        if (r->registry_restored || r->users_restored)
            out_warn("  This boot restored from a backup — the primary had gone bad.");
    }
    {
        bool d = false; uint32_t img = 0;
        bool have = storage_stat("/os/rollback.img", &d, &img) && img;
        out_multi("  Rollback   : %s%s%s", have ? C_CYAN : C_GRAY,
                  have ? "a saved firmware copy is kept" : "none kept",
                  C_RESET);
    }
    out_multi("  Firmware   : %u KB of %u KB reserved",
              (unsigned)(storage_firmware_bytes() / 1024),
              (unsigned)(storage_reserve_bytes() / 1024));
    out_multi("  Filesystem : %u KB free of %u KB",
              (unsigned)(storage_free_bytes() / 1024),
              (unsigned)(storage_total_bytes() / 1024));

    if (!missing) { out_blank(); out_ok("Everything is where it should be."); return 0; }

    out_blank();
    if (!fix) {
        out_warn("%u item%s missing. Run 'fscheck --fix' to recreate %s.",
                 (unsigned)missing, missing == 1 ? " is" : "s are",
                 missing == 1 ? "it" : "them");
        return 1;
    }
    fs_layout_check(/*verbose*/true);

    // Directories are only half of it. A missing backup is repaired by writing
    // one, and writing one means saving what is in memory — which is exactly
    // what these do, primary and shadow together.
    persist_save_registry();
    persist_save_users();

    out_ok("Repaired.");
    out_multi("  'fscheck --scan' reads every file, which finds damaged flash.");
    return 0;
}

void fsinit_register(void) {
    static const Command c{"fscheck", "check and repair the OS layout",
                           cmd_fscheck, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
