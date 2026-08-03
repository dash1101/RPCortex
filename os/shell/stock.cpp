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
#include "registry.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern "C" const unsigned char stock_stress_data[];
extern "C" const unsigned int  stock_stress_len;
extern "C" const unsigned char stock_bench_data[];
extern "C" const unsigned int  stock_bench_len;

struct StockPkg { const char *name; const unsigned char *data; const unsigned int *len; };

static const StockPkg kStock[] = {
    {"stress", stock_stress_data, &stock_stress_len},
    {"bench",  stock_bench_data,  &stock_bench_len},
};
#define N_STOCK (sizeof(kStock) / sizeof(kStock[0]))

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

void stock_install_once(void) {
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
