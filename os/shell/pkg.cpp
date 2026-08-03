#include "pkg.h"
#include "apps.h"
#include "command.h"
#include "out.h"
#include "loader.h"
#include "storage.h"
#include "pkgindex.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PKG_DIR   "/os/pkg"
#define PKG_INDEX "/os/pkg/index.cfg"
#define IDX_BUF   2048

// Path to a package's installed image: /pkg/<name>.app
static void pkg_path(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.app", PKG_DIR, name);
}

// --- the index: name,version per line --------------------------------------
// Thin filesystem wrappers over the pure pkgindex_* operations (host-tested).

static void index_add(const char *name, const char *version) {
    char *buf = (char *)malloc(IDX_BUF);
    if (!buf) return;
    uint32_t n = storage_read_file(PKG_INDEX, (uint8_t *)buf, IDX_BUF - 1);
    buf[n] = 0;
    uint32_t nn = pkgindex_add(buf, n, IDX_BUF, name, version);
    if (nn != n) storage_write_file(PKG_INDEX, (uint8_t *)buf, nn);
    free(buf);
}

static void index_remove(const char *name) {
    char *buf = (char *)malloc(IDX_BUF);
    if (!buf) return;
    uint32_t n = storage_read_file(PKG_INDEX, (uint8_t *)buf, IDX_BUF - 1);
    buf[n] = 0;
    uint32_t nn = pkgindex_remove(buf, n, IDX_BUF, name);
    storage_write_file(PKG_INDEX, (uint8_t *)buf, nn);
    free(buf);
}

static void index_walk(PkgIndexFn cb, void *ctx) {
    char *buf = (char *)malloc(IDX_BUF);
    if (!buf) return;
    uint32_t n = storage_read_file(PKG_INDEX, (uint8_t *)buf, IDX_BUF - 1);
    buf[n] = 0;
    pkgindex_walk(buf, n, cb, ctx);
    free(buf);
}

// --- operations ------------------------------------------------------------

bool pkg_install_file(const char *file, bool quiet) {
    // Validate by actually loading it — this checks the ELF, the ABI version and
    // every relocation, not just that a file exists. The header gives the name
    // and version to record.
    AppSource src; void *h = nullptr;
    if (!storage_open_source(file, &src, &h)) { out_err("No such file: %s", file); return false; }
    LoadedApp probe;
    LoadResult rc = app_load(src, &probe);
    storage_close_source(h);
    if (rc != LOAD_OK) {
        out_err("Not a valid package: %s%s%s", load_result_str(rc),
            probe.detail[0] ? " - " : "", probe.detail);
        return false;
    }
    char name[24], version[12];
    strncpy(name, probe.header.name, sizeof(name) - 1); name[sizeof(name)-1] = 0;
    strncpy(version, probe.header.version, sizeof(version) - 1); version[sizeof(version)-1] = 0;
    app_unload(&probe);      // validated; the live copy is loaded below

    // A reinstall/upgrade: drop the running copy before overwriting its file.
    apps_unload(name);

    char dst[40]; pkg_path(name, dst, sizeof(dst));
    if (strcmp(file, dst) != 0 && !storage_copy(file, dst)) {
        out_err("Could not copy package into %s.", PKG_DIR);
        return false;
    }
    index_add(name, version);
    if (!quiet) out_okp("pkg", "Installed %s %s", name, version);
    // Load it now so its commands are available without a reboot — and NOT
    // quietly. A package whose commands could not register is the difference
    // between "installed" and "usable", and staying silent about it produced
    // exactly the report this comment exists because of: pkg said Installed,
    // the command did not exist, and nothing anywhere said why.
    if (apps_launch(dst, 0, /*quiet*/false) < 0) {
        out_warnp("pkg", "'%s' is installed but did not load.", name);
        out_multi("  It will be tried again at the next boot.");
    }
    return true;
}

static bool pkg_remove(const char *name) {
    char path[40]; pkg_path(name, path, sizeof(path));
    bool known = storage_stat(path, nullptr, nullptr);
    apps_unload(name);                 // stop it and free it if resident
    storage_remove(path);
    index_remove(name);
    if (!known) { out_err("Not installed: %s", name); return false; }
    out_okp("pkg", "Removed %s", name);
    return true;
}

static void list_cb(void *, const char *name, const char *version) {
    out_multi("  %s%-16s%s %-8s", C_CYAN, name, C_RESET, version);
}
static void pkg_list(void) {
    out_info("Installed packages:");
    index_walk(list_cb, nullptr);
}

// --- boot loading + command ------------------------------------------------

bool pkg_is_disabled(const char *name);    // diag.cpp

static void load_cb(void *, const char *name, const char *) {
    // A package that crashes at boot can be switched off without removing it,
    // so the device comes up and the package is still there to look at. The
    // check is here rather than in the index so the record of what is installed
    // stays separate from the decision to load it.
    if (pkg_is_disabled(name)) {
        out_warnp("pkg", "'%s' is disabled and was not loaded.", name);
        return;
    }
    char path[40]; pkg_path(name, path, sizeof(path));
    apps_launch(path, 0, /*quiet*/true);
}

void pkg_load_installed(void) { index_walk(load_cb, nullptr); }

struct VerLookup { const char *want; char *out; unsigned cap; bool hit; };

static void ver_cb(void *ctx, const char *name, const char *version) {
    VerLookup *v = (VerLookup *)ctx;
    if (v->hit) return;
    // Case-insensitive: the index says "RPCMark" and people type "rpcmark".
    const char *a = name, *b = v->want;
    while (*a && *b) {
        char la = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char lb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (la != lb) return;
        a++; b++;
    }
    if (*a || *b) return;
    snprintf(v->out, v->cap, "%s", version);
    v->hit = true;
}

bool pkg_installed_version(const char *name, char *out, unsigned cap) {
    char *buf = (char *)malloc(IDX_BUF);
    if (!buf) return false;
    uint32_t n = storage_read_file(PKG_INDEX, (uint8_t *)buf, IDX_BUF - 1);
    buf[n] = 0;
    VerLookup v{name, out, cap, false};
    pkgindex_walk(buf, n, ver_cb, &v);
    free(buf);
    return v.hit;
}

void pkg_init(void) { storage_mkdir(PKG_DIR); }   // no-op if it already exists

int pkg_repo_command(int argc, char **argv);   // pkgrepo.cpp

static int cmd_pkg(int argc, char **argv) {
    if (argc >= 2) {
        // The repo half first: it owns install (by name), update, search, info
        // and upgrade, and returns -1 for anything it does not handle.
        int rc = pkg_repo_command(argc, argv);
        if (rc >= 0) return rc;
    }
    if (argc >= 3 && !strcmp(argv[1], "remove")) return pkg_remove(argv[2]) ? 0 : 1;
    if (argc >= 2 && !strcmp(argv[1], "list"))   { pkg_list(); return 0; }
    out_multi("Usage:");
    out_multi("  pkg update              refresh the package list");
    out_multi("  pkg search [text]       list or search available packages");
    out_multi("  pkg info <name>         details, including whether it verifies");
    out_multi("  pkg install <name>      install from the repo");
    out_multi("  pkg install <path>      install a file already on the device");
    out_multi("  pkg upgrade             update everything with a newer version");
    out_multi("  pkg remove <name>       uninstall");
    out_multi("  pkg list                what is installed");
    out_multi("  pkg repo [add|remove]   which repositories to use");
    out_multi("  pkg certs [install]     trusted roots for HTTPS");
    return 1;
}

void pkg_register(void) {
    static const Command c{"pkg", "install/remove/list packages", cmd_pkg, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
