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
void stock_install_once(void);
void jobs_run_startup(void);
void jobs_start_services(void);
#include "banner.h"
#include "out.h"
#include "task.h"
#include "logring.h"

void net_autoconnect(void);
void task_start_core1(void);

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 300 && !stdio_usb_connected(); i++) sleep_ms(10);
    sleep_ms(150);

    // Become pid 1 before anything else can want to spawn. From here the shell
    // is a task like any other, which is what lets it be listed, and what lets
    // other things run while it waits at the prompt.
    task_init("shell");
    // Before the banner: anything the boot logs should land in the ring, and a
    // ring that survived a warm reboot holds the reason for it.
    bool prior = log_init();

    banner_print();
    if (prior) out_warnp("Boot", "The device restarted. 'logdump' shows what led up to it.");

    if (!kboot()) {
        // A usable shell is impossible (no storage). A real recovery prompt goes
        // here later; for now, say so rather than spin silently.
        klog(LOG_ERROR, "boot failed - recovery shell not yet implemented");
    }

    // Core 1 joins the SAME scheduler — one task table, two cores taking from
    // it. Started after storage and the registry are up, so nothing it picks up
    // can race against boot still finishing.
    task_start_core1();

    shell_register_builtins();
    net_autoconnect();       // rejoin a saved network, if one is set to auto
    session_boot();          // first-run setup, then login
    stock_install_once();    // first boot: write the built-in packages into /pkg
    pkg_load_installed();    // installed packages' commands go live
    // After login, so a startup command runs as the logged-in user and its
    // output is not competing with the password prompt.
    jobs_start_services();
    jobs_run_startup();

    shell_run();
    return 0;
}
