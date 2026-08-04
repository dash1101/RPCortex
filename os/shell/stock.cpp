// Packages that ship with the firmware.
//
// Their bytes are baked into flash and written into /pkg the first time the OS
// boots. They are ordinary packages from that point: `pkg remove stress` deletes
// it and it stays deleted, which is the whole difference between shipping a
// package and shipping a built-in command that calls itself one.
//
// `pkg stock` puts them back, since the copy in flash never goes anywhere.

#include "pkg.h"
#include "command.h"
#include "out.h"
#include "storage.h"
#include "logring.h"
#include "registry.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct StockPkg { const char *name; const unsigned char *data; const unsigned int *len; };

// The development test suite, baked in rather than published.
//
// bench, probe and stress measure what a host test cannot — real cores, real
// interrupt jitter, real timing — so a device wants them before it has a
// working network, which is when they are usually needed. They are of no use to
// somebody who just wants the OS, and publishing each one costs a build and an
// index entry every time it changes.
//
// Built with RPC_DEV_PACKAGES off, none of this exists and the table is empty.
// That is the whole switch: shipping the OS should not mean picking them back
// out of the build by hand.
#if RPC_DEV_PACKAGES
extern "C" const unsigned char stock_stress_data[];
extern "C" const unsigned int  stock_stress_len;
extern "C" const unsigned char stock_bench_data[];
extern "C" const unsigned int  stock_bench_len;
extern "C" const unsigned char stock_probe_data[];
extern "C" const unsigned int  stock_probe_len;

static const StockPkg kStock[] = {
    {"stress", stock_stress_data, &stock_stress_len},
    {"bench",  stock_bench_data,  &stock_bench_len},
    {"probe",  stock_probe_data,  &stock_probe_len},
};
#else
// An empty array is not valid C++, and a table of one null entry would be
// walked. A count of zero with no array is the honest shape.
static const StockPkg *const kStock = nullptr;
#endif
#if RPC_DEV_PACKAGES
  #define N_STOCK (sizeof(kStock) / sizeof(kStock[0]))
#else
  #define N_STOCK 0u
#endif

// Write one out and install it. Returns false if it could not be written.
static bool stock_place(const StockPkg &p, bool quiet) {
    char path[48];
    snprintf(path, sizeof(path), "/%s.app", p.name);
    if (!storage_write_file(path, p.data, *p.len)) {
        out_err("Could not write %s.", path);
        return false;
    }
    bool ok = pkg_install_file(path, quiet);
    storage_remove(path);          // pkg_install copied it into /pkg
    return ok;
}

// Called at boot.
//
// Two rules that pull against each other, and both matter:
//
//   * a package the user REMOVED stays removed. Reinstalling it every boot would
//     make `pkg remove` a no-op.
//   * a package that CHANGED with the firmware is replaced. Without this a
//     drop-in firmware update left the old copy installed forever — which is
//     exactly what happened: several firmware builds' worth of debugging
//     checkpoints were added to the self test and none of them ever ran,
//     because the device kept loading the binary from the first boot.
//
// The two are told apart by recording each package's length when it is
// installed. A different length means the firmware carries a new build; a
// missing file with a matching record means the user deleted it.
static bool stock_should_place(const StockPkg &p, bool *is_update) {
    char key[REG_KEY_MAX];
    snprintf(key, sizeof(key), "Stock.%s_len", p.name);
    const char *seen = reg_get(key, nullptr);

    if (!seen || !seen[0]) { *is_update = false; return true; }   // never installed

    uint32_t recorded = (uint32_t)strtoul(seen, nullptr, 10);
    if (recorded == *p.len) { *is_update = false; return false; } // same build

    *is_update = true;
    return true;                                                  // firmware moved on
}

static void stock_record(const StockPkg &p) {
    char key[REG_KEY_MAX], val[16];
    snprintf(key, sizeof(key), "Stock.%s_len", p.name);
    snprintf(val, sizeof(val), "%u", (unsigned)*p.len);
    reg_set(key, val);
}

// The trusted roots. Not a package — a file the TLS layer reads — but it ships
// the same way and for the same reason: it must exist on a first boot without
// anyone having to fetch it, and yet remain replaceable afterwards.
//
// Written only when absent, so a hand-updated bundle survives a firmware update
// that would otherwise put the shipped one back.
extern "C" const unsigned char stock_cacerts_data[];
extern "C" const unsigned int  stock_cacerts_len;
extern "C" const unsigned char stock_index_data[];
extern "C" const unsigned int  stock_index_len;

// The package catalogue this image shipped with, written out on a first boot
// so `pkg available` answers before the device has ever been online. v1 shipped
// its list the same way.
//
// Only when absent, never over the top: an index fetched with `pkg update` is
// newer than the one compiled in, and putting the shipped copy back after a
// firmware update would silently undo it.
bool stock_install_index(void) {
    bool is_dir = false; uint32_t size = 0;
    if (storage_stat("/os/pkg/index.cfg", &is_dir, &size) && size > 0) return true;
    if (!storage_write_file("/os/pkg/index.cfg", stock_index_data, stock_index_len)) {
        log_add(LOG_K_WARN, "pkg: shipped index could not be written");
        return false;
    }
    log_addf(LOG_K_OK, "pkg: installed the shipped catalogue (%u bytes)",
             (unsigned)stock_index_len);
    return true;
}

bool stock_install_cacerts(bool force) {
    bool is_dir = false; uint32_t size = 0;
    // Only when absent or empty, so a hand-updated bundle survives a firmware
    // update that would otherwise put the shipped one back.
    if (!force && storage_stat("/os/ca.pem", &is_dir, &size) && size > 0) return true;

    if (!storage_write_file("/os/ca.pem", stock_cacerts_data, stock_cacerts_len)) {
        // Saying nothing here is how a device ends up unable to install
        // packages with no clue why: pkg update reports "no trusted
        // certificates" and nothing ever explains that the write failed.
        out_errp("certs", "Could not write /os/ca.pem — HTTPS will not verify.");
        log_add(LOG_K_ERR, "certs: /os/ca.pem could not be written");
        return false;
    }
    log_addf(LOG_K_OK, "certs: installed %u bytes of trusted roots",
             (unsigned)stock_cacerts_len);
    return true;
}

static void install_cacerts(void) { stock_install_cacerts(false); }

void stock_install_once(void) {
    install_cacerts();
    stock_install_index();
    for (unsigned i = 0; i < N_STOCK; i++) {
        bool is_update = false;
        if (!stock_should_place(kStock[i], &is_update)) continue;

        // A user-removed package that has NOT changed is left alone above. One
        // that has changed is reinstalled even if it was removed — the firmware
        // shipping a new version is a clear enough intent, and `pkg remove`
        // still works afterwards.
        if (stock_place(kStock[i], /*quiet*/!is_update)) {
            stock_record(kStock[i]);
            if (is_update)
                out_okp("pkg", "Updated built-in package '%s' to match the firmware.",
                        kStock[i].name);
        }
    }
    reg_set("System.Stock", "true");
}

static int cmd_stock(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        out_info("Packages built into this firmware:");
        for (unsigned i = 0; i < N_STOCK; i++)
            out_multi("  %s%-12s%s %u B", C_CYAN, kStock[i].name, C_RESET, *kStock[i].len);
        return 0;
    }
    // Reinstall them all, or one by name.
    int done = 0;
    for (unsigned i = 0; i < N_STOCK; i++) {
        if (argc >= 2 && strcmp(argv[1], kStock[i].name) != 0) continue;
        if (stock_place(kStock[i], /*quiet*/false)) done++;
    }
    if (!done) {
        out_warn("Nothing to restore. 'pkg stock list' shows what is built in.");
        return 1;
    }
    return 0;
}

void stock_register(void) {
    static const Command c{"stock", "restore a built-in package", cmd_stock, nullptr};
    cmd_register(&c);
}
