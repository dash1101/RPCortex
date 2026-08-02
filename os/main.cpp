// RPCortex v2 — entry point.
//
// Bring up stdio, run the boot sequence, register the built-in commands, and
// hand off to the shell. Deliberately tiny: the kernel boots, the shell drives,
// and everything else is a command or a loaded app.

#include "pico/stdlib.h"
#include "kernel.h"
#include "shell.h"
#include "session.h"
#include "pkg.h"
#include "banner.h"

void net_autoconnect(void);

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 300 && !stdio_usb_connected(); i++) sleep_ms(10);
    sleep_ms(150);

    banner_print();

    if (!kboot()) {
        // A usable shell is impossible (no storage). A real recovery prompt goes
        // here later; for now, say so rather than spin silently.
        klog(LOG_ERROR, "boot failed - recovery shell not yet implemented");
    }

    shell_register_builtins();
    net_autoconnect();       // rejoin a saved network, if one is set to auto
    session_boot();          // first-run setup, then login
    pkg_load_installed();    // installed packages' commands go live
    shell_run();
    return 0;
}
