#include "fsguard.h"
#include "ident.h"
#include "session.h"
#include "users.h"
#include "protect.h"
#include "out.h"
#include "logring.h"

// Restricted means "not an admin", asked of the CALLING task's user (see
// session_user). The console with nobody signed in is unrestricted, as it
// always was: that is boot, and the login prompt. A remote login is never in
// that state — it has a user from the moment its task starts — and an empty
// name there fails CLOSED, as an account that cannot be an admin.
bool fs_session_restricted(void) { return ident_restricted(); }

bool fs_may_read(const char *abs) {
    if (!fs_session_restricted()) return true;
    return !protect_secret(abs) && !protect_home(abs, ident_user(), false);
}

bool fs_may_change(const char *abs) {
    if (!fs_session_restricted()) return true;
    return !protect_reaches(abs) && !protect_system(abs) && !protect_home(abs, ident_user(), true);
}

static void refuse(const char *abs, const char *verb) {
    out_err("Permission denied: %s is protected. Only an admin can %s it.", abs, verb);
    log_addf(LOG_K_WARN, "[Perm] %s%s refused %s on %.60s", ident_user(), ident_remote() ? " (ssh)" : "", verb, abs);
}

// An empty path is what path_resolve leaves when a path was too long to
// resolve, and to littlefs it means the root. Every command that resolves a path
// asks one of these two next, so this is where a too-long path stops.
static bool too_long(const char *abs) {
    if (abs && abs[0]) return false;
    out_err("Path too long.");
    return true;
}

bool fs_allow_read(const char *abs) {
    if (too_long(abs)) return false;
    if (fs_may_read(abs)) return true;
    refuse(abs, "read");
    return false;
}

bool fs_allow_change(const char *abs) {
    if (too_long(abs)) return false;
    if (fs_may_change(abs)) return true;
    refuse(abs, "change");
    return false;
}
