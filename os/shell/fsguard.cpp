#include "fsguard.h"
#include "session.h"
#include "users.h"
#include "protect.h"
#include "out.h"
#include "logring.h"

bool fs_session_restricted(void) {
    const char *u = session_user();
    return u && u[0] && !users_is_admin(u);
}

bool fs_may_read(const char *abs) {
    return !fs_session_restricted() || !protect_secret(abs);
}

bool fs_may_change(const char *abs) {
    return !fs_session_restricted() || !protect_reaches(abs);
}

static void refuse(const char *abs, const char *verb) {
    out_err("Permission denied: %s is protected. Only an admin can %s it.", abs, verb);
    log_addf(LOG_K_WARN, "[Perm] %s refused %s on %.60s", session_user(), verb, abs);
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
