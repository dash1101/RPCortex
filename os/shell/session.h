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

#endif  // RPC_SESSION_H
