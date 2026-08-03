// Installing packages from the repo — the half of the package manager v1 had
// and v2 did not.
//
// `pkg install <file>` already existed, which is a different and much smaller
// thing: it assumed the package was somehow already on the device. This is the
// part that makes `pkg install greet` mean something.
//
// The order of operations is the whole design, and it is chosen so that no
// failure leaves a half-installed package behind:
//
//   1. resolve the name in the cached index
//   2. check it can run here at all (arch, ABI) BEFORE spending a download
//   3. stream it to a temp file, never into RAM
//   4. verify the SHA-256 against what the index published
//   5. only then install
//
// Step 4 is the one that matters most. TLS proves who served the file; the hash
// proves the file is the one the index describes, which is a different claim and
// the one that protects against a repo serving a corrupt or swapped artifact.
// Without it "it downloaded successfully" only means bytes arrived.

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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool http_transport_get(HttpTransport *t);
bool http_tls_available(void);
const char *http_tls_why(void);
void http_tls_reset(void);
bool stock_install_cacerts(bool force);
void http_last_detail(char *out, unsigned cap);
bool net_is_connected(void);

// The repo list lives in the registry, one URL per comma-separated entry, so
// it survives a reboot and is editable without a filesystem format. A device
// with none configured falls back to the official one rather than being unable
// to install anything.
#define REPO_KEY      "Pkg.Repos"
#define REPO_DEFAULT  "https://raw.githubusercontent.com/dash1101/RPCortex-repo/main/repo-v2/index.json"
#define CACHE      "/os/pkg/repo.json"
#define TMP_PKG    "/os/pkg/.download"
#define INDEX_MAX  16384

// The architecture this image runs.
#if PICO_RP2040
#define MY_ARCH "armv6m"
#else
#define MY_ARCH "armv8m"
#endif

// One architecture, or several separated by commas.
//
// A package author chooses: build for one board and say so, or build fat and
// serve every target from one artifact. Both are legitimate, so the field is a
// LIST — "armv6m" and "armv6m,xtensa,riscv32" are equally valid, and the second
// is what a fat package publishes.
//
// Compatibility within ARM is one-way, and getting it backwards would be
// expensive in both directions. ARMv8-M Baseline is a superset of ARMv6-M, so
// an armv6m package runs correctly on an RP2350 — refusing it would stop one
// binary serving both boards for no reason. The reverse is a real fault: armv8m
// code on an RP2040 executes instructions the chip does not have, so it is
// refused up front rather than allowed to fault later with nothing pointing at
// the cause.
static bool arch_one_runs_here(const char *a, size_t n) {
    if (n == strlen(MY_ARCH) && !strncmp(a, MY_ARCH, n)) return true;
#if !PICO_RP2040
    if (n == 6 && !strncmp(a, "armv6m", 6)) return true;    // baseline runs on v8-M
#endif
    return false;
}

static bool arch_runs_here(const char *arch) {
    if (!arch || !arch[0]) return true;              // unstated: allow, as v1 did
    for (const char *p = arch; *p; ) {
        while (*p == ' ') p++;
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n && p[n - 1] == ' ') n--;            // tolerate "a, b"
        if (n && arch_one_runs_here(p, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

// --- helpers ----------------------------------------------------------------

// Each repo's cache is its own file, so one unreachable repo does not discard
// what the others published.
static void cache_path(int index, char *out, size_t cap) {
    if (index == 0) snprintf(out, cap, "%s", CACHE);
    else            snprintf(out, cap, "/os/pkg/repo%d.json", index);
}

static char *load_cache(uint32_t *len) {
    char *buf = (char *)malloc(INDEX_MAX);
    if (!buf) return nullptr;
    uint32_t n = storage_read_file(CACHE, (uint8_t *)buf, INDEX_MAX - 1);
    if (n == 0) { free(buf); return nullptr; }
    buf[n] = 0;
    *len = n;
    return buf;
}

static bool require_index(char **buf, uint32_t *len) {
    *buf = load_cache(len);
    if (*buf) return true;
    out_err("No package list. Run 'pkg update' first.");
    return false;
}

// Search every cached repo in order, so a package from the second one is found
// as readily as one from the first. Earlier repos win a name collision, which
// makes the list an explicit priority order rather than an accident.
static bool find_in_any_repo(const char *name, RepoEntry *out) {
    for (int i = 0; i < 8; i++) {
        char path[40]; cache_path(i, path, sizeof(path));
        char *buf = (char *)malloc(INDEX_MAX);
        if (!buf) return false;
        uint32_t n = storage_read_file(path, (uint8_t *)buf, INDEX_MAX - 1);
        if (n == 0) { free(buf); continue; }
        buf[n] = 0;
        bool hit = repo_find(buf, n, name, out);
        free(buf);
        if (hit) return true;
    }
    return false;
}

// Walk every cached repo's entries.
static void walk_all_repos(RepoEntryFn cb, void *ctx) {
    for (int i = 0; i < 8; i++) {
        char path[40]; cache_path(i, path, sizeof(path));
        char *buf = (char *)malloc(INDEX_MAX);
        if (!buf) return;
        uint32_t n = storage_read_file(path, (uint8_t *)buf, INDEX_MAX - 1);
        if (n) { buf[n] = 0; repo_walk(buf, n, cb, ctx); }
        free(buf);
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
    if (!http_transport_get(&t)) {
        out_err("No network. Connect with 'wifi connect <ssid>' first.");
        return false;
    }
    if (!strncmp(url, "https://", 8) && !http_tls_available()) {
        out_err("HTTPS cannot be verified: %s", http_tls_why());
        out_multi("  Try 'pkg certs install' — the bundle ships in the firmware.");
        out_multi("  Unverified HTTPS is not offered: anything on the path could");
        out_multi("  serve a package and it would be installed.");
        return false;
    }

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
        out_err("%s%s%s", fetch_error_str(r.error), r.detail[0] ? " - " : "", r.detail);
        // A transport failure on its own says almost nothing. What the
        // connection actually managed is what points at the cause.
        if (r.error == FETCH_ERR_RECV || r.error == FETCH_ERR_CONNECT ||
            r.error == FETCH_ERR_SEND) {
            char d[96];
            http_last_detail(d, sizeof(d));
            out_multi("  %s", d);
        }
        return false;
    }
    uint8_t digest[32];
    sha256_final(&d.sha, digest);
    hex_of(digest, hex_out);
    *bytes = r.bytes;
    return true;
}

// --- subcommands ------------------------------------------------------------

// Walk the configured repos, calling cb for each URL.
typedef bool (*RepoUrlFn)(void *ctx, const char *url, int index);

static int repo_each(RepoUrlFn cb, void *ctx) {
    const char *list = reg_get(REPO_KEY, REPO_DEFAULT);
    if (!list[0]) list = REPO_DEFAULT;
    int n = 0;
    char url[REPO_URL_MAX];
    for (const char *p = list; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && p[len - 1] == ' ') len--;
        if (len && len < sizeof(url)) {
            memcpy(url, p, len); url[len] = 0;
            if (cb && !cb(ctx, url, n)) return n + 1;
            n++;
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
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

static int do_update(void) {
    out_info("Fetching package lists...");
    UpdCtx u{0, 0, 0};
    repo_each(update_one, &u);

    if (u.ok == 0) { out_err("No package list could be fetched."); return 1; }
    out_ok("%lu package%s from %d repo%s.%s", (unsigned long)u.total,
           u.total == 1 ? "" : "s", u.ok, u.ok == 1 ? "" : "s",
           u.failed ? "  Some repos failed." : "");
    return 0;
}

// --- repo management --------------------------------------------------------

static bool print_repo(void *, const char *url, int index) {
    out_multi("  %d. %s", index + 1, url);
    return true;
}

static int do_repo(int argc, char **argv) {
    if (argc < 3 || !strcmp(argv[2], "list")) {
        out_info("Package repositories:");
        if (repo_each(print_repo, nullptr) == 0) out_multi("  (none)");
        out_multi("  'pkg repo add <url>' / 'pkg repo remove <url|number>'");
        return 0;
    }

    const char *list = reg_get(REPO_KEY, REPO_DEFAULT);
    char buf[REG_VAL_MAX];

    if (!strcmp(argv[2], "add")) {
        if (argc < 4) { out_multi("Usage: pkg repo add <url>"); return 1; }
        if (strncmp(argv[3], "http://", 7) && strncmp(argv[3], "https://", 8)) {
            out_err("A repo URL must begin with http:// or https://");
            return 1;
        }
        if (strstr(list, argv[3])) { out_ok("Already listed."); return 0; }
        int n = snprintf(buf, sizeof(buf), "%s%s%s", list, list[0] ? "," : "", argv[3]);
        if (n < 0 || (unsigned)n >= sizeof(buf)) {
            out_err("The repo list is full. Remove one first.");
            return 1;
        }
        reg_set(REPO_KEY, buf);
        persist_save_registry();
        out_ok("Added. Run 'pkg update' to fetch it.");
        return 0;
    }

    if (!strcmp(argv[2], "remove")) {
        if (argc < 4) { out_multi("Usage: pkg repo remove <url|number>"); return 1; }
        // A number, because typing a full URL to delete it is unkind.
        int want = atoi(argv[3]);
        char work[REG_VAL_MAX]; snprintf(work, sizeof(work), "%s", list);
        buf[0] = 0;
        int i = 0, removed = 0;
        for (char *p = strtok(work, ","); p; p = strtok(nullptr, ",")) {
            i++;
            if ((want > 0 && i == want) || !strcmp(p, argv[3])) { removed++; continue; }
            if (buf[0]) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, p, sizeof(buf) - strlen(buf) - 1);
        }
        if (!removed) { out_err("No such repo."); return 1; }
        reg_set(REPO_KEY, buf);
        persist_save_registry();
        out_ok("Removed.");
        return 0;
    }

    out_multi("Usage: pkg repo list | add <url> | remove <url|number>");
    return 1;
}

struct ListCtx { const char *q; int shown; };

static bool list_cb(void *ctx, const RepoEntry *e) {
    ListCtx *c = (ListCtx *)ctx;
    if (c->q) {
        // Match either the name or the description, case-insensitively.
        char ln[REPO_NAME_MAX + REPO_DESC_MAX];
        snprintf(ln, sizeof(ln), "%s %s", e->name, e->desc);
        for (char *p = ln; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        char q[64];
        snprintf(q, sizeof(q), "%s", c->q);
        for (char *p = q; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        if (!strstr(ln, q)) return true;
    }
    out_multi("  %s%-14s%s %-8s %s", C_CYAN, e->name, C_RESET, e->ver, e->desc);
    c->shown++;
    return true;
}

static int do_search(const char *query) {
    char *buf; uint32_t len;
    if (!require_index(&buf, &len)) return 1;
    free(buf);                       // only needed to prove a list exists
    ListCtx c{query, 0};
    out_info(query ? "Matching packages:" : "Available packages:");
    walk_all_repos(list_cb, &c);
    if (!c.shown) { out_warn("Nothing matched '%s'.", query ? query : ""); return 1; }
    return 0;
}

static int do_info(const char *name) {
    char *buf; uint32_t len;
    if (!require_index(&buf, &len)) return 1;
    free(buf);
    RepoEntry e;
    if (!find_in_any_repo(name, &e)) { out_err("No package called '%s'.", name); return 1; }

    out_info("%s %s", e.name, e.ver);
    if (e.desc[0])   out_multi("  %s", e.desc);
    if (e.author[0]) out_multi("  Author   %s", e.author);
    if (e.arch[0])   out_multi("  Arch     %s%s", e.arch,
                               arch_runs_here(e.arch) ? "" : "  (not this board)");
    if (e.abi[0])    out_multi("  ABI      %s", e.abi);
    if (e.size)      out_multi("  Size     %lu bytes", (unsigned long)e.size);
    out_multi("  Verified %s", e.sha256[0] ? "yes, SHA-256" : "NO - the index publishes no hash");

    char installed[24];
    if (pkg_installed_version(e.name, installed, sizeof(installed))) {
        int c = repo_version_cmp(e.ver, installed);
        if (c > 0) out_multi("  Installed %s - an update is available", installed);
        else       out_multi("  Installed %s - up to date", installed);
    }
    return 0;
}

static int do_install(const char *name) {
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
    char hex[65]; uint64_t got = 0;
    if (!download(e.url, TMP_PKG, hex, &got)) return 1;

    // The hash is what makes this an install rather than a hope. TLS proved who
    // served the bytes; this proves they are the bytes the index describes.
    if (e.sha256[0]) {
        if (strcmp(hex, e.sha256) != 0) {
            storage_remove(TMP_PKG);
            out_err("Checksum mismatch - the download does not match the index.");
            out_multi("  expected  %s", e.sha256);
            out_multi("  received  %s", hex);
            out_multi("  Nothing was installed. Try 'pkg update' and again.");
            return 1;
        }
        out_ok("Checksum verified.");
    } else {
        // Loud, because silently installing an unverified package is exactly
        // the habit this design exists to avoid.
        out_warn("The index publishes no checksum for this package.");
        out_multi("  Installed unverified - only the transport was authenticated.");
    }

    if (!pkg_install_file(TMP_PKG, /*quiet*/true)) {
        storage_remove(TMP_PKG);
        out_err("The package downloaded but would not install.");
        return 1;
    }
    storage_remove(TMP_PKG);
    out_ok("Installed %s %s (%lu bytes).", e.name, e.ver, (unsigned long)got);
    return 0;
}

struct UpCtx { int found; int done; int failed; };

static bool upgrade_cb(void *ctx, const RepoEntry *e) {
    UpCtx *u = (UpCtx *)ctx;
    char have[24];
    if (!pkg_installed_version(e->name, have, sizeof(have))) return true;   // not installed
    if (repo_version_cmp(e->ver, have) <= 0) return true;                   // current
    u->found++;
    out_info("%s %s -> %s", e->name, have, e->ver);
    if (do_install(e->name) == 0) u->done++; else u->failed++;
    return !intr_check();
}

static int do_upgrade(void) {
    char *buf; uint32_t len;
    if (!require_index(&buf, &len)) return 1;
    free(buf);
    UpCtx u{0, 0, 0};
    walk_all_repos(upgrade_cb, &u);
    if (!u.found) { out_ok("Everything is up to date."); return 0; }
    out_ok("%d upgraded, %d failed.", u.done, u.failed);
    return u.failed ? 1 : 0;
}

// --- entry ------------------------------------------------------------------

// `pkg certs` — what the trust store looks like, and a way to put it back.
//
// The bundle ships in flash and is written on a first boot, but a write can
// fail, a file can be truncated, and neither announces itself: the only symptom
// is `pkg update` refusing to verify. This turns that into a readable answer
// and a one-word repair, without a reflash.
static int do_certs(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[2], "install")) {
        if (!stock_install_cacerts(/*force*/true)) return 1;
        http_tls_reset();          // so the repair takes effect immediately
        out_ok("Trusted roots reinstalled.");
    }

    bool is_dir = false; uint32_t size = 0;
    bool present = storage_stat("/os/ca.pem", &is_dir, &size);
    out_info("Trusted roots");
    out_multi("  Built in   always available; the image carries its own copy");
    out_multi("  Override   /os/ca.pem  %s", present ? "present" : "not set");
    if (present) out_multi("  Size       %lu bytes", (unsigned long)size);

    if (http_tls_available()) {
        out_ok("HTTPS can be verified.");
    } else {
        out_warn("HTTPS cannot be verified: %s", http_tls_why());
        out_multi("  'pkg certs install' writes the bundle that ships in firmware.");
    }
    return 0;
}

int pkg_repo_command(int argc, char **argv) {
    const char *sub = argv[1];
    if (!strcmp(sub, "certs")) return do_certs(argc, argv);
    if (!strcmp(sub, "repo"))  return do_repo(argc, argv);
    if (!strcmp(sub, "update"))  return do_update();
    if (!strcmp(sub, "search"))  return do_search(argc >= 3 ? argv[2] : nullptr);
    if (!strcmp(sub, "upgrade")) return do_upgrade();
    if (!strcmp(sub, "info")) {
        if (argc < 3) { out_multi("Usage: pkg info <name>"); return 1; }
        return do_info(argv[2]);
    }
    if (!strcmp(sub, "install")) {
        if (argc < 3) { out_multi("Usage: pkg install <name|file> [name...]"); return 1; }

        // Every name given, not just the first. Installing one package at a
        // time is the common case and taking several is free; silently doing
        // one of three is not something anyone would expect.
        //
        // A path installs from the filesystem; a bare name comes from the repo.
        // Distinguishing on the shape of the argument keeps one verb doing one
        // job from the user's side.
        int failed = 0, done = 0;
        for (int i = 2; i < argc; i++) {
            int rc = strchr(argv[i], '/') ? (pkg_install_file(argv[i], false) ? 0 : 1)
                                          : do_install(argv[i]);
            if (rc) failed++;
            else    done++;
            // Keep going after a failure. One name being wrong is no reason to
            // skip the rest, and the summary below says what happened.
        }
        if (argc > 3)
            out_infop("pkg", "%d installed, %d failed.", done, failed);
        return failed ? 1 : 0;
    }
    return -1;      // not ours
}
