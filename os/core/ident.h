// Who the calling task acts for.
//
// There used to be one answer for the whole device — whoever was logged in at
// the console — and every permission check asked it. A login over SSH is a
// second user at the same moment, so the answer is now per TASK (see TaskShell
// in task.h): a task belonging to a remote login answers with that login's
// user, and every other task follows the console exactly as before.
//
// Pure, so the rule is host-tested with the real code rather than a stand-in:
// session.cpp and fsguard.cpp are thin wrappers over this.
#ifndef RPC_IDENT_H
#define RPC_IDENT_H

#include <stdint.h>
#include "task.h"

// The user the calling task acts for: its own, or the console's.
const char *ident_user(void);

// Does the calling task carry an identity of its own (a remote login, or
// something started from one)?
bool ident_remote(void);

// The console's login. Set by the login prompt, cleared at logout.
void        ident_console_set(const char *user);
const char *ident_console(void);

// Make the calling task a remote login's. `cwd` is where it starts.
void ident_attach(const char *user, const TaskTerm *term, uint16_t intr_dom, const char *cwd);

// Is the calling task's user NOT an admin? The console with nobody signed in
// (boot, the login prompt) is unrestricted, as it always was. A task with an
// identity of its own is never in that state, and an empty name there counts
// as restricted: it fails closed.
bool ident_restricted(void);

// May the calling task stop task `pid` (`kill`)? An admin may stop anything.
// An ordinary account only what is its own: from a remote login, the tasks of
// logins as the same user; at the console, the tasks that follow the console.
// The SSH server, another person's session and the device's services are an
// admin's to stop.
bool ident_may_stop(int pid);

#endif  // RPC_IDENT_H
