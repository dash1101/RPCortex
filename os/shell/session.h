// Login and first-run setup — the C++ replacement for initialization.py's
// login_seq / setup_seq. Runs after kboot and before the shell.
#ifndef RPC_SESSION_H
#define RPC_SESSION_H

// First-run setup if there are no accounts, then a login loop. Blocks until a
// user authenticates. Sets the active user (System.Active_User in the registry).
void session_boot(void);

// The logged-in user, for the prompt and whoami. "root" until login completes.
const char *session_user(void);

// Log out: clear the active user so session_boot's login loop runs again.
void session_logout(void);

// One line of input at the standard prompt, optionally masked. Shared with the
// user-management commands so every prompt on the device — login, passwd,
// "are you sure" — looks and behaves the same.
void session_prompt(const char *msg, char *buf, unsigned max, bool secret);

// Ask a yes/no question. True only for a full "yes", matching v1: a destructive
// action should not proceed on a stray keypress.
bool session_confirm(const char *msg);

// Register session commands (`autonomy`). Separate from session_boot because
// the command has to exist whether or not anyone has logged in yet.
void session_register(void);

#endif  // RPC_SESSION_H
