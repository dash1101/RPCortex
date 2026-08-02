// The package index as pure buffer operations, split from pkg.cpp so the fiddly
// string logic — dedup on add, exact-line removal — host-tests without a
// filesystem. pkg.cpp reads the index file into a buffer, calls these, writes it
// back, exactly as the registry and users cores are wrapped.
//
// Format: one "name,version\n" line per package.
#ifndef RPC_PKGINDEX_H
#define RPC_PKGINDEX_H

#include <stdint.h>

// Append "name,version\n" unless `name` is already listed. Returns the new
// length. buf must be NUL-terminated within cap.
uint32_t pkgindex_add(char *buf, uint32_t len, uint32_t cap,
                      const char *name, const char *version);

// Remove the line whose name field equals `name` exactly (not a prefix).
// Returns the new length.
uint32_t pkgindex_remove(char *buf, uint32_t len, uint32_t cap, const char *name);

// True if `name` is listed.
bool pkgindex_has(const char *buf, uint32_t len, const char *name);

typedef void (*PkgIndexFn)(void *ctx, const char *name, const char *version);
void pkgindex_walk(char *buf, uint32_t len, PkgIndexFn cb, void *ctx);

#endif  // RPC_PKGINDEX_H
