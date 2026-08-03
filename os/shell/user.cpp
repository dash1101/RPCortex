// Account commands — the v1 sys_user.py set.
//
// whoami / users / mkacct / usermod / passwd / rmuser / logout, with v1's
// wording and, more importantly, v1's guard rails: only an admin may create,
// modify or remove an account; the active user cannot be removed; root and guest
// are protected; and anything destructive asks for a typed "yes".
//
// A non-admin changing their OWN password is allowed and is the one case that
// does not need the admin check — otherwise a normal user could never rotate a
// credential without an admin present.

#include "command.h"
#include "out.h"
#include "session.h"
#include "users.h"
#include "registry.h"
#include "persist.h"
#include "storage.h"

#include <stdio.h>
#include <string.h>

// Every management command funnels through this so the rule lives in one place.
static bool require_admin(const char *what) {
    if (users_is_admin(session_user())) return true;
    out_err("'%s' requires an admin account.", what);
    return false;
}

// Read a new password twice. Returns false (with the reason already printed) if
// the user gave a blank or mismatched pair.
static bool read_new_password(const char *prompt_msg, char *out, unsigned cap) {
    char confirm[40];
    session_prompt(prompt_msg, out, cap, true);
    if (out[0] == 0) { out_warn("Password cannot be blank."); return false; }
    session_prompt("Confirm password", confirm, sizeof(confirm), true);
    if (strcmp(out, confirm) != 0) { out_err("Passwords do not match."); return false; }
    return true;
}

static int cmd_whoami(int, char **) {
    const char *u = session_user();
    out_multi("%s%s", u && u[0] ? u : "(unknown)",
              users_is_admin(u) ? "  (admin)" : "");
    return 0;
}

static int cmd_users(int, char **) {
    uint32_t n = users_count();
    if (!n) { out_warn("No user accounts found."); return 1; }
    out_info("User accounts (%u):", (unsigned)n);
    for (uint32_t i = 0; i < n; i++) {
        const char *name = users_name_at(i);
        char home[USR_HOME_MAX];
        reg_note_home(name, home, sizeof(home));
        out_multi("  %-16s %-18s%s%s%s", name, home,
                  users_is_admin(name)             ? "  admin"  : "",
                  users_is_nopass(name)            ? "  nopass" : "",
                  strcmp(name, session_user()) == 0 ? "  active" : "");
    }
    return 0;
}

// mkacct <name> [--admin] [--nopass]
static int cmd_mkacct(int argc, char **argv) {
    if (!require_admin("mkacct")) return 1;

    char name[USR_NAME_MAX];
    bool admin = false, nopass = false;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--admin"))  admin  = true;
        if (!strcmp(argv[i], "--nopass")) nopass = true;
    }
    if (argc >= 2) { strncpy(name, argv[1], sizeof(name) - 1); name[sizeof(name) - 1] = 0; }
    else {
        out_info("Create a new user account.");
        session_prompt("Username", name, sizeof(name), false);
    }
    if (name[0] == 0)      { out_warn("Username cannot be blank."); return 1; }
    if (users_exists(name)) { out_err("User '%s' already exists.", name); return 1; }

    char pw[40] = {0};
    if (!nopass && !read_new_password("Password", pw, sizeof(pw))) return 1;

    if (!users_add(name, nopass ? nullptr : pw, admin, nopass)) {
        out_err("Failed to create account '%s'.", name);
        return 1;
    }
    persist_save_users();
    char home[USR_HOME_MAX + 8];
    snprintf(home, sizeof(home), "/home/%s", name);
    storage_mkdir(home);
    out_ok("Account '%s' created%s%s.", name,
           admin ? " (admin)" : "", nopass ? " (no password)" : "");
    out_multi("  Home: %s", home);
    return 0;
}

// passwd [user] — own password with no argument; another user's needs admin.
static int cmd_passwd(int argc, char **argv) {
    const char *target = (argc >= 2) ? argv[1] : session_user();
    bool own = strcmp(target, session_user()) == 0;
    if (!own && !require_admin("passwd")) return 1;
    if (!users_exists(target)) { out_err("User '%s' not found.", target); return 1; }

    // Changing your own password proves you hold the current one first. An admin
    // resetting someone else's does not — that is the point of an admin reset.
    if (own && !users_is_nopass(target)) {
        char cur[40];
        session_prompt("Current password", cur, sizeof(cur), true);
        if (!users_verify(target, cur)) { out_err("Incorrect password."); return 1; }
    }

    char pw[40];
    char msg[64];
    snprintf(msg, sizeof(msg), "New password for '%s'", target);
    if (!read_new_password(msg, pw, sizeof(pw))) return 1;
    if (!users_set_password(target, pw)) { out_err("Could not set the password."); return 1; }
    persist_save_users();
    out_ok("Password updated for '%s'.", target);
    return 0;
}

// usermod <user> admin on|off | nopass on|off | passwd
static int cmd_usermod(int argc, char **argv) {
    if (!require_admin("usermod")) return 1;
    if (argc < 3) {
        out_warn("Usage: usermod <user> passwd | admin on|off | nopass on|off");
        return 1;
    }
    const char *user = argv[1];
    const char *sub  = argv[2];
    if (!users_exists(user)) { out_err("User '%s' not found.", user); return 1; }

    if (!strcmp(sub, "passwd")) {
        char pw[40], msg[64];
        snprintf(msg, sizeof(msg), "New password for '%s'", user);
        if (!read_new_password(msg, pw, sizeof(pw))) return 1;
        if (!users_set_password(user, pw)) { out_err("Could not set the password."); return 1; }
        persist_save_users();
        out_ok("Password updated for '%s'.", user);
        return 0;
    }

    if (!strcmp(sub, "nopass")) {
        if (argc < 4) { out_warn("Usage: usermod <user> nopass on|off"); return 1; }
        if (!strcmp(argv[3], "on")) {
            if (!users_set_nopass(user)) { out_err("Could not change '%s' — admins must keep a password.", user); return 1; }
            persist_save_users();
            out_ok("'%s' now signs in without a password.", user);
            return 0;
        }
        if (!strcmp(argv[3], "off")) {
            char pw[40], msg[64];
            snprintf(msg, sizeof(msg), "Set a password for '%s'", user);
            if (!read_new_password(msg, pw, sizeof(pw))) return 1;
            if (!users_set_password(user, pw)) { out_err("Could not set the password."); return 1; }
            persist_save_users();
            out_ok("'%s' now requires a password.", user);
            return 0;
        }
        out_warn("Usage: usermod <user> nopass on|off");
        return 1;
    }

    if (!strcmp(sub, "admin")) {
        if (argc < 4) { out_warn("Usage: usermod <user> admin on|off"); return 1; }
        bool on = !strcmp(argv[3], "on");
        if (!on && strcmp(argv[3], "off")) { out_warn("Usage: usermod <user> admin on|off"); return 1; }
        // Removing the last admin would leave the device unmanageable, with no
        // way back short of a factory reset.
        if (!on) {
            uint32_t admins = 0;
            for (uint32_t i = 0; i < users_count(); i++)
                if (users_is_admin(users_name_at(i))) admins++;
            if (admins <= 1 && users_is_admin(user)) {
                out_err("'%s' is the only admin — promote another account first.", user);
                return 1;
            }
        }
        if (!users_set_admin(user, on)) { out_err("Could not change '%s'.", user); return 1; }
        persist_save_users();
        out_ok("'%s' is %s an admin.", user, on ? "now" : "no longer");
        return 0;
    }

    out_warn("Unknown action '%s'. Use: passwd | admin | nopass", sub);
    return 1;
}

static int cmd_rmuser(int argc, char **argv) {
    if (!require_admin("rmuser")) return 1;
    if (argc < 2) { out_warn("Usage: rmuser <username>"); return 1; }
    const char *target = argv[1];
    if (!strcmp(target, "root") || !strcmp(target, "guest")) {
        out_err("'%s' is a protected account and cannot be removed.", target);
        return 1;
    }
    if (!strcmp(target, session_user())) {
        out_err("Cannot remove the currently active user.");
        return 1;
    }
    if (!users_exists(target)) { out_err("User '%s' not found.", target); return 1; }

    char msg[64];
    snprintf(msg, sizeof(msg), "Remove user '%s' ?", target);
    if (!session_confirm(msg)) { out_info("Cancelled."); return 0; }

    if (!users_remove(target)) { out_err("Could not remove '%s'.", target); return 1; }
    persist_save_users();
    out_ok("User '%s' removed.", target);
    return 0;
}

static int cmd_logout(int, char **) {
    out_info("Logging out...");
    session_logout();
    session_boot();          // straight back to the login prompt
    return 0;
}

void user_register(void) {
    static const Command cmds[] = {
        {"whoami",  "the logged-in user",         cmd_whoami,  nullptr},
        {"users",   "list accounts",              cmd_users,   nullptr},
        {"mkacct",  "mkacct <name> [--admin]",    cmd_mkacct,  nullptr, LEVEL_ADMIN},
        {"passwd",  "passwd [user]",              cmd_passwd,  nullptr, LEVEL_ADMIN},
        {"usermod", "usermod <user> <action>",    cmd_usermod, nullptr, LEVEL_ADMIN},
        {"rmuser",  "rmuser <username>",          cmd_rmuser,  nullptr, LEVEL_ADMIN},
        {"logout",  "return to the login prompt", cmd_logout,  nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);

    cmd_alias("id",   "whoami");
    cmd_alias("exit", "logout");
}
