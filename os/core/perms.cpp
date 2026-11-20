#include "perms.h"
#include "users.h"

#include <stdio.h>
#include <string.h>

static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static bool is(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

// --- settings ----------------------------------------------------------------

bool perms_reg_key_system(const char *key) {
    if (!key) return true;
    // Exact-case prefixes, as the registry compares keys exactly.
    static const char *const kSystem[] = {
        "System.Autonomous",    // boot straight into a shell as somebody
        "System.Setup",         // first-run setup, which makes a new root
        "System.Active_User",
        "System.SafeBoot",      // and System.SafeBootCmd: run at the next start, with sudo
        "SSH.",                 // the network login
        "Alias.",               // expanded in everyone's shell, root's included
        "Pkg.Repos",            // where `sudo pkg install` fetches from
    };
    for (const char *p : kSystem)
        if (starts(key, p)) return true;
    return false;
}

bool perms_reg_write_ok(const char *key, bool restricted, bool remote, bool elevated) {
    if (!key) return false;
    if (restricted) {
        // Not User.*: those land in the CONSOLE user's own settings (the
        // per-user scope follows the console's login), which a remote account
        // has no business writing.
        if (remote) return starts(key, "Apps.");
        return !perms_reg_key_system(key);
    }
    return elevated || !perms_reg_key_system(key);
}

// --- who may run what ----------------------------------------------------------

bool perms_allows(const char *, bool is_admin, CmdLevel level, bool elevated) {
    if (level == LEVEL_USER) return true;
    // sudo raises the level only for someone who already had it. A non-admin
    // typing sudo gets a refusal, not a prompt: there is no path from a normal
    // account to root on a device with no way to audit one.
    if (!is_admin) return false;
    if (level == LEVEL_ADMIN) return true;
    return elevated;
}

// --- forms: which arguments change the system -----------------------------------
//
// Each rule names the forms that are everyday, and everything else needs sudo.
// That way round on purpose: a subcommand added later is protected until
// somebody decides it is harmless, rather than open until somebody notices.

static bool g_pkg_privileged;
void perms_set_packages_privileged(bool yes) { g_pkg_privileged = yes; }
bool perms_packages_privileged(void) { return g_pkg_privileged; }

static bool ends_rps(const char *p) {
    size_t n = p ? strlen(p) : 0;
    if (n < 4) return false;
    const char *e = p + n - 4;
    return e[0] == '.' && (e[1] | 32) == 'r' && (e[2] | 32) == 'p' && (e[3] | 32) == 's';
}

static CmdLevel lv_pkg(int argc, char **argv, const char *) {
    if (argc < 2) return LEVEL_USER;                              // the usage text
    const char *s = argv[1];
    if (is(s, "list") || is(s, "search") || is(s, "info") || is(s, "update")) return LEVEL_USER;
    if (is(s, "repo") && (argc < 3 || is(argv[2], "list"))) return LEVEL_USER;
    if (is(s, "certs") && argc < 3) return LEVEL_USER;
    return LEVEL_SYSTEM;
}

static CmdLevel lv_update(int argc, char **argv, const char *) {
    if (argc < 2 || is(argv[1], "check")) return LEVEL_USER;
    if ((is(argv[1], "rollback") || is(argv[1], "revert")) && argc < 3) return LEVEL_USER;
    return LEVEL_SYSTEM;
}

static CmdLevel lv_ssh(int argc, char **argv, const char *) {
    if (argc < 2 || !is(argv[1], "server")) return LEVEL_USER;    // the client, keygen, pubkey
    if (argc < 3 || is(argv[2], "status") || is(argv[2], "fingerprint")) return LEVEL_USER;
    return LEVEL_SYSTEM;
}

static CmdLevel lv_list_only(int argc, char **argv, const char *) {
    return (argc < 2 || is(argv[1], "list")) ? LEVEL_USER : LEVEL_SYSTEM;
}

static CmdLevel lv_service(int argc, char **argv, const char *) {
    if (argc < 2 || is(argv[1], "list") || is(argv[1], "status")) return LEVEL_USER;
    return LEVEL_SYSTEM;
}

static CmdLevel lv_autonomy(int argc, char **argv, const char *) {
    return (argc < 2 || is(argv[1], "status")) ? LEVEL_USER : LEVEL_SYSTEM;
}

static CmdLevel lv_mkacct(int argc, char **argv, const char *) {
    for (int i = 1; i < argc; i++)
        if (is(argv[i], "--admin")) return LEVEL_SYSTEM;
    return LEVEL_USER;
}

static CmdLevel lv_passwd(int argc, char **argv, const char *user) {
    bool own = argc < 2 || (user && is(argv[1], user));
    if (!own) return LEVEL_SYSTEM;
    // Your own, proving the current one first. An account with no password has
    // nothing to prove, and giving itself one makes it reachable over SSH, so
    // that stays an admin's to do.
    return (!user || !user[0] || users_is_nopass(user)) ? LEVEL_ADMIN : LEVEL_USER;
}

static CmdLevel lv_safeboot(int argc, char **, const char *) {
    // The staged command runs at the next start with sudo already in force,
    // so staging one is the same as running it with sudo.
    return argc >= 2 ? LEVEL_SYSTEM : LEVEL_USER;
}

static CmdLevel lv_stock(int argc, char **argv, const char *) {
    return (argc >= 2 && is(argv[1], "list")) ? LEVEL_USER : LEVEL_SYSTEM;
}

static CmdLevel lv_reg(int argc, char **argv, const char *) {
    if (argc >= 3 && is(argv[1], "set") && perms_reg_key_system(argv[2])) return LEVEL_SYSTEM;
    return LEVEL_USER;
}

static CmdLevel lv_run(int argc, char **argv, const char *) {
    // A script is lines of commands, each judged on its own. A package on a
    // board with no sandbox is code with every privilege the OS has.
    if (argc >= 2 && g_pkg_privileged && !ends_rps(argv[1])) return LEVEL_SYSTEM;
    return LEVEL_USER;
}

struct FormsRule {
    CmdLevel  (*level)(int argc, char **argv, const char *user);
    uint8_t     words;          // how many words of argv name the operation
    const char *text;           // for `sudo -l`
};

static const FormsRule kForms[FORMS_COUNT] = {
    /* NONE     */ { nullptr, 1, nullptr },
    /* PKG      */ { lv_pkg, 2, "install, remove, upgrade, repo add|remove, certs install" },
    /* UPDATE   */ { lv_update, 2, "install, from-file, rollback --now|--save|--forget" },
    /* SSH      */ { lv_ssh, 3, "server on|off|port|rekey" },
    /* STARTUP  */ { lv_list_only, 2, "add, remove, clear, run" },
    /* TASK     */ { lv_list_only, 2, "add, remove, clear, start, stop, reload" },
    /* SERVICE  */ { lv_service, 2, "add, remove, clear, start" },
    /* AUTONOMY */ { lv_autonomy, 2, "on, off" },
    /* MKACCT   */ { lv_mkacct, 1, "--admin" },
    /* PASSWD   */ { lv_passwd, 1, "another account's" },
    /* SAFEBOOT */ { lv_safeboot, 1, "with a command to run at the next start" },
    /* STOCK    */ { lv_stock, 1, "everything but list" },
    /* REG      */ { lv_reg, 2, "set of System.Autonomous, System.Setup, System.Active_User, "
                                 "System.SafeBoot*, SSH.*, Alias.*, Pkg.Repos" },
    /* RUN      */ { lv_run, 1, "of a package (on this board packages run with the OS's privileges)" },
};

CmdLevel perms_level(CmdLevel base, uint8_t forms, int argc, char **argv, const char *user) {
    if (forms == FORMS_NONE || forms >= FORMS_COUNT || !kForms[forms].level || argc < 1 || !argv)
        return base;
    CmdLevel l = kForms[forms].level(argc, argv, user);
    return l > base ? l : base;
}

const char *perms_forms_text(uint8_t forms) {
    if (forms >= FORMS_COUNT) return nullptr;
    if (forms == FORMS_RUN && !g_pkg_privileged) return nullptr;
    return kForms[forms].text;
}

void perms_label(uint8_t forms, int argc, char **argv, char *out, uint32_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    uint32_t words = forms < FORMS_COUNT ? kForms[forms].words : 1;
    uint32_t o = 0;
    for (int i = 0; i < argc && (uint32_t)i < words && argv && argv[i]; i++) {
        if (i > 0 && argv[i][0] == '-') break;         // an option is not part of the name
        int n = snprintf(out + o, cap - o, "%s%s", i ? " " : "", argv[i]);
        if (n < 0 || (uint32_t)n >= cap - o) { out[cap - 1] = 0; return; }
        o += (uint32_t)n;
    }
}
