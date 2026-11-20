// The package repository, between its two halves: pkgrepo.cpp (the catalogue,
// which works offline) and pkgfetch.cpp (everything that downloads, which an
// image without Wi-Fi leaves out and net_off.cpp stands in for).
#ifndef RPC_PKGREPO_H
#define RPC_PKGREPO_H

#include <stddef.h>
#include <stdint.h>
#include "repoindex.h"

// Largest repository index read into memory at once.
#define INDEX_MAX  16384

// The architecture this image runs.
#if PICO_RP2040
#define MY_ARCH "armv6m"
#else
#define MY_ARCH "armv8m"
#endif

// --- pkgrepo.cpp ------------------------------------------------------------

// Whether a package built for `arch` (one name or a comma list) runs here.
bool arch_runs_here(const char *arch);
// The cache file for the index-th repository.
void cache_path(int index, char *out, size_t cap);
// Load the first repository's cached index, or say there is none.
bool require_index(char **buf, uint32_t *len);
// Search every cached repository in order; earlier ones win a name collision.
bool find_in_any_repo(const char *name, RepoEntry *out);
void walk_all_repos(RepoEntryFn cb, void *ctx);
// Each configured repository URL, in order.
typedef bool (*RepoUrlFn)(void *ctx, const char *url, int index);
int  repo_each(RepoUrlFn cb, void *ctx);

// --- pkgfetch.cpp, or net_off.cpp without Wi-Fi -------------------------------

int pkg_fetch_update(void);                       // pkg update
int pkg_fetch_install(const char *name);          // pkg install <name>
int pkg_fetch_upgrade(void);                      // pkg upgrade
int pkg_fetch_certs(int argc, char **argv);       // pkg certs [install]

#endif  // RPC_PKGREPO_H
