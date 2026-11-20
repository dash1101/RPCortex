// Which files an ordinary account must not read or change.
//
// The filesystem has no permissions of its own, and a handful of files are the
// device's accounts, settings and the key that seals saved Wi-Fi passwords. A
// guest who can `cat` the key, or `edit` the user list to make themselves an
// admin, makes the roles decoration. This says which paths are off limits; the
// shell and the package file calls ask it before they touch a path, and ignore
// the answer for an admin.
//
// Paths are judged after normalisation (. and .. and // and a trailing slash
// removed, case ignored), so none of those is a way round it. Pass the path as
// the storage layer will see it: absolute, or relative to the root.
#ifndef RPC_PROTECT_H
#define RPC_PROTECT_H

// Is this exactly one of the protected files? The test for reading one.
bool protect_secret(const char *path);

// Would an operation on this path reach a protected file? True for the file
// itself, for a directory that contains one (removing or moving /os takes the
// files with it, as would copying it somewhere readable), and for a wildcard
// in the directory that holds them (/os/*.cfg). The test for deleting, moving,
// copying and overwriting.
bool protect_reaches(const char *path);

// Does this path touch ANOTHER user's ~/.ssh? Those files decide who may log
// in as that user (authorized_keys) and whom they trust (known_hosts), and
// hold their private key — so an ordinary account that could write one could
// log in as root over SSH, and one that could read one could impersonate them.
//
// `self` may touch their own. With `change`, the test is "reaches", as above:
// the home directory itself (removing or moving it takes .ssh with it), /home,
// and a wildcard where a home or a .ssh could match. Without it, only .ssh and
// what is inside it, and a wildcard that could match .ssh. Case is ignored in
// ".ssh"; the user name has to match exactly, as the filesystem does. A path
// too long to judge reaches.
bool protect_home_ssh(const char *path, const char *self, bool change);

#endif
