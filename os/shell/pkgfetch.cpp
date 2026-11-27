// Fetching packages from the repository: `pkg update`, `pkg install <name>`,
// `pkg upgrade` and `pkg certs`.
//
// The half of the package manager that needs a network, split out of
// pkgrepo.cpp, which keeps what works offline (the catalogue: search, info and
// the repository list) and installs a package from a file. In every image: with
// Wi-Fi the download goes over it, and without Wi-Fi through the bridge, when rpc
// or the browser tool on the console offers it (httproute.h, LINK.md). `pkg
// certs` is TLS and lives with it in httptransport.cpp. The design notes on why
// an install goes in the order it does are at the top of pkgrepo.cpp.

#include "command.h"
#include "out.h"
#include "httpfetch.h"
#include "repoindex.h"
#include "storage.h"
#include "sha256.h"
#include "interrupt.h"
#include "pkg.h"
#include "registry.h"
#include "persist.h"
#include "pkgrepo.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "httproute.h"

// Each install downloads into its own file, named for its task: two installs
// running at once (two SSH sessions, a session and the console) used to share
// one, and each overwrote, then deleted, the other's download (PAR-74).
#define TMP_PKG_DIR    "/os/pkg"
#define TMP_PKG_PREFIX ".download."

static void tmp_pkg_path(char *out, uint32_t cap) {
    snprintf(out, cap, TMP_PKG_DIR "/" TMP_PKG_PREFIX "%d", task_self());
}

// A download left by a task that is gone (faulted, killed, the board lost
// power mid-way) is nobody's: removed before the next install starts. One
// whose task is still running is another install in progress, and kept.
struct StaleCtx { char names[4][32]; int n; };
static void sweep_stale_downloads(void) {
    StaleCtx c{};
    storage_walk(TMP_PKG_DIR, [](void *ctx, const char *name, bool d, uint32_t) {
        StaleCtx *x = (StaleCtx *)ctx;
        const size_t pl = sizeof(TMP_PKG_PREFIX) - 1;
        if (d || strncmp(name, TMP_PKG_PREFIX, pl) != 0 || x->n >= 4) return;
        int pid = atoi(name + pl);
        const TaskInfo *t = pid > 0 ? task_find(pid) : nullptr;
        if (t && t->state != TASK_DONE) return;
        snprintf(x->names[x->n++], sizeof(x->names[0]), "%s", name);
    }, &c);
    // The old shared name, from firmware before this one.
    storage_remove(TMP_PKG_DIR "/.download");
    for (int i = 0; i < c.n; i++) {
        char p[64];
        snprintf(p, sizeof(p), TMP_PKG_DIR "/%s", c.names[i]);
        storage_remove(p);
    }
}

struct DlSink { void *fh; Sha256Ctx sha; };

static int dl_sink(void *ctx, const uint8_t *data, uint32_t len) {
    DlSink *d = (DlSink *)ctx;
    sha256_update(&d->sha, data, len);      // hashed as it streams; never buffered
    return storage_sink_write(d->fh, data, len) ? 0 : -1;
}

static int poll_intr(void *) { return intr_check() ? 1 : 0; }

static void progress(void *, uint64_t got, uint64_t total) {
    static uint64_t last;
    if (got < last) last = 0;
    if (got - last < 4096 && got != total) return;
    last = got;
    out_progress("Downloading", got, total);
}

static void hex_of(const uint8_t *digest, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i * 2] = h[digest[i] >> 4]; out[i * 2 + 1] = h[digest[i] & 15]; }
    out[64] = 0;
}

// Download `url` to `dest`, returning the hex digest of what arrived.
static bool download(const char *url, const char *dest, char *hex_out, uint64_t *bytes) {
    HttpTransport t;
    HttpVia via = http_route(&t);
    if (!via) { http_no_route(); return false; }
    if (!http_https_ready(url, via)) return false;

    DlSink d{nullptr, {}};
    sha256_init(&d.sha);
    d.fh = storage_open_sink(dest);
    if (!d.fh) { out_err("Could not write to %s.", dest); return false; }

    FetchOpts o{};
    o.poll = poll_intr;
    o.progress = progress;
    uint32_t room = storage_free_bytes();
    o.max_bytes = room > 8192 ? room - 8192 : 1;

    FetchResult r;
    bool good = http_fetch(&t, url, dl_sink, &d, &o, &r);
    if (!storage_close_sink(d.fh) && good) { good = false; r.error = FETCH_ERR_SINK; }
    out_progress_done();

    if (!good) {
        storage_remove(dest);
        // A transport failure on its own says almost nothing. What the
        // connection actually managed (or the bridge's host said) is what
        // points at the cause.
        http_report_failure(&r);
        return false;
    }
    uint8_t digest[32];
    sha256_final(&d.sha, digest);
    hex_of(digest, hex_out);
    *bytes = r.bytes;
    return true;
}

struct UpdCtx { int ok; int failed; uint32_t total; };

static bool update_one(void *ctx, const char *url, int index) {
    UpdCtx *u = (UpdCtx *)ctx;
    char path[40]; cache_path(index, path, sizeof(path));

    out_info("%s", url);
    char hex[65]; uint64_t n = 0;
    if (!download(url, path, hex, &n)) { u->failed++; return true; }

    uint32_t len = 0;
    char *buf = (char *)malloc(INDEX_MAX);
    uint32_t got = buf ? storage_read_file(path, (uint8_t *)buf, INDEX_MAX - 1) : 0;
    if (buf) buf[got] = 0;
    len = got;
    uint32_t count = buf ? repo_walk(buf, len, nullptr, nullptr) : 0;
    free(buf);

    if (count == 0) {
        // Keeping an unreadable list would make every later command fail with a
        // confusing "not found" instead of pointing here.
        storage_remove(path);
        out_warn("  nothing readable at this repo");
        u->failed++;
        return true;
    }
    out_ok("  %lu package%s", (unsigned long)count, count == 1 ? "" : "s");
    u->ok++;
    u->total += count;
    return true;
}

int pkg_fetch_update(void) {
    out_info("Fetching package lists...");
    UpdCtx u{0, 0, 0};
    repo_each(update_one, &u);

    if (u.ok == 0) { out_err("No package list could be fetched."); return 1; }
    out_ok("%lu package%s from %d repo%s.%s", (unsigned long)u.total,
           u.total == 1 ? "" : "s", u.ok, u.ok == 1 ? "" : "s",
           u.failed ? "  Some repos failed." : "");
    return 0;
}

// One attempt: find the package in the cached list, download it, check it
// against the list, install it. INSTALL_STALE when the download does not match
// the list and this is not the last try, so the caller can refresh the list.
enum { INSTALL_STALE = 2 };

static int fetch_install_once(const char *name, bool last_try) {
    char *buf; uint32_t len;
    if (!require_index(&buf, &len)) return 1;
    free(buf);
    RepoEntry e;
    if (!find_in_any_repo(name, &e)) {
        out_err("No package called '%s'.", name);
        out_multi("  'pkg search' lists what is available.");
        return 1;
    }

    // Refuse what cannot run here BEFORE spending a download on it. A package
    // for the other architecture loads happily and then executes instructions
    // this chip does not have.
    if (!arch_runs_here(e.arch)) {
        out_err("%s is built for %s; this board is %s.", e.name, e.arch, MY_ARCH);
        out_multi("  A package can list several architectures, or target one deliberately.");
        return 1;
    }

    out_info("Installing %s %s", e.name, e.ver);
    sweep_stale_downloads();
    char TMP_PKG[40];
    tmp_pkg_path(TMP_PKG, sizeof(TMP_PKG));
    char hex[65]; uint64_t got = 0;
    if (!download(e.url, TMP_PKG, hex, &got)) { storage_remove(TMP_PKG); return 1; }

    // The hash is what makes this an install rather than a hope. TLS proved who
    // served the bytes; this proves they are the bytes the index describes.
    if (e.sha256[0]) {
        if (strcmp(hex, e.sha256) != 0) {
            storage_remove(TMP_PKG);
            if (!last_try) {
                // Most likely the list here is older than the repository: a
                // package was updated since it was fetched, or this is the
                // catalogue the image shipped with. Said once, then fixed.
                out_warn("The package list here is out of date for %s; fetching the current one.", e.name);
                return INSTALL_STALE;
            }
            out_err("Checksum mismatch - the download does not match the index.");
            out_multi("  expected  %s", e.sha256);
            out_multi("  received  %s", hex);
            out_multi("  Nothing was installed. The repository may be changing; try again shortly.");
            return 1;
        }
        out_ok("Checksum verified.");
    } else {
        // Loud, because silently installing an unverified package is exactly
        // the habit this design exists to avoid.
        out_warn("The index publishes no checksum for this package.");
        out_multi("  Installed unverified - only the transport was authenticated.");
    }

    if (!pkg_install_file(TMP_PKG, /*quiet*/true, /*consume*/true)) {
        storage_remove(TMP_PKG);
        out_err("The package downloaded but would not install.");
        return 1;
    }
    storage_remove(TMP_PKG);
    out_ok("Installed %s %s (%lu bytes).", e.name, e.ver, (unsigned long)got);
    return 0;
}

int pkg_fetch_install(const char *name) {
    // No list yet (a new board, or one that has never run `pkg update`): fetch
    // it first rather than send the person away to type another command.
    {
        char path[40]; cache_path(0, path, sizeof(path));
        bool is_dir = false; uint32_t size = 0;
        if (!storage_stat(path, &is_dir, &size) || is_dir || !size) {
            out_info("No package list yet; fetching it first.");
            if (pkg_fetch_update() != 0) return 1;
        }
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        int rc = fetch_install_once(name, /*last_try*/ attempt == 1);
        if (rc != INSTALL_STALE) return rc;
        // A list that disagrees with the repository: fetch it and try once
        // more. A fresh board used to stop here with "Checksum mismatch", from
        // the catalogue its image shipped with, about a package that had been
        // updated since.
        if (pkg_fetch_update() != 0) return 1;
    }
    return 1;
}

struct UpCtx { int found; int done; int failed; };

// Whether the installed copy is byte-for-byte what the index describes.
//
// Version numbers are the normal signal, and they miss one real case: a package
// REBUILT without its version changing. That is not a corner case during
// development, it is what happens every time — and the symptom is a repo
// serving new bytes while `pkg upgrade` reports everything up to date, with no
// way to tell from the device that the two disagree.
//
// Hashing the installed file costs one read of a few KB and only happens for
// packages whose version already matched, so it is off the common path.
static bool installed_matches(const char *name, const char *want_hex) {
    if (!want_hex || !want_hex[0]) return true;      // nothing published to compare

    char path[48];
    snprintf(path, sizeof(path), "/os/pkg/%s.app", name);

    // Whole file in one read. A package is a few KB against a heap with
    // hundreds free, and the alternative is a chunked reader this is the only
    // caller of.
    #define PKG_HASH_MAX 65536
    uint8_t *buf = (uint8_t *)malloc(PKG_HASH_MAX);
    if (!buf) return true;                            // cannot check: do not nag

    uint32_t n = storage_read_file(path, buf, PKG_HASH_MAX);
    if (n == 0) { free(buf); return true; }           // no file to compare

    Sha256Ctx c; sha256_init(&c);
    sha256_update(&c, buf, n);
    free(buf);

    uint8_t dg[SHA256_DIGEST_LEN];
    sha256_final(&c, dg);
    char hex[65];
    for (int i = 0; i < SHA256_DIGEST_LEN; i++) {
        static const char *H = "0123456789abcdef";
        hex[i * 2]     = H[dg[i] >> 4];
        hex[i * 2 + 1] = H[dg[i] & 15];
    }
    hex[64] = 0;
    return strcmp(hex, want_hex) == 0;
}

static bool upgrade_cb(void *ctx, const RepoEntry *e) {
    UpCtx *u = (UpCtx *)ctx;
    char have[24];
    if (!pkg_installed_version(e->name, have, sizeof(have))) return true;   // not installed

    bool newer = repo_version_cmp(e->ver, have) > 0;
    if (!newer) {
        // Same version. Rebuilt, or genuinely identical?
        if (installed_matches(e->name, e->sha256)) return true;
        u->found++;
        out_info("%s %s - rebuilt, the installed copy differs", e->name, e->ver);
        if (pkg_fetch_install(e->name) == 0) u->done++; else u->failed++;
        return !intr_check();
    }

    u->found++;
    out_info("%s %s -> %s", e->name, have, e->ver);
    if (pkg_fetch_install(e->name) == 0) u->done++; else u->failed++;
    return !intr_check();
}

int pkg_fetch_upgrade(void) {
    char *buf; uint32_t len;
    if (!require_index(&buf, &len)) return 1;
    free(buf);
    UpCtx u{0, 0, 0};
    walk_all_repos(upgrade_cb, &u);
    if (!u.found) { out_ok("Everything is up to date."); return 0; }
    out_ok("%d upgraded, %d failed.", u.done, u.failed);
    return u.failed ? 1 : 0;
}
