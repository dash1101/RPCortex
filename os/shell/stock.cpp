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

extern "C" const unsigned char stock_stress_data[];
extern "C" const unsigned int  stock_stress_len;

struct StockPkg { const char *name; const unsigned char *data; const unsigned int *len; };

static const StockPkg kStock[] = {
    {"stress", stock_stress_data, &stock_stress_len},
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

// Called at boot. Does nothing after the first time, so a package the user
// removed stays removed — the opposite would make `pkg remove` useless.
void stock_install_once(void) {
    if (strcmp(reg_get("System.Stock", "false"), "true") == 0) return;
    for (unsigned i = 0; i < N_STOCK; i++) stock_place(kStock[i], /*quiet*/true);
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
