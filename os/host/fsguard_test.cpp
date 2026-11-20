// The session guard: who is held back from the protected files, and who is not.
//
// The path rules are tested in protect_test. This is the other half, the decision
// about the person: an admin and the system pass, an ordinary account and the
// guest do not, and the message says which file and what was refused.

#include "fsguard.h"
#include "ident.h"
#include "out.h"
#include "logring.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

// The account list, faked. Whoever is "logged in" at the console is set with
// ident_console_set, the call the login prompt makes.
bool users_is_admin(const char *n) { return n && !strcmp(n, "root"); }

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

int main(void) {
    const char *reg = "/os/registry.cfg", *users = "/os/users.cfg", *key = "/os/device.key";
    const char *open_file = "/etc/startup.cfg", *os_dir = "/os", *wild = "/os/*.cfg";

    // --- nobody logged in: boot, first-run setup, the login prompt --------------
    ident_console_set("");
    ck(!fs_session_restricted(), "with nobody logged in the session is not restricted");
    ck(fs_may_read(reg) && fs_may_change(reg) && fs_may_change(os_dir), "so boot code is never held back");

    // --- an admin ----------------------------------------------------------------
    ident_console_set("root");
    ck(!fs_session_restricted(), "an admin is not restricted");
    for (const char *p : {reg, users, key, os_dir, wild, "/"}) {
        ck(fs_may_read(p) && fs_may_change(p), "an admin may read and change every protected path");
    }

    // --- the guest, and an ordinary account --------------------------------------
    for (const char *who : {"guest", "sam"}) {
        ident_console_set(who);
        ck(fs_session_restricted(), "an account that is not an admin is restricted");
        ck(!fs_may_read(reg) && !fs_may_read(users) && !fs_may_read(key), "it cannot read the registry, the user list or the key");
        ck(!fs_may_change(reg) && !fs_may_change(users) && !fs_may_change(key), "it cannot change them");
        ck(!fs_may_change(os_dir) && !fs_may_change("/") && !fs_may_change(wild) && !fs_may_change("/*"),
           "it cannot remove the directory that holds them, the root, or a wildcard that matches them");
        ck(!fs_may_read("/OS/Users.CFG") && !fs_may_read("/os/../os/device.key") && !fs_may_change("os//registry.cfg"),
           "and spelling the path differently does not help");

        // What runs at boot as the console's user is in /etc and /os, so none of
        // it may be CHANGED by an account that is not an admin — that would be
        // root at the next restart. A deliberate change of policy: /etc used to
        // be ordinary files.
        ck(fs_may_read(open_file) && !fs_may_change(open_file), "startup items can be read, not changed");
        ck(!fs_may_change("/os/pkg/index.cfg") && !fs_may_change("/os/ca.pem"),
           "nor installed packages, nor the trusted certificates");

        // What must stay open, or the rule swallows the filesystem.
        ck(fs_may_read(os_dir) && fs_may_read("/os/crash.log") && fs_may_read("/os/version"),
           "the /os directory can still be listed, and its other files read");
        char own[48];
        snprintf(own, sizeof(own), "/home/%s/notes.txt", who);
        ck(fs_may_change(own) && fs_may_change("/tmp/x") && fs_may_change("/usb/*"),
           "a person's own files, and the shared areas, can be changed");
        ck(fs_may_read("/home/root/notes.txt") && !fs_may_change("/home/root/notes.txt"),
           "someone else's can be read, not changed");
    }

    // --- the message ----------------------------------------------------------------
    {
        ident_console_set("guest");
        char cap[512];
        ck(out_capture_begin_all(cap, sizeof(cap)), "capture starts");
        bool ok = fs_allow_read("/os/device.key");
        uint32_t n = out_capture_end();
        cap[n < sizeof(cap) ? n : sizeof(cap) - 1] = 0;
        ck(!ok, "fs_allow_read refuses");
        ck(strstr(cap, "/os/device.key") != nullptr, "the message names the file");
        ck(strstr(cap, "Permission denied") != nullptr && strstr(cap, "admin") != nullptr, "and says it needs an admin");
        ck(strstr(cap, "read") != nullptr, "and what was refused");

        ck(out_capture_begin_all(cap, sizeof(cap)), "capture starts again");
        ok = fs_allow_change("/os/users.cfg");
        n = out_capture_end(); cap[n < sizeof(cap) ? n : sizeof(cap) - 1] = 0;
        ck(!ok && strstr(cap, "change") != nullptr, "a refused change says change");

        ck(out_capture_begin_all(cap, sizeof(cap)), "capture starts a third time");
        ok = fs_allow_read("/etc/startup.cfg") && fs_allow_change("/home/guest/a");
        n = out_capture_end();
        ck(ok && n == 0, "an allowed access prints nothing");

        // The refusal is logged, so `logdump` shows who tried.
        bool logged = false;
        for (uint32_t i = 0; i < log_count(); i++) {
            const LogLine *l = log_at(i);
            if (l && strstr(l->text, "[Perm] guest refused read") && strstr(l->text, "device.key")) logged = true;
        }
        ck(logged, "the refusal is written to the log with the account and the file");
    }

    printf("  fsguard: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
