// The session's right to touch a path. See core/protect.h for which paths.
//
// An admin may read anything but the files holding passwords and keys, and
// change anything outside /os and /etc; those need sudo as well (core/sudo.h). The system itself is not asked: boot,
// persistence and networking write the registry and the user list directly
// through storage, and never come through here. What this stops is an ordinary
// account, including the guest, using a shell command or a package to read the
// device key, edit the user list into making themselves an admin, or delete
// the settings, and an admin without sudo changing what runs at the next start.
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

// Would fs_may_change say yes if the calling task ran with sudo? True only
// for an admin, without sudo, and a path in /os or /etc (or one that reaches
// them): the cases where the answer is "use sudo" rather than "no".
bool fs_change_needs_sudo(const char *abs);

// The same for reading: true only for an admin, without sudo, and a file that
// holds a password or a key (core/protect.h, protect_credential).
bool fs_read_needs_sudo(const char *abs);

#endif
