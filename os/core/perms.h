// Permissions.
//
// Two levels, not a matrix. A command is either something anyone signed in may
// run, or something that changes the machine for everybody — and the second kind
// needs an admin, or `sudo`. A finer-grained scheme would be more impressive and
// would mostly produce a device where nobody remembers which of nine capability
// bits `reboot` needs.
//
// The level lives on the Command itself rather than in a list somewhere else,
// because a list is a thing that drifts: add a command, forget the list, and it
// is unprotected with nothing to notice.
//
// `sudo` raises the level for ONE command, and only for a user who could have
// run it by being an admin — it is a convenience for an admin working as
// themselves, not a way for a normal account to become root. A device this small
// has no business having a privilege-escalation path.
#ifndef RPC_PERMS_H
#define RPC_PERMS_H

#include <stdint.h>

enum CmdLevel {
    // Anyone signed in. Reading, listing, looking at your own things.
    LEVEL_USER = 0,
    // Changes the machine for everyone, or reaches outside the user's own data:
    // accounts, the registry, the radio, packages, power, the filesystem
    // outside a home directory.
    LEVEL_ADMIN = 1,
};

// Whether the named user may run something at `level` right now. `elevated` is
// true for the single command a `sudo` applies to.
bool perms_allows(const char *user, bool is_admin, CmdLevel level, bool elevated);

#endif  // RPC_PERMS_H
