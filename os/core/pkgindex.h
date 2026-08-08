// The package index as pure buffer operations, split from pkg.cpp so the fiddly
// string logic — dedup on add, exact-line removal — host-tests without a
// filesystem. pkg.cpp reads the index file into a buffer, calls these, writes it
// back, exactly as the registry and users cores are wrapped.
//
// Format: one "name,version\n" line per package.
#ifndef RPC_PKGINDEX_H
#define RPC_PKGINDEX_H

#include <stdint.h>

// Record "name,version". One line per package: an existing entry has its
// version REWRITTEN rather than being left alone. Returns the new length. buf
// must be NUL-terminated within cap.
//
// This used to return early when the name was already listed, on the reasoning
// that the index must not grow a second line for one package. True, but it made
// every upgrade invisible: the new binary installed, and the index went on
// naming the version it replaced. `pkg list` reported 0.95.0 on a device that
// had been running 0.97.0 since the last reboot, and `pkg upgrade` offered the
// same upgrade for ever, because the version it compares against was frozen at
// whatever was installed first.
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
