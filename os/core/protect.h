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

// Does this path belong to ANOTHER user's home? Changing anything there is
// refused to an ordinary account: ~/.ssh decides who may log in as that user
// (an authorized_keys an ordinary account could write was a way in as root
// over SSH), and anything else there may be a script its owner — an admin,
// perhaps — runs at boot or by hand. Reading stays open except ~/.ssh, which
// holds a private key and the list of hosts trusted.
//
// `self` may do anything in their own. With `change`, /home itself and a
// wildcard that could match a home count too. Case is ignored in "/home" and
// ".ssh"; the user name has to match exactly, as the filesystem does. A path
// too long to judge counts.
bool protect_home(const char *path, const char *self, bool change);

// Is this /os or /etc, or inside them? Everything the device runs at boot as
// the console's user is there — startup items, services, scheduled tasks,
// installed packages and their index, the trusted certificates, a staged
// update — so an ordinary account that could CHANGE any of it would be root
// at the next restart. Reading is a separate question (protect_secret).
bool protect_system(const char *path);

#endif
