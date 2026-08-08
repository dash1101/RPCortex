// Login and first-run setup — the C++ replacement for initialization.py's
// setup_seq / login_seq.
//
// v1's first run was a guided six-step walk-through rather than a bare password
// prompt, and that is most of why the OS felt approachable on a first boot. The
// same shape is kept here: numbered steps, a sentence of context before each
// question, a sensible default in brackets, and a confirmation that says how to
// change the answer later. The step count is five rather than six because two of
// v1's steps (NTP sync, verbose boot) depend on machinery v2 does not have yet —
// a step that cannot do anything is worse than one fewer step.
//
// The login loop keeps v1's behaviour exactly: a NOPASS account signs in with no
// password prompt at all, a wrong password backs off on an escalating delay, and
// three misses return to the username prompt rather than locking the device.

#include "session.h"
#include "users.h"
#include "registry.h"
#include "persist.h"
#include "kernel.h"
#include "command.h"
#include "logring.h"
#include "out.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"

// wifi's command entry, so setup can offer to connect without duplicating it.
int  net_setup_scan_and_join(void);
bool net_available(void);

static char g_user[24] = "root";

// Consecutive failed passwords across the whole session, not just this attempt
// run — the backoff should keep growing for a scripted attacker who restarts at
// the username prompt.
static uint32_t g_login_fails;

const char *session_user(void) { return g_user; }
void session_logout(void) {
    // Their settings are written back and put away before the name goes. In the
    // other order there is no scope left to save and the session's changes are
    // simply lost.
    persist_scope_leave();
    g_user[0] = 0;
    session_reauth_forget();
}

// Line input with optional masking. Separate from the shell's reader because a
// password must not echo its characters — it prints '•' instead, which shows the
// user their typing landed without putting the secret on screen.
static void read_field(const char *prompt, char *buf, size_t max, bool secret) {
    out_prompt(prompt);
    size_t n = 0;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            // YIELD, not sleep. This loop waits on a human typing a password,
            // which can take a minute, and the watchdog is only fed by the
            // scheduler — a sleep here fed nothing and the device rebooted every
            // eight seconds in the middle of first-run setup.
            //
            // It also lets background tasks run while someone is at the login
            // prompt, which sleeping did not.
            task_yield();
            continue;
        }
        // out_write, not putchar/printf: it holds the output lock, and a
        // background task printing into a half-echoed password is what
        // corrupted stdio and hard faulted.
        if (c == '\r' || c == '\n') { out_write("\n", 1); buf[n] = 0; return; }
        if ((c == 8 || c == 127) && n) { n--; out_write("\b \b", 3); continue; }
        if (c >= 32 && c < 127 && n + 1 < max) {
            buf[n++] = (char)c;
            if (secret) out_write("\xe2\x80\xa2", 3);   // •
            else        out_write((const char *)&c, 1);
        }
    }
}

// autonomy status | on [user] | off
//
// Lives here because accept() is what a login actually is, and it is static to
// this file. Admin-only: turning the lock screen off for everyone is exactly the
// sort of thing that should not be one command away from a guest account.
static int cmd_autonomy(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    if (strcmp(sub, "status") == 0) {
        const char *u = reg_get("System.Autonomous", "");
        if (!u[0]) { out_infop("autonomy", "Off — this device asks for a login."); return 0; }
        if (!users_exists(u))
            out_warnp("autonomy", "Set to '%s', which no longer exists. It will ask instead.", u);
        else
            out_okp("autonomy", "On, as '%s'.", u);
        return 0;
    }

    if (strcmp(sub, "off") == 0) {
        // Turning it OFF is protected too, and that is not symmetry for its own
        // sake: on a device that boots straight to a shell, `autonomy off`
        // followed by a reboot is how somebody locks the owner out of their own
        // hardware. Both directions change who can reach this device.
        if (!session_reauth("turn autonomy off")) return 1;
        reg_set("System.Autonomous", "");
        persist_save_dirty();
        out_ok("Autonomy off. This device will ask for a login.");
        log_add(LOG_K_WARN, "autonomy: disabled");
        return 0;
    }

    if (strcmp(sub, "on") == 0) {
        // Defaults to whoever is turning it on, since that is nearly always the
        // intent and naming yourself is a strange thing to have to do.
        const char *who = argc > 2 ? argv[2] : session_user();
        if (!who || !who[0]) { out_err("No user to log in as."); return 1; }
        if (!users_exists(who)) { out_err("No such user '%s'.", who); return 1; }
        out_warn("This device will boot straight to a shell as '%s'.", who);
        out_multi("  Anyone with physical access has that account without a password.");
        if (!session_confirm("Enable autonomy")) { out_info("Cancelled."); return 1; }
        // The password AFTER the warning and the yes, so somebody who did not
        // mean it has already left, and the one prompt that costs real effort is
        // the last thing between the intent and the change.
        //
        // Turning the login prompt off is the single largest change to this
        // device's security posture that one command can make, and it was
        // reachable from an unattended admin shell with the word "yes".
        if (!session_reauth("turn autonomy on")) return 1;
        reg_set("System.Autonomous", who);
        persist_save_dirty();
        out_ok("Autonomy on, as '%s'.", who);
        log_addf(LOG_K_WARN, "autonomy: enabled as '%s'", who);
        return 0;
    }

    out_multi("Usage: autonomy status      is it on, and as whom");
    out_multi("       autonomy on [user]   boot straight to a shell (defaults to you)");
    out_multi("       autonomy off         ask for a login again");
    return 1;
}

void session_register(void) {
    static const Command c{"autonomy", "run with no login prompt", cmd_autonomy,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}

void session_prompt(const char *msg, char *buf, unsigned max, bool secret) {
    read_field(msg, buf, max, secret);
}

bool session_confirm(const char *msg) {
    char ans[8];
    char q[96];
    snprintf(q, sizeof(q), "%s (yes/no)", msg);
    read_field(q, ans, sizeof(ans), false);
    return strcmp(ans, "yes") == 0;
}

// --- re-authentication --------------------------------------------------------
//
// See the note in session.h for why "are you sure" is not enough on its own.

static char     g_reauth_user[USR_NAME_MAX + 1];
static uint32_t g_reauth_at;
static bool     g_reauth_have;

void session_reauth_forget(void) {
    g_reauth_have = false;
    g_reauth_user[0] = 0;
    // The password itself is never stored — only the fact that one was checked
    // and when. There is nothing here to wipe beyond the name and the clock.
}

bool session_reauth(const char *what_for) {
    const char *who = session_user();
    if (!who || !who[0]) { out_err("Nobody is signed in."); return false; }

    // A NOPASS account has no secret to prove. Waving it through would make the
    // check theatre on exactly the accounts where it matters most.
    if (users_is_nopass(who)) {
        out_errp("auth", "'%s' signs in without a password, so it cannot "
                         "authorise this.", who);
        out_multi("  `passwd %s` gives it one.", who);
        return false;
    }

    uint32_t now = task_now_ms();
    if (g_reauth_have && strcmp(g_reauth_user, who) == 0 &&
        (uint32_t)(now - g_reauth_at) < REAUTH_GRACE_MS) {
        return true;
    }

    // Three attempts, then a refusal. The same shape as the login loop rather
    // than a new one — somebody mistyping a password should meet the behaviour
    // they already know.
    for (int tries = 0; tries < 3; tries++) {
        char pw[USR_CRED_MAX];
        char prompt[96];
        snprintf(prompt, sizeof(prompt), "Password for %s, to %s", who, what_for);
        read_field(prompt, pw, sizeof(pw), true);

        bool ok = users_verify(who, pw);
        // Not left on the stack for whatever runs next in this frame.
        for (unsigned i = 0; i < sizeof(pw); i++) pw[i] = 0;

        if (ok) {
            snprintf(g_reauth_user, sizeof(g_reauth_user), "%s", who);
            g_reauth_at   = now;
            g_reauth_have = true;
            return true;
        }
        out_err("Wrong password.");
    }
    // LOGGED, always. A refused privileged action is the one somebody wants to
    // find afterwards, and it is the only record that the attempt happened.
    log_addf(LOG_K_WARN, "auth: refused '%s' three times, to %s", who, what_for);
    return false;
}

// A [Y/n] question: blank means yes. v1's default for anything it wanted people
// to say yes to.
static bool ask_yes_default(const char *msg) {
    char a[8];
    read_field(msg, a, sizeof(a), false);
    if (a[0] == 0) return true;
    return a[0] == 'y' || a[0] == 'Y';
}

// --- first run --------------------------------------------------------------

static void setup_root(void) {
    out_info("[1/5] Root account");
    out_multi("  'root' is the system administrator.");
    out_blank();
    if (users_exists("root")) {
        out_info("  Root account already exists - skipping.");
        return;
    }
    char pw[40], confirm[40];
    while (true) {
        read_field("  Set a password for 'root'", pw, sizeof(pw), true);
        if (pw[0] == 0) { out_warn("  Password cannot be blank."); continue; }
        read_field("  Confirm password", confirm, sizeof(confirm), true);
        if (strcmp(pw, confirm) != 0) { out_err("  Passwords do not match.  Try again."); continue; }
        break;
    }
    if (users_add("root", pw, /*admin*/true, /*nopass*/false)) out_ok("  Root account created.");
    else                                                       out_err("  Could not create root account.");
}

static void setup_owner(void) {
    out_info("[2/5] Owner");
    out_multi("  Optional - who owns this device? (shown in sysinfo)");
    out_blank();
    char owner[REG_VAL_MAX];
    read_field("  Owner name [skip]", owner, sizeof(owner), false);
    if (owner[0]) {
        reg_set("System.Owner", owner);
        out_ok("  Owner set to '%s'.", owner);
    } else {
        out_ok("  Skipped.  Set later: reg set System.Owner <name>");
    }
}

static void setup_device_name(void) {
    out_info("[3/5] Device name");
    out_multi("  Optional - appears in the shell prompt:  user@<name>");
    out_multi("  Leave blank to keep the default 'vela'.");
    out_blank();
    char devid[REG_VAL_MAX];
    read_field("  Device name [vela]", devid, sizeof(devid), false);
    if (devid[0]) {
        reg_set("System.Device_ID", devid);
        out_ok("  Device name set to '%s'.", devid);
    } else {
        out_ok("  Keeping 'vela'.  Change later: reg set System.Device_ID <name>");
    }
}

static bool setup_wifi(void) {
    out_info("[4/5] WiFi");
    if (!net_available()) {
        out_ok("  No WiFi hardware detected - skipping.");
        return false;
    }
    out_multi("  Connect now so the device can reach the package repo.");
    out_blank();
    if (!ask_yes_default("  Set up WiFi now? [Y/n]")) {
        out_ok("  Skipped.  Set up later with: wifi scan / wifi connect <ssid>");
        return false;
    }
    if (net_setup_scan_and_join() != 0) {
        out_warn("  Could not connect.  Set up later with: wifi connect <ssid>");
        return false;
    }
    if (ask_yes_default("  Reconnect automatically on boot? [Y/n]")) {
        reg_set("WiFi.Auto", "true");
        out_ok("  Autoconnect enabled.");
    }
    return true;
}

static void setup_time(bool wifi_ok) {
    out_info("[5/5] Time");
    out_multi("  Timezone offset from UTC in whole hours (e.g. -5 = US Eastern).");
    out_blank();
    char tz[12];
    read_field("  Timezone offset [0]", tz, sizeof(tz), false);
    if (tz[0]) {
        char *end = nullptr;
        long v = strtol(tz, &end, 10);
        if (end != tz && *end == 0 && v >= -12 && v <= 14) {
            char norm[8]; snprintf(norm, sizeof(norm), "%ld", v);
            reg_set("System.TZ_Offset", norm);
            out_ok("  Timezone set to UTC%+ld.", v);
        } else {
            out_warn("  Not an hour offset - skipping.  Set later: reg set System.TZ_Offset <hours>");
        }
    } else {
        out_ok("  Keeping UTC (0).");
    }
    if (!wifi_ok)
        out_multi("  (No WiFi - set the clock with 'date set YYYY-MM-DD HH:MM:SS'.)");
    else
        out_multi("  Set the clock with 'date set YYYY-MM-DD HH:MM:SS'.");
}

static void first_run(void) {
    out_blank();
    out_info("=== RPCortex %s - First Run Setup ===", RPC_OS_VERSION);
    out_blank();
    out_info("Welcome. A few questions, once.");
    out_multi("  Five quick steps - everything here can be changed later.");
    out_blank();

    setup_root();       out_blank();
    setup_owner();      out_blank();
    setup_device_name();out_blank();
    bool wifi_ok = setup_wifi(); out_blank();
    setup_time(wifi_ok);out_blank();

    // Silent: the guest account, as v1 created it at the end of setup.
    if (!users_exists("guest")) users_add("guest", nullptr, /*admin*/false, /*nopass*/true);

    reg_set("System.Setup", "true");
    persist_save_users();
    persist_save_registry();

    out_blank();
    out_ok("All set!");
    out_multi("  Log in with 'root' or 'guest'.");
    out_blank();
}

// --- login ------------------------------------------------------------------

static void accept(const char *name) {
    g_login_fails = 0;
    snprintf(g_user, sizeof(g_user), "%s", name);
    reg_set("System.Active_User", g_user);
    // Their own settings, before anything reads one. Any "User." key looked up
    // between here and the prompt would otherwise get the device default and
    // then be written back as theirs.
    persist_scope_enter(g_user);
    persist_save_dirty();
    out_ok("Welcome, %s!", g_user);
}

void session_boot(void) {
    if (users_count() == 0 || strcmp(reg_get("System.Setup", "false"), "true") != 0)
        first_run();

    // Autonomous: come up logged in, with no prompt at all. For a device that
    // lives on a shelf doing a job, where there is nobody present to type a
    // password and a login prompt is just a thing that stops it working.
    //
    // AFTER first_run deliberately — a fresh device still gets its setup — and
    // guarded on the account still existing, because removing the autonomous
    // user would otherwise leave a device that logs in as nobody and has no way
    // to reach a prompt.
    const char *auto_user = reg_get("System.Autonomous", "");
    if (auto_user[0]) {
        if (users_exists(auto_user)) {
            out_infop("autonomy", "Logging in as '%s' without a prompt.", auto_user);
            accept(auto_user);
            return;
        }
        out_warnp("autonomy", "Account '%s' no longer exists — asking instead.", auto_user);
    }

    out_info("=== Login ===");
    out_multi("  Type 'root' or 'guest' to log in.");
    out_blank();

    char name[24], pw[40];
    while (true) {
        read_field("Username", name, sizeof(name), false);
        if (name[0] == 0) { out_warn("Please enter a username."); continue; }
        if (!users_exists(name)) {
            out_warn("User '%s' not found.", name);
            out_multi("  Available accounts: root, guest  |  New users: run 'mkacct' after login");
            continue;
        }
        // A NOPASS account signs in with no password prompt at all — asking for
        // one and then ignoring the answer, which is what a shared prompt would
        // do, teaches people their password does not matter.
        if (users_is_nopass(name)) {
            out_info("No password required for '%s'.", name);
            accept(name);
            return;
        }

        int attempts = 0;
        while (true) {
            read_field("Password", pw, sizeof(pw), true);
            if (pw[0] == 0) { out_warn("Password cannot be blank."); continue; }
            if (users_verify(name, pw)) { accept(name); return; }

            g_login_fails++;
            // Escalating backoff: ~0.5 s for the first miss, growing to a 5 s
            // cap. Slows a scripted brute-force without locking someone out of
            // their own device over one typo.
            // task_sleep_ms, not sleep_ms: a raw sleep parks the core without
            // reaching the scheduler, so nothing feeds the watchdog and a five
            // second backoff comes within one of tripping it. Sleeping through
            // the scheduler also lets background work continue during the pause.
            uint32_t wait = 500 + (g_login_fails - 1) * 750;
            task_sleep_ms(wait > 5000 ? 5000 : wait);

            if (++attempts < 3) {
                out_warn("Incorrect password.  Attempt %d/3.", attempts);
            } else {
                out_err("Too many failed attempts.  Returning to username prompt.");
                if (g_login_fails >= 6) {
                    out_warn("Repeated failures detected — cooling down.");
                    task_sleep_ms(5000);
                }
                break;
            }
        }
    }
}
