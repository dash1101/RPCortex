#include "sudo.h"
#include "ident.h"
#include "users.h"
#include "task.h"
#include "interrupt.h"
#include "lock.h"
#include "out.h"
#include "logring.h"

#include <stdio.h>
#include <string.h>

static SudoReadFn  g_read;
static SudoPauseFn g_pause;
static bool        g_maint;

void sudo_set_reader(SudoReadFn fn)  { g_read = fn; }
void sudo_set_pause(SudoPauseFn fn)  { g_pause = fn; }
void sudo_set_maintenance(bool on)   { g_maint = on; }
bool sudo_maintenance(void)          { return g_maint; }

// --- the grace -------------------------------------------------------------------
//
// Keyed by session AND user. The interrupt domain stands for the session (0 the
// console, one per remote login, never reused with the same value), and the
// user is checked as well, so the same name proving itself at the serial port
// says nothing about who is typing in a remote session, and a different person
// logging in at the console does not inherit it. The password itself is never
// kept; only that one was checked, and when.

struct Grace {
    bool     used;
    uint16_t dom;
    uint32_t at;
    char     user[TASK_USER_MAX];
};
static Grace   g_grace[SUDO_GRACE_SLOTS];
static RpcLock g_grace_lock;
// Wrong passwords given to sudo_check_password in a row, across the device: a
// program can try far faster than a person, so every third is logged.
static uint32_t g_check_fails;

static bool fresh(const Grace &g, uint32_t now) {
    return g.used && (uint32_t)(now - g.at) < SUDO_GRACE_MS;
}

// Is there one for this session and user? `touch` starts its time again.
static bool grace_has(uint16_t dom, const char *user, bool touch) {
    LockGuard _g(&g_grace_lock);
    uint32_t now = task_now_ms();
    for (Grace &g : g_grace) {
        if (!g.used || g.dom != dom || strcmp(g.user, user) != 0) continue;
        if (!fresh(g, now)) { g.used = false; return false; }
        if (touch) g.at = now;
        return true;
    }
    return false;
}

static void grace_put(uint16_t dom, const char *user) {
    LockGuard _g(&g_grace_lock);
    uint32_t now = task_now_ms();
    Grace *slot = nullptr;
    // This session's own entry first (a new user replaces the old one there),
    // then a free or expired slot, then the oldest.
    for (Grace &g : g_grace)
        if (g.used && g.dom == dom) { slot = &g; break; }
    for (Grace &g : g_grace)
        if (!slot && !fresh(g, now)) slot = &g;
    if (!slot) {
        slot = &g_grace[0];
        for (Grace &g : g_grace)
            if ((uint32_t)(now - g.at) > (uint32_t)(now - slot->at)) slot = &g;
    }
    slot->used = true;
    slot->dom  = dom;
    slot->at   = now;
    snprintf(slot->user, sizeof(slot->user), "%s", user);
}

bool sudo_grace_valid(void) {
    const char *who = ident_user();
    return who && who[0] && grace_has(task_shell()->intr_dom, who, false);
}

void sudo_grace_forget(uint16_t dom) {
    LockGuard _g(&g_grace_lock);
    for (Grace &g : g_grace)
        if (g.dom == dom) g.used = false;
}

void sudo_grace_forget_all(void) {
    LockGuard _g(&g_grace_lock);
    for (Grace &g : g_grace) g.used = false;
}

// --- can a person be asked? --------------------------------------------------------

SudoAsk sudo_can_ask(void) {
    TaskShell *ts = task_shell();
    if (ts->noprompt) return SUDO_NOT_HERE;
    if (ts->own_user) {
        // A remote login's own shell has its terminal; a job started from it
        // does not (the terminal is never inherited).
        if (!ts->term) return SUDO_NO_TERMINAL;
        // No pty was asked for: `ssh dev 'cmd'`. Whatever is typed there is
        // echoed by the client and may be in a script, and the same test is
        // what out_to_screen uses to leave colour off.
        uint16_t cols = 0, rows = 0;
        if (!ts->term->size || !ts->term->size(ts->term->ctx, &cols, &rows)) return SUDO_NO_PTY;
        return SUDO_CAN_ASK;
    }
    // Following the console: only the console's own shell task has a person
    // at the keys. Every other task is background work.
    int sp = task_shell_pid();
    if (!sp || task_self() != sp) return SUDO_NO_TERMINAL;
    return SUDO_CAN_ASK;
}

static void say_cannot_ask(SudoAsk a) {
    switch (a) {
    case SUDO_NO_PTY:
        out_err("sudo needs a terminal to ask for the password; connect with ssh -t.");
        break;
    case SUDO_NOT_HERE:
        out_err("sudo needs a password and cannot ask for one here; run `sudo -v` first.");
        break;
    default:
        out_err("sudo needs a password and this job has no terminal; run `sudo -v` first.");
        break;
    }
}

// --- proving it ---------------------------------------------------------------------

bool sudo_authenticate(const char *what_for) {
    TaskShell *ts = task_shell();
    const char *who = ident_user();
    if (!who || !who[0]) { out_err("Nobody is signed in."); return false; }

    // Already proven for what this task is doing, or the system's own work.
    if (ts->elevated) return true;

    // A NOPASS account has no secret to prove. Waving it through would make the
    // check theatre on exactly the accounts where it matters most.
    if (users_is_nopass(who)) {
        out_errp("auth", "'%s' signs in without a password, so it cannot "
                         "authorise this.", who);
        out_multi("  An admin can give it one: sudo passwd %s", who);
        return false;
    }

    if (grace_has(ts->intr_dom, who, /*touch*/true)) return true;

    SudoAsk can = sudo_can_ask();

    // RECOVERY. In a maintenance boot the person at the console's own prompt is
    // not asked. That boot is reached by restarting into it at the console or by
    // the recovery ladder, the SSH server stays off in it, and nothing a remote
    // login can do reaches this console's keyboard: so it is physical presence,
    // which is what recovering a forgotten password has to come down to.
    if (g_maint && can == SUDO_CAN_ASK && !ts->own_user) {
        out_warnp("sudo", "Maintenance boot: not asking for a password at the console.");
        log_addf(LOG_K_WARN, "sudo: %s without a password (maintenance boot), to %s", who,
                 what_for ? what_for : "use sudo");
        grace_put(ts->intr_dom, who);
        return true;
    }

    if (can != SUDO_CAN_ASK || !g_read) {
        ts->sudo_flags |= SUDO_F_BLOCKED;
        say_cannot_ask(can == SUDO_CAN_ASK ? SUDO_NOT_HERE : can);
        return false;
    }

    // Three attempts, then a refusal. The same shape as the login loop rather
    // than a new one: somebody mistyping a password should meet the behaviour
    // they already know.
    for (int tries = 0; tries < 3; tries++) {
        char pw[USR_CRED_MAX];
        char prompt[96];
        snprintf(prompt, sizeof(prompt), "Password for %s, to %s", who,
                 what_for ? what_for : "use sudo");
        pw[0] = 0;
        g_read(prompt, pw, sizeof(pw));
        // A remote login cancelled or going away: no answer is not a wrong one.
        if (!pw[0] && ts->own_user && (task_should_stop() || intr_pending())) return false;

        bool ok = pw[0] && users_verify(who, pw);
        // Not left on the stack for whatever runs next in this frame.
        for (unsigned i = 0; i < sizeof(pw); i++) ((volatile char *)pw)[i] = 0;

        if (ok) {
            grace_put(ts->intr_dom, who);
            return true;
        }
        out_err("Wrong password.");
        if (g_pause) g_pause(1000);
    }
    // LOGGED, always. A refused privileged action is the one somebody wants to
    // find afterwards, and it is the only record that the attempt happened.
    log_addf(LOG_K_WARN, "auth: refused '%s' three times, to %s", who,
             what_for ? what_for : "use sudo");
    // The way back, where it is one: only the console's keyboard reaches it.
    if (!ts->own_user)
        out_multi("  Forgotten? After `safeboot`, sudo at this console does not ask.");
    return false;
}

SudoCheck sudo_check_password(const char *pw) {
    const char *who = ident_user();
    if (!who || !who[0]) return SUDO_NOBODY;
    if (!users_is_admin(who)) {
        log_addf(LOG_K_WARN, "sudo: %s is not an admin, refused", who);
        return SUDO_NOT_ADMIN;
    }
    if (users_is_nopass(who)) return SUDO_NOPASS;
    if (!pw || !pw[0] || !users_verify(who, pw)) {
        uint32_t n;
        {
            LockGuard _g(&g_grace_lock);
            n = ++g_check_fails;
        }
        if (n % 3 == 0)
            log_addf(LOG_K_WARN, "auth: %u wrong passwords in a row for '%s' from a host tool",
                     (unsigned)n, who);
        if (g_pause) g_pause(1000);
        return SUDO_WRONG;
    }
    {
        LockGuard _g(&g_grace_lock);
        g_check_fails = 0;
    }
    grace_put(task_shell()->intr_dom, who);
    return SUDO_OK;
}

// --- the gate --------------------------------------------------------------------------

SudoStep sudo_step(CmdLevel level, bool is_admin, bool prefixed, bool elevated) {
    if (!is_admin && (prefixed || level >= LEVEL_ADMIN)) return SUDO_REFUSE_NOT_ADMIN;
    if (prefixed && !elevated) return SUDO_AUTHENTICATE;
    if (level == LEVEL_SYSTEM && !elevated) return SUDO_REFUSE_NEED_SUDO;
    return SUDO_GO;
}

// The command as typed, for "Run it with sudo: sudo <this>". Shortened rather
// than wrapped: it is a reminder of the shape, not something to copy byte for
// byte from a long line.
static void join_args(int argc, char **argv, char *out, uint32_t cap) {
    uint32_t o = 0;
    out[0] = 0;
    for (int i = 0; i < argc; i++) {
        uint32_t n = (uint32_t)strlen(argv[i]);
        bool space = strchr(argv[i], ' ') != nullptr;
        uint32_t need = (i ? 1 : 0) + n + (space ? 2 : 0);
        if (o + need + 4 >= cap) { snprintf(out + o, cap - o, "%s...", i ? " " : ""); return; }
        o += (uint32_t)snprintf(out + o, cap - o, space ? "%s\"%s\"" : "%s%s", i ? " " : "", argv[i]);
    }
}

bool sudo_gate(CmdLevel base, uint8_t forms, int argc, char **argv, bool prefixed,
               bool *elevated) {
    TaskShell *ts = task_shell();
    const char *who = ident_user();
    bool admin = who && who[0] && users_is_admin(who);
    CmdLevel lvl = perms_level(base, forms, argc, argv, who);
    char label[48];
    perms_label(forms, argc, argv, label, sizeof(label));

    switch (sudo_step(lvl, admin, prefixed, ts->elevated)) {
    case SUDO_REFUSE_NOT_ADMIN:
        if (prefixed) {
            out_err("%s is not an admin. sudo cannot grant what the account does not have.",
                    who && who[0] ? who : "Nobody");
            log_addf(LOG_K_WARN, "sudo: %s is not an admin, refused '%s'", who ? who : "", label);
        } else {
            out_err("'%s' needs an admin account; signed in as %s.", label, who ? who : "");
        }
        return false;
    case SUDO_REFUSE_NEED_SUDO: {
        char line[80];
        join_args(argc, argv, line, sizeof(line));
        out_err("'%s' changes the system. Run it with sudo: sudo %s", label, line);
        return false;
    }
    case SUDO_AUTHENTICATE: {
        char why[64];
        snprintf(why, sizeof(why), "run '%s'", label);
        if (!sudo_authenticate(why)) return false;
        *elevated = true;
        return true;
    }
    case SUDO_GO:
    default:
        *elevated = ts->elevated;
        return true;
    }
}

int sudo_validate(void) {
    const char *who = ident_user();
    if (!who || !who[0] || !users_is_admin(who)) {
        out_err("%s is not an admin, so sudo is not available.", who && who[0] ? who : "Nobody");
        return 1;
    }
    if (!sudo_authenticate("use sudo")) return 1;
    if (task_shell()->elevated) {
        out_ok("This already runs with sudo.");
        return 0;
    }
    out_ok("sudo will not ask again here for %u minutes.", (unsigned)(SUDO_GRACE_MS / 60000u));
    return 0;
}

int sudo_forget(void) {
    sudo_grace_forget(task_shell()->intr_dom);
    out_ok("sudo will ask for the password again.");
    return 0;
}
