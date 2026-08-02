// Pure path normalisation, split out of the filesystem commands so it can be
// host-tested — '.'/'..' resolution is exactly the logic that hides bugs.
#ifndef RPC_PATH_H
#define RPC_PATH_H
#include <stddef.h>

// Resolve `in` (absolute or relative to `cwd`) into a clean absolute path in
// `out`: applies '.', pops '..', collapses '//', and never escapes above root.
void path_resolve(const char *cwd, const char *in, char *out, size_t cap);

#endif
