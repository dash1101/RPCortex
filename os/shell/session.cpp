#include "session.h"
#include "users.h"
#include "registry.h"
#include "persist.h"
#include "kernel.h"
#include "out.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

static char g_user[24] = "root";

const char *session_user(void) { return g_user; }
void session_logout(void) { g_user[0] = 0; }

// Line input with optional masking. Separate from the shell's reader because a
// password must not echo its characters — it prints '*' instead, which shows the
// user their typing landed without putting the secret on screen.
static void read_field(const char *prompt, char *buf, size_t max, bool secret) {
    out_prompt(prompt);
    size_t n = 0;
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) { sleep_ms(2); continue; }
        if (c == '\r' || c == '\n') { putchar('\n'); buf[n] = 0; return; }
        if ((c == 8 || c == 127) && n) { n--; printf("\b \b"); continue; }
        if (c >= 32 && c < 127 && n + 1 < max) {
            buf[n++] = (char)c;
            if (secret) printf("\u2022"); else putchar(c);
        }
    }
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

// First run: no accounts exist yet. Create root (admin) with a chosen password
// and a NOPASS guest, matching v1's setup. A non-blank root password is required
// — an admin account with no password is not a setup, it is a hole.
static void first_run(void) {
    out_info("Welcome! Let's get your device configured.");
    char pw[40], confirm[40];
    while (true) {
        read_field("Set a password for 'root'", pw, sizeof(pw), true);
        if (strlen(pw) == 0) { out_warn("  Password cannot be blank."); continue; }
        read_field("Confirm password", confirm, sizeof(confirm), true);
        if (strcmp(pw, confirm) != 0) { out_err("  Passwords do not match.  Try again."); continue; }
        break;
    }
    users_add("root", pw, /*admin*/true, /*nopass*/false);
    users_add("guest", nullptr, /*admin*/false, /*nopass*/true);
    reg_set("System.Setup", "true");
    persist_save_users();
    persist_save_registry();
    out_ok("Setup complete.");
}

void session_boot(void) {
    if (users_count() == 0) first_run();
    out_info("=== Login ===");

    char name[24], pw[40];
    while (true) {
        read_field("Username", name, sizeof(name), false);
        if (name[0] == 0) continue;
        if (!users_exists(name)) { out_err("User not found."); continue; }
        // A NOPASS account (guest) still asks, so the flow looks the same, but
        // any answer is accepted.
        read_field("Password", pw, sizeof(pw), true);
        if (users_verify(name, pw)) break;
        out_err("Incorrect password.");
    }
    strncpy(g_user, name, sizeof(g_user) - 1);
    g_user[sizeof(g_user) - 1] = 0;
    reg_set("System.Active_User", g_user);
    persist_save_dirty();
    out_ok("Welcome, %s!", g_user);
}
