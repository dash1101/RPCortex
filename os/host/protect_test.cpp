// Which paths an ordinary account is kept away from.
//
// Every row here is a way somebody would try to get at the registry, the user
// list or the device key, or a path that must stay open so the rule does not
// swallow the filesystem. A rule that is too wide breaks `ls /os` for a guest; a
// rule that is too narrow is a user list anybody can edit.

#include "protect.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}
static void secret(const char *p, bool want) {
    char m[200]; snprintf(m, sizeof(m), "secret(\"%s\") is %s", p ? p : "null", want ? "true" : "false");
    ck(protect_secret(p) == want, m);
}
static void reaches(const char *p, bool want) {
    char m[200]; snprintf(m, sizeof(m), "reaches(\"%s\") is %s", p ? p : "null", want ? "true" : "false");
    ck(protect_reaches(p) == want, m);
}

int main(void) {
    // --- the files themselves --------------------------------------------------
    for (const char *p : {"/os/registry.cfg", "/os/registry.bak", "/os/users.cfg", "/os/users.bak", "/os/device.key"}) {
        secret(p, true);
        reaches(p, true);
    }

    // --- every way of spelling the same file -----------------------------------
    for (const char *p : {"/OS/Registry.CFG", "/os//registry.cfg", "/os/./registry.cfg",
                          "/os/x/../registry.cfg", "/os/registry.cfg/", "os/registry.cfg",
                          "/../os/registry.cfg", "/os/../os/users.cfg", "//os///device.key",
                          "/Os/USERS.bak"}) {
        secret(p, true);
    }

    // --- near misses stay open -------------------------------------------------
    for (const char *p : {"/os/registry.cfg.old", "/os/registry.cfg2", "/os/registry", "/etc/registry.cfg",
                          "/os/version", "/os/crash.log", "/os/crash.old", "/os/rollback.img",
                          "/home/x/users.cfg", "/usb/users.cfg", "/os", "/", "", "/oslo/users.cfg",
                          "/os/sub/users.cfg", "/etc/users.cfg"}) {
        secret(p, false);
    }
    secret(nullptr, false);
    reaches(nullptr, false);

    // --- reaching: containers and wildcards ------------------------------------
    for (const char *p : {"/", "/os", "/os/", "/os/.", "/os/../os", "/OS", "/os/x/..", "//os//"}) reaches(p, true);
    for (const char *p : {"/os/*", "/os/*.cfg", "/os/reg*", "/os/?", "/os/device.k?y", "/OS/*", "/os/./*.bak"}) reaches(p, true);
    // A wildcard in the root can match /os itself, which `rm -r` would take whole.
    for (const char *p : {"/*", "/o*", "/?s", "/*s", "*", "/e*", "/../*", "//*"}) reaches(p, true);

    // --- and things that must stay open ----------------------------------------
    for (const char *p : {"/etc", "/etc/*", "/etc/startup.cfg", "/usb", "/usb/*", "/os/crash.log", "/os/version",
                          "/os/sub", "/os/sub/*", "/home", "/home/*", "/oslo", "/os2/*", "/oslo/*", "/tmp/x",
                          "/etc/s*", "/usb/*.txt", "/os/sub/*.cfg", "/home/x/*"}) {
        reaches(p, false);
    }

    // --- a name too long to judge is not assumed harmless -----------------------
    {
        char big[400];
        memset(big, 'a', sizeof(big) - 1); big[0] = '/'; big[sizeof(big) - 1] = 0;
        ck(protect_secret(big) && protect_reaches(big), "a path too long to normalise is treated as protected");
    }

    printf("  protect: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
