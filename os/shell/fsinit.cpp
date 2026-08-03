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

#include <stdio.h>
#include <string.h>

static const char *kDirs[] = { "/os", "/os/pkg", "/etc", "/home", "/tmp" };
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
    bool fix = (argc >= 2 && (!strcmp(argv[1], "--fix") || !strcmp(argv[1], "fix")));

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
    out_ok("Repaired.");
    return 0;
}

void fsinit_register(void) {
    static const Command c{"fscheck", "check and repair the OS layout",
                           cmd_fscheck, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
