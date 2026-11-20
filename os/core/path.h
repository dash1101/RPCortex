// Pure path normalisation, split out of the filesystem commands so it can be
// host-tested — '.'/'..' resolution is exactly the logic that hides bugs.
#ifndef RPC_PATH_H
#define RPC_PATH_H
#include <stddef.h>

// Resolve `in` (absolute or relative to `cwd`) into a clean absolute path in
// `out`: applies '.', pops '..', collapses '//', and never escapes above root.
//
// Returns false, with `out` set to "", when the result does not fit in `cap`.
// The caller must check: it used to drop any name of 40 characters or more and
// carry on, so `rm -r /home/root/<a long name>` resolved to /home/root. An
// empty path is the filesystem root to littlefs, so a caller that ignores the
// result is not safe either; the storage calls that destroy refuse "" as well.
bool path_resolve(const char *cwd, const char *in, char *out, size_t cap);

#endif
