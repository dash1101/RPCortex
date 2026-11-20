#include "perms.h"

#include <string.h>

static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

bool perms_reg_write_ok(const char *key, bool restricted, bool remote) {
    if (!key) return false;
    if (!restricted) return true;
    // Not User.*: those land in the CONSOLE user's own settings (the
    // per-user scope follows the console's login), which a remote account
    // has no business writing.
    if (remote) return starts(key, "Apps.");
    // Exact-case prefixes, as the registry compares keys exactly.
    return !starts(key, "System.Autonomous") && !starts(key, "System.Setup") &&
           !starts(key, "System.Active_User") && !starts(key, "SSH.") && !starts(key, "Alias.");
}

bool perms_allows(const char *, bool is_admin, CmdLevel level, bool elevated) {
    if (level == LEVEL_USER) return true;
    // sudo raises the level only for someone who already had it. A non-admin
    // typing sudo gets a refusal, not a prompt — there is no path from a normal
    // account to root on a device with no way to audit one.
    return is_admin && (is_admin || elevated);
}
