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

// `~` as a shell has it: a word that is `~` alone, or starts `~/`, becomes the
// home directory (`~/notes` is `/home/root/notes`). `~word` is left alone, and
// so is anything inside quotes. A word starts at the beginning of the line or
// after a space, tab, `|`, `;`, `&` or `>`, so `cat ~/a | grep x > ~/b` works.
//
// Returns the length of the expanded line in `out`, or -1 if it does not fit in
// `cap` (and `out` is then ""). tilde_expanded_len() says how big `out` must be.
int    tilde_expand(const char *in, const char *home, char *out, size_t cap);
size_t tilde_expanded_len(const char *in, const char *home);

#endif
