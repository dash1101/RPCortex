#include "ident.h"
#include "users.h"

#include <stdio.h>
#include <string.h>

static char g_console[TASK_USER_MAX] = "root";

const char *ident_user(void) {
    TaskShell *ts = task_shell();
    return ts->own_user ? ts->user : g_console;
}

bool ident_remote(void) { return task_shell()->own_user; }

void ident_console_set(const char *user) {
    snprintf(g_console, sizeof(g_console), "%s", user ? user : "");
}

const char *ident_console(void) { return g_console; }

void ident_attach(const char *user, const TaskTerm *term, uint16_t intr_dom, const char *cwd) {
    TaskShell *ts = task_shell();
    snprintf(ts->user, sizeof(ts->user), "%s", user ? user : "");
    snprintf(ts->cwd, sizeof(ts->cwd), "%s", cwd && cwd[0] ? cwd : "/");
    ts->term = term;
    ts->intr_dom = intr_dom;
    // Last: from here on every check this task makes is answered for `user`.
    ts->own_user = true;
}

bool ident_restricted(void) {
    TaskShell *ts = task_shell();
    if (ts->own_user) return !ts->user[0] || !users_is_admin(ts->user);
    return g_console[0] && !users_is_admin(g_console);
}
