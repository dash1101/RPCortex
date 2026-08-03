#include "perms.h"

bool perms_allows(const char *, bool is_admin, CmdLevel level, bool elevated) {
    if (level == LEVEL_USER) return true;
    // sudo raises the level only for someone who already had it. A non-admin
    // typing sudo gets a refusal, not a prompt — there is no path from a normal
    // account to root on a device with no way to audit one.
    return is_admin && (is_admin || elevated);
}
