// RPCortex v2 — entry point.
//
// Bring up stdio, run the boot sequence, then hand off to the shell.
//
// The shell runs as a SPAWNED TASK rather than on this stack. That is not
// tidiness: the boot stack lives in SCRATCH_Y, which is 4 KB and cannot be made
// larger, while the shell is the deepest call chain in the system — read_line,
// the pipeline, a package's app_main, and printf's 1128-byte frame somewhere
// inside it. Running commands here overflowed and hung the device, and because
// this stack is not one the scheduler allocated, it had no tripwire to catch it.
//
// So main becomes the idle task: it boots the machine, starts the shell with a
// stack sized for the job, and then does nothing but yield. Same shape as any
// system where init spawns the real work and then gets out of the way.

#include "pico/stdlib.h"
#include "kernel.h"
#include "task.h"
#include "shell.h"
#include "session.h"
#include "pkg.h"
#include "banner.h"
#include "logring.h"
#include "blackbox.h"
#include "out.h"
#include "lock.h"

void net_autoconnect(void);
void task_start_core1(void);
void stock_install_once(void);
void update_report_boot(void);
void fs_layout_check(bool verbose);
bool fs_accounts_check(void);
void jobs_run_startup(void);
void jobs_start_services(void);

// Everything that needs a real stack. Runs as pid 2.
static int shell_task(void *) {
    net_autoconnect();       // rejoin a saved network, if one is set to auto
    session_boot();          // first-run setup, then login

    // Before anything else prints: someone who just updated wants to know it
    // worked, and wants to know it first.
    update_report_boot();

    stock_install_once();    // first boot only; a removed package stays removed
    pkg_load_installed();    // installed packages' commands go live
    jobs_start_services();
    jobs_run_startup();

    // An interactive shell was reached, so this boot succeeded.
    kboot_succeeded();

    shell_run();             // never returns
    return 0;
}

int main(void) {
    stdio_init_all();
    for (int i = 0; i < 300 && !stdio_usb_connected(); i++) sleep_ms(10);
    sleep_ms(150);

    // bb_init FIRST. It lives in memory that survives a reset, so until it has
    // snapshotted and cleared the previous run its timestamps belong to a boot
    // that no longer exists — and anything reading them in between is reading
    // the future.
    bb_init();
    task_watchdog_start();
    task_preempt_start();   // force-terminate a task that stops yielding entirely

    lock_hw_init();          // before core 1 exists, so the claim cannot race
    task_init("init");
    bool prior = log_init();

    banner_print();
    if (prior) out_warnp("Boot", "The device restarted. 'logdump' shows what led up to it.");

    // What the previous run was doing when it stopped. A hang writes no log
    // lines — logging is something running code does — so this is the only
    // record of it, and it is the first thing worth seeing.
    const BlackBox *bb = bb_previous();
    if (bb && bb->task[0]) {
        out_errp("Crash", "Last run stopped while running '%s' (pid %d, core %u).",
                 bb->task, bb->pid, (unsigned)bb->core);
        if (bb->cmd[0])   out_multi("   Command   : %s", bb->cmd);
        if (bb->phase[0]) out_multi("   Reached   : %s%s%s   <- it stopped here",
                                    C_WARN, bb->phase, C_RESET);
        out_multi("   Yields    : %u before it stopped", (unsigned)bb->yields);
        if (bb->stack_size)
            out_multi("   Stack     : %u of %u bytes used%s",
                      (unsigned)bb->stack_used, (unsigned)bb->stack_size,
                      bb->stack_used * 100 / bb->stack_size >= 80 ? "  (NEARLY FULL)" : "");
        out_blank();
    }

    if (!kboot()) {
        klog(LOG_ERROR, "boot failed - recovery shell not yet implemented");
    }

    // Before anything reads a file: put back any directory that is missing, so
    // a deleted /os is a repaired boot rather than a reflash.
    fs_layout_check(/*verbose*/false);
    fs_accounts_check();

    task_start_core1();
    shell_register_builtins();

    if (task_spawn("shell", "(kernel)", shell_task, nullptr,
                   TASK_STACK_SHELL, AFFINITY_CORE0) < 0) {
        klog(LOG_ERROR, "could not start the shell task");
    }

    // Become the idle task. Nothing else runs here, so this stack stays shallow
    // and the 4 KB it is stuck with is plenty.
    while (true) task_yield();
}
