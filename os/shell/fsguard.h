// The session's right to touch a path. See core/protect.h for which paths.
//
// An admin may touch anything. So may the system itself: boot, persistence and
// networking write the registry and the user list directly through storage, and
// never come through here. What this stops is an ordinary account, including the
// guest, using a shell command or a package to read the device key, edit the
// user list into making themselves an admin, or delete the settings.
#ifndef RPC_FSGUARD_H
#define RPC_FSGUARD_H

#include <stddef.h>

// Is a non-admin account logged in? False at boot and at the login prompt, when
// nobody is, so nothing that runs then is affected.
bool fs_session_restricted(void);

// May this session READ the file at `abs`? Says why, on the console, when not.
bool fs_allow_read(const char *abs);

// May it DELETE, MOVE, COPY, OVERWRITE or otherwise change what is at `abs`?
// Stricter than reading: it also refuses a directory that contains a protected
// file, and a wildcard that would match one.
bool fs_allow_change(const char *abs);

// The same two questions with no message, for the package file calls, which
// report failure through their return value.
bool fs_may_read(const char *abs);
bool fs_may_change(const char *abs);

#endif
