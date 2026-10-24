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

bool fs_allow_read(const char *abs) {
    if (fs_may_read(abs)) return true;
    refuse(abs, "read");
    return false;
}

bool fs_allow_change(const char *abs) {
    if (fs_may_change(abs)) return true;
    refuse(abs, "change");
    return false;
}
