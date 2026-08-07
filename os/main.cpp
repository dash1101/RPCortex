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
#include "mpu.h"
#include "loader.h"
#include "sandbox.h"
#include "pico/flash.h"

extern "C" void fault_stack_init(void);
void net_autoconnect(void);
void task_start_core1(void);
extern "C" void usb_task_start(void);
#if CFG_TUD_MSC
void usbmsc_init(void);
#else
static inline void usbmsc_init(void) {}
#endif
void stock_install_once(void);
void update_report_boot(void);
void fs_layout_check(bool verbose);
uint32_t fs_integrity_check(bool verbose);
bool fs_accounts_check(void);
void jobs_run_startup(void);
void jobs_start_services(void);
bool safeboot_consume(char *staged, uint32_t cap);
int  shell_run_line_now(char *line);

// Everything that needs a real stack. Runs as pid 2.
static int shell_task(void *) {
    // THIS BOOT WORKED — recorded here, at the top, and not after the login
    // prompt where it used to be.
    //
    // The strike counter exists to catch a device that cannot bring its OS up:
    // three failures and the recovery ladder in kboot starts undoing things.
    // Reaching this line means the kernel booted, storage mounted, the registry
    // loaded and the scheduler is running a task — the OS is up. Everything
    // after this point is either waiting for a person to type something or
    // running somebody's package, and neither is the filesystem's fault.
    //
    // Clearing it after login instead meant a device left sitting at a login
    // prompt and power-cycled three times — which is a completely ordinary
    // thing to do to a board on a desk — rebuilt its own filesystem.
    kboot_succeeded();

    net_autoconnect();       // rejoin a saved network, if one is set to auto
    session_boot();          // first-run setup, then login

    // Before anything else prints: someone who just updated wants to know it
    // worked, and wants to know it first.
    update_report_boot();

    // A maintenance boot skips everything that could be the reason the device is
    // unusable: installed packages, services, startup items. The flag is
    // consumed here — read and cleared in one go, before anything acts on it —
    // so however this boot ends, the next one is normal. A crash between
    // reading and clearing would otherwise latch the device into maintenance
    // mode permanently, which is worse than whatever it was diagnosing.
    char staged[96];
    // Consumed unconditionally, whichever way this boot became a maintenance
    // one: the flag has to be cleared even when the ladder is what forced it,
    // or an unrelated safeboot left over from earlier would fire later.
    bool asked = safeboot_consume(staged, sizeof(staged));
    bool safe  = asked || kboot_maintenance();

    if (safe) {
        if (!asked)
            out_warnp("recovery", "Starting with nothing loaded, after repeated failures.");
        out_warnp("safeboot", "Maintenance boot: no packages, services or startup items.");
        out_multi("  Reboot normally to bring them back.");
    } else {
        stock_install_once();    // first boot only; a removed package stays removed
        pkg_load_installed();    // installed packages' commands go live
        jobs_start_services();
        jobs_run_startup();
    }

    // A staged command runs with the machine as quiet as it gets, which is the
    // whole point of staging it. Failures are the command's to report.
    if (safe && staged[0]) {
        out_infop("safeboot", "Running '%s'.", staged);
        shell_run_line_now(staged);
    }

    shell_run();             // never returns
    return 0;
}

int main(void) {
    // Before anything: mark the unused part of this stack, so how deep the boot
    // sequence really goes can be counted rather than guessed. It is 4 KB, it
    // cannot be made larger, and directly below it is core 1's stack — so an
    // overflow here corrupts the other processor rather than landing in spare
    // memory. `mpu` prints the number.
    task_main_stack_paint();
    stdio_init_all();
    // stdio_flush() inside the wait, not just sleep.
    //
    // The device stack is no longer serviced from an interrupt — see usbdev.cpp
    // for why it had to move — so something has to call tud_task() during boot
    // or the device never enumerates. Of the stdio paths that reach it, this is
    // the only one that works before a host is attached: out_chars and in_chars
    // both check stdio_usb_connected() first, which cannot become true until
    // enumeration has happened, so relying on them is a standoff neither side
    // breaks. out_flush calls tud_task() unconditionally, and stdio_flush() is
    // the way in.
    //
    // Three seconds of this is enough to enumerate; after it the printf traffic
    // through the rest of boot keeps the stack turning, and the usb task takes
    // over once the scheduler is up.
    for (int i = 0; i < 300 && !stdio_usb_connected(); i++) { stdio_flush(); sleep_ms(10); }
    sleep_ms(150);

    // bb_init FIRST. It lives in memory that survives a reset, so until it has
    // snapshotted and cleared the previous run its timestamps belong to a boot
    // that no longer exists — and anything reading them in between is reading
    // the future.
    bb_init();
    // Paint the fault handler's own stacks, so how deep a crash report actually
    // goes is a measurement and not a guess. `mpu` reports it.
    fault_stack_init();
    task_watchdog_start();
    task_preempt_start();   // force-terminate a task that stops yielding entirely

    lock_hw_init();          // before core 1 exists, so the claim cannot race
    // Memory protection before the scheduler, because task_init immediately
    // arms the guard for pid 1 and there has to be something to arm.
    mpu_platform_init();
    // Which form the loader builds veneers in. A sandboxed package cannot branch
    // into the firmware, so its calls have to be supervisor calls — and that is
    // decided here, once, rather than baked in, so the same loader serves a part
    // that cannot afford the sandbox.
    loader_set_veneer_mode(sandbox_supported() ? LOADER_VENEER_SVC
                                               : LOADER_VENEER_DIRECT);
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
    fs_integrity_check(/*verbose*/false);
    fs_accounts_check();

    // Core 0 registers as a flash lockout victim before core 1 exists.
    //
    // flash_safe_execute parks the OTHER core before touching flash, and it
    // refuses outright — immediately, not after its timeout — if that core
    // never registered. Only core 1 did, so a filesystem write issued FROM
    // core 1 had no victim to park and failed on the spot. Since an unpinned
    // task lives on core 1 about 99% of the time, that is nearly every write a
    // background task makes: `stress` reported eleven refused writes out of
    // eighteen, with the wrong bytes and failed deletes that follow from a file
    // that was never written.
    //
    // Boot-time writes always worked, because those run on core 0, which is
    // exactly why this survived so long.
    flash_safe_execute_core_init();
    task_start_core1();
    // After kboot, because the setting comes out of the registry and the
    // registry comes off the filesystem. A device told to keep its files to
    // itself has to still be doing that after a reboot.
    usbmsc_init();
    // Before the shell, so USB is being serviced by the time there is a prompt
    // to type at. It is what drives tud_task() from here on.
    usb_task_start();
    shell_register_builtins();

    if (task_spawn("shell", "(kernel)", shell_task, nullptr,
                   TASK_STACK_SHELL, AFFINITY_CORE0) < 0) {
        klog(LOG_ERROR, "could not start the shell task");
    }

    // Become the idle task. Nothing else runs here, so this stack stays shallow
    // and the 4 KB it is stuck with is plenty.
    while (true) task_yield();
}
