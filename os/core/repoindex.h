// Reading the package repo's index.json, without a JSON library.
//
// The index has one fixed shape and this only ever reads five fields from it, so
// a general parser would be a lot of code to solve a problem nobody has. This is
// a scanner: it finds `"key": "value"` pairs inside object boundaries and hands
// back one entry at a time.
//
// Deliberately tolerant about layout — whitespace, key order, unknown extra
// fields, minified or pretty-printed all read the same — and deliberately strict
// about size: every field lands in a fixed buffer, and a value too long to hold
// is a rejected entry rather than a truncated one. A truncated URL is a request
// to somewhere real and wrong.
//
// Pure, so the actual index.json from the repo is checked in as a fixture and
// parsed by the host suite. The format changing is then a failing test rather
// than a device that quietly finds no packages.
#ifndef RPC_REPOINDEX_H
#define RPC_REPOINDEX_H

#include <stdint.h>
#include <stdbool.h>

#define REPO_NAME_MAX  40
#define REPO_VER_MAX   16
#define REPO_DESC_MAX  96
#define REPO_URL_MAX   200
#define REPO_HASH_MAX  65      // 64 hex digits and a terminator

struct RepoEntry {
    char name[REPO_NAME_MAX];
    char ver[REPO_VER_MAX];
    char desc[REPO_DESC_MAX];
    char author[REPO_NAME_MAX];
    char url[REPO_URL_MAX];

    // v2 index fields. Absent from the v1 index, so each is optional rather
    // than required — but when present they are the difference between
    // installing a package and installing whatever happened to arrive.
    char sha256[REPO_HASH_MAX];   // "" when the index publishes none
    char abi[8];                  // loader ABI the package was built against
    char arch[12];                // armv6m / armv8m
    uint32_t size;                // 0 when unstated
};

// Walk the entries in `json`. `cb` returning false stops the walk early — which
// is how a lookup for one name avoids parsing the rest. Returns the number of
// entries visited.
typedef bool (*RepoEntryFn)(void *ctx, const RepoEntry *e);
uint32_t repo_walk(const char *json, uint32_t len, RepoEntryFn cb, void *ctx);

// Find one package by name, case-insensitively — the repo says "RPCMark" and
// people type "rpcmark". Returns false when absent.
bool repo_find(const char *json, uint32_t len, const char *name, RepoEntry *out);

// Compare two dotted versions numerically: "1.10.0" is newer than "1.9.0",
// which a string compare gets backwards. Returns >0 when a is newer, 0 when
// equal, <0 when older. Missing components count as zero, so "2.1" == "2.1.0".
int repo_version_cmp(const char *a, const char *b);

#endif  // RPC_REPOINDEX_H
