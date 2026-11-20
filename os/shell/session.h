// Login and first-run setup — the C++ replacement for initialization.py's
// login_seq / setup_seq. Runs after kboot and before the shell.
#ifndef RPC_SESSION_H
#define RPC_SESSION_H

// First-run setup if there are no accounts, then a login loop. Blocks until a
// user authenticates. Sets the active user (System.Active_User in the registry).
void session_boot(void);

#include <stdint.h>
#include "task.h"

// The user the CALLING TASK acts for: its own when it belongs to a remote
// login (or was started from one), otherwise whoever is logged in at the
// console. "root" until the console's login completes. Every permission check
// asks this; see TaskShell in task.h.
const char *session_user(void);

// Does the calling task belong to a remote login?
bool session_is_remote(void);

// Make the calling task a remote login's: its user, its terminal, its Ctrl+C
// domain, and a working directory of /home/<user> when that exists. Called by
// the SSH server as the first thing a session's shell task does, before any
// command can run.
void session_attach(const char *user, const TaskTerm *term, uint16_t intr_dom);

// Log out. At the console: clear the active user so session_boot's login loop
// runs again. In a remote login: end that login, and leave the console alone.
void session_logout(void);

// One line of input at the standard prompt, optionally masked. Shared with the
// user-management commands so every prompt on the device — login, passwd,
// "are you sure" — looks and behaves the same.
void session_prompt(const char *msg, char *buf, unsigned max, bool secret);

// Ask a yes/no question. True only for a full "yes", matching v1: a destructive
// action should not proceed on a stray keypress.
bool session_confirm(const char *msg);

// Ask for the signed-in user's password again, and say what it is for.
//
// "Are you sure" asks the terminal, not the person. Anything that changes who
// can reach this device — turning the login prompt off, adding an account,
// wiping it — should cost the password of somebody who already had it, because
// the realistic threat is an unattended session rather than a mistyped command.
//
// A pass is remembered for REAUTH_GRACE_MS, so a run of privileged commands
// asks once rather than five times. The grace is per user and is dropped the
// moment anybody logs out.
//
// A NOPASS account has nothing to check, so it is refused outright rather than
// waved through: an account that signs in without a password must not be able
// to authorise something a password was the point of.
bool session_reauth(const char *what_for);

// How long a successful re-authentication counts for. Two minutes is long
// enough to finish what was started and short enough that walking away ends it.
#define REAUTH_GRACE_MS 120000

// Forget any grace period. Called at logout, and by anything that has reason to
// believe the person at the keyboard changed.
void session_reauth_forget(void);

// Register session commands (`autonomy`). Separate from session_boot because
// the command has to exist whether or not anyone has logged in yet.
void session_register(void);

#endif  // RPC_SESSION_H
