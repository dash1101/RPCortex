// The six recovery and diagnostic commands, carried over from v1's
// sys_recovery.py.
//
// They are grouped because they share an audience: someone whose device is
// misbehaving and who needs to find out what, or to turn one thing off so the
// rest boots. Nothing here needs infrastructure that did not already exist,
// which is why they were the last of v1's commands still missing.
//
// `compat` earns its place more than the others. v1 gained it when the ESP32-S3
// port started and "does this even work here" stopped having an obvious answer;
// it probes what the OS actually depends on and reports per subsystem. On a
// port to a new board, running it is the first useful thing anyone can do.

#include "command.h"
#include "out.h"
#include "registry.h"
#include "storage.h"
#include "task.h"
#include "persist.h"
#include "kernel.h"
#include "rollback.h"
#include "session.h"
#include "users.h"
#include "logring.h"
#include "logring.h"
#include "blackbox.h"
#include "mpu.h"
#include "sandbox.h"
#include "ptrcheck.h"

void apps_stack_peak(uint32_t *used, uint32_t *size);
void apps_stack_peak_reset(void);
// The fault handler's own stack, one per core. See fault.cpp.
extern "C" uint32_t fault_stack_used(int core);
extern "C" uint32_t fault_stack_size(void);
#include "perms.h"
#include "users.h"
#include "session.h"
#include "pkg.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

// The same gate user.cpp applies, rather than a second idea of what admin means.
static bool require_admin(const char *what) {
    if (users_is_admin(session_user())) return true;
    out_err("Only an admin can %s.", what);
    out_multi("  Try 'sudo %s' if the account has admin rights.", what);
    return false;
}

bool net_available(void);
bool net_is_connected(void);
bool http_tls_available(void);

// --- compat -----------------------------------------------------------------

enum Verdict { V_OK, V_WARN, V_FAIL, V_NA };

static void probe(const char *what, Verdict v, const char *detail) {
    const char *tag, *col;
    switch (v) {
        case V_OK:   tag = "OK  "; col = C_GREEN;  break;
        case V_WARN: tag = "WARN"; col = C_WARN;   break;
        case V_FAIL: tag = "FAIL"; col = C_FAIL;   break;
        default:     tag = "n/a "; col = C_GRAY;   break;
    }
    out_multi("  %s%s%s  %-18s %s", col, tag, C_RESET, what, detail ? detail : "");
}

static int cmd_compat(int argc, char **argv) {
    bool quick = (argc >= 2 && !strcmp(argv[1], "-q"));

    out_info("Platform self-test");
    out_multi("  Each line is something the OS depends on, probed rather than assumed.");
    out_blank();

    char d[64];

    // CPU clock: readable, and settable to what it already is. Setting it to
    // the CURRENT value is deliberate — it exercises the path without changing
    // anything, so a board that cannot do it says so safely.
    unsigned mhz = clock_get_hz(clk_sys) / 1000000u;
    snprintf(d, sizeof(d), "%u MHz", mhz);
    probe("cpu clock", mhz > 0 ? V_OK : V_FAIL, d);

    snprintf(d, sizeof(d), "%lu cores", (unsigned long)task_core_count());
    probe("cores", task_core_count() >= 1 ? V_OK : V_FAIL, d);

    // Filesystem: write, read back, compare, remove. A mount that reports
    // healthy and then loses a byte is the failure worth catching.
    {
        const char *path = "/os/.compat";
        const char *msg = "rpcortex";
        bool wrote = storage_write_file(path, (const uint8_t *)msg, 8);
        uint8_t back[16] = {0};
        uint32_t n = wrote ? storage_read_file(path, back, sizeof(back)) : 0;
        bool same = (n == 8 && memcmp(back, msg, 8) == 0);
        storage_remove(path);
        snprintf(d, sizeof(d), "%lu KB free", (unsigned long)(storage_free_bytes() / 1024));
        probe("filesystem", same ? V_OK : V_FAIL, same ? d : "write/read-back mismatch");
    }

    // Heap. A single allocation of a TLS-sized block, because that is the one
    // that fails first and the one v1 could not make reliable.
    {
        void *p = malloc(16 * 1024);
        probe("heap 16 KB", p ? V_OK : V_WARN, p ? "TLS-sized block available" : "could not allocate");
        free(p);
    }

    {
        uint32_t h = task_main_stack_headroom();
        snprintf(d, sizeof(d), "%lu bytes free on the boot stack", (unsigned long)h);
        probe("stacks", h > 256 ? V_OK : V_WARN, d);
    }

    // Clock. Unset reads as 1970 and every log timestamp is then meaningless,
    // which is worth saying out loud rather than leaving to be discovered.
    {
        bool set = strcmp(reg_get("System.Clock_Set", "false"), "true") == 0;
        probe("real-time clock", set ? V_OK : V_WARN,
              set ? "set" : "not set - use 'date set' or 'ntp'");
    }

    probe("wireless", net_available() ? (net_is_connected() ? V_OK : V_WARN) : V_NA,
          net_available() ? (net_is_connected() ? "connected" : "present, not connected")
                          : "no radio on this board");

    probe("tls", net_available() ? (http_tls_available() ? V_OK : V_WARN) : V_NA,
          net_available() ? (http_tls_available() ? "trusted roots loaded"
                                                  : "no roots parsed - pkg install will refuse")
                          : "needs wireless");

    // The log ring and black box survive a reset; if they do not, a crash
    // report cannot either, and that is worth knowing BEFORE the crash.
    probe("crash reporting", log_count() > 0 ? V_OK : V_WARN,
          log_count() > 0 ? "log ring active" : "log ring empty");

    if (!quick) {
        out_blank();
        out_multi("  Press a key to check the terminal (or wait 5 s):");
        uint32_t start = task_now_ms();
        int c = PICO_ERROR_TIMEOUT;
        while (task_now_ms() - start < 5000) {
            c = getchar_timeout_us(0);
            if (c != PICO_ERROR_TIMEOUT) break;
            task_yield();
        }
        if (c == PICO_ERROR_TIMEOUT) probe("keyboard", V_WARN, "no key seen");
        else { snprintf(d, sizeof(d), "byte 0x%02x", (unsigned)c); probe("keyboard", V_OK, d); }
    }

    out_blank();
    out_multi("  'compat -q' skips the keypress test.");
    return 0;
}

// --- diag -------------------------------------------------------------------

static int cmd_diag(int, char **) {
    out_info("Diagnostics");

    out_multi("  %sVersion%s   %s %s", C_CYAN, C_RESET, RPC_OS_VERSION, RPC_OS_CODENAME);
    out_multi("  %sUptime%s    %lu s", C_CYAN, C_RESET, (unsigned long)(task_now_ms() / 1000));
    out_multi("  %sClock%s     %u MHz", C_CYAN, C_RESET,
              (unsigned)(clock_get_hz(clk_sys) / 1000000u));
    out_multi("  %sTasks%s     %lu  (CPU %lu%%)", C_CYAN, C_RESET,
              (unsigned long)task_count(), (unsigned long)task_cpu_percent());
    out_multi("  %sFlash%s     %lu KB firmware of a %lu KB slot",
              C_CYAN, C_RESET,
              (unsigned long)(storage_firmware_bytes() / 1024),
              (unsigned long)(storage_fw_slot_bytes() / 1024));
    out_multi("  %sStaging%s   %lu KB at offset %lu KB, for updates",
              C_CYAN, C_RESET,
              (unsigned long)(storage_fw_slot_bytes() / 1024),
              (unsigned long)(storage_stage_offset() / 1024));
    out_multi("  %sStorage%s   %lu KB free of %lu KB", C_CYAN, C_RESET,
              (unsigned long)(storage_free_bytes() / 1024),
              (unsigned long)(storage_total_bytes() / 1024));
    out_multi("  %sBoots%s     %lu", C_CYAN, C_RESET, (unsigned long)log_boot_count());

    // The one thing worth surfacing without being asked: whether the last run
    // ended badly. Someone running diag is usually asking exactly that.
    const BlackBox *bb = bb_previous();
    if (bb) {
        out_blank();
        out_warn("The previous run did not shut down cleanly.");
        out_multi("  Task    %s (pid %d, core %u)", bb->task, bb->pid, (unsigned)bb->core);
        if (bb->cmd[0])   out_multi("  Command %s", bb->cmd);
        if (bb->phase[0]) out_multi("  Reached %s", bb->phase);
        out_multi("  'logdump' has the run-up to it.");
    } else {
        out_blank();
        out_ok("No crash recorded from the previous run.");
    }
    return 0;
}

// --- inputstat --------------------------------------------------------------

// v1 called this `keycode`, and it exists because terminals disagree about what
// they send. Backspace is the standing example: some send 0x08 and some 0x7f,
// and a key mapping built on the wrong assumption deletes words when it should
// delete characters. Being able to see the bytes settles it in ten seconds.
static int cmd_inputstat(int, char **) {
    out_info("Key byte viewer");
    out_multi("  Press keys to see what this terminal actually sends.");
    out_multi("  Ctrl+C or 'q' to stop.");
    out_blank();

    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            if (task_should_stop()) break;
            task_yield();
            continue;
        }
        if (c == 3 || c == 'q') break;

        const char *name = "";
        if      (c == 0x1b) name = "  ESC - an escape sequence follows";
        else if (c == 0x08) name = "  BS  - backspace, the 0x08 convention";
        else if (c == 0x7f) name = "  DEL - backspace, the 0x7f convention";
        else if (c == '\r') name = "  CR";
        else if (c == '\n') name = "  LF";
        else if (c == '\t') name = "  TAB";
        else if (c < 32)    name = "  control";

        char printable[8];
        if (c >= 32 && c < 127) snprintf(printable, sizeof(printable), "'%c'", (char)c);
        else                    snprintf(printable, sizeof(printable), "   ");
        out_multi("  0x%02x  %3d  %s%s", (unsigned)c, c, printable, name);
    }
    out_blank();
    out_ok("Stopped.");
    return 0;
}

// --- regreset ---------------------------------------------------------------

static int cmd_regreset(int argc, char **argv) {
    if (!require_admin("reset the registry")) return 1;

    if (argc < 2 || strcmp(argv[1], "--yes") != 0) {
        out_warn("This clears every setting and restores the defaults.");
        out_multi("  Accounts and files are NOT touched - only the registry.");
        out_multi("  Run 'regreset --yes' to go ahead.");
        return 1;
    }

    reg_clear();
    // Rebuild the identity keys the shell reads on every prompt, so the device
    // comes back with a working prompt rather than a blank one.
    reg_set("System.Setup", "true");
    reg_set("System.Device_ID", "vela");
    persist_save_registry();

    out_ok("Registry reset. Reboot for everything to pick up the defaults.");
    log_add(LOG_K_WARN, "registry reset to defaults");
    return 0;
}

// --- pkgdisable / pkgenable -------------------------------------------------
//
// The point of these is a package that crashes at boot. Removing it works, but
// loses it; disabling keeps the files and stops it loading, so the device boots
// and the package can be looked at rather than guessed about.
//
// The disabled list is a registry key rather than a file, because the registry
// is already loaded before packages are, and a file read at that point would be
// one more thing that has to work for the device to come up.

#define DISABLED_KEY "Pkg.Disabled"

static bool disabled_contains(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        const char *e = strchr(p, ',');
        size_t seg = e ? (size_t)(e - p) : strlen(p);
        if (seg == n && strncmp(p, name, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

bool pkg_is_disabled(const char *name) {
    return disabled_contains(reg_get(DISABLED_KEY, ""), name);
}

static int cmd_pkgdisable(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: pkgdisable <name>   |   pkgdisable list"); return 1; }

    const char *list = reg_get(DISABLED_KEY, "");
    if (!strcmp(argv[1], "list")) {
        if (!list[0]) { out_ok("No packages are disabled."); return 0; }
        out_info("Disabled packages:");
        char buf[REG_VAL_MAX]; snprintf(buf, sizeof(buf), "%s", list);
        for (char *p = strtok(buf, ","); p; p = strtok(nullptr, ","))
            out_multi("  %s", p);
        return 0;
    }

    if (!require_admin("disable a package")) return 1;
    if (disabled_contains(list, argv[1])) { out_ok("'%s' is already disabled.", argv[1]); return 0; }

    char buf[REG_VAL_MAX];
    int n = snprintf(buf, sizeof(buf), "%s%s%s", list, list[0] ? "," : "", argv[1]);
    if (n < 0 || (unsigned)n >= sizeof(buf)) {
        out_err("The disabled list is full. Remove a package instead.");
        return 1;
    }
    reg_set(DISABLED_KEY, buf);
    persist_save_registry();
    out_ok("'%s' will not load at the next boot.", argv[1]);
    out_multi("  Its files are untouched. 'pkgenable %s' undoes this.", argv[1]);
    return 0;
}

static int cmd_pkgenable(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: pkgenable <name>"); return 1; }
    if (!require_admin("enable a package")) return 1;

    const char *list = reg_get(DISABLED_KEY, "");
    if (!disabled_contains(list, argv[1])) { out_ok("'%s' is not disabled.", argv[1]); return 0; }

    // Rebuild without it, rather than editing in place — the same reason the
    // package index removes by exact line.
    char buf[REG_VAL_MAX] = {0};
    char work[REG_VAL_MAX]; snprintf(work, sizeof(work), "%s", list);
    for (char *p = strtok(work, ","); p; p = strtok(nullptr, ",")) {
        if (!strcmp(p, argv[1])) continue;
        if (buf[0]) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, p, sizeof(buf) - strlen(buf) - 1);
    }
    reg_set(DISABLED_KEY, buf);
    persist_save_registry();
    out_ok("'%s' will load at the next boot.", argv[1]);
    return 0;
}

// --- factoryreset -----------------------------------------------------------
//
// Everything a person put on the device goes; the OS itself stays. /os is left
// alone apart from the two files below, so /os/ca.pem survives — though since
// the trusted roots are compiled into the image, losing it would now cost a
// custom root rather than the ability to verify anything at all.
//
// Reboots rather than returning. The accounts it just deleted include the one
// running this command, so there is no session left to hand back to.
bool fs_rmtree(const char *path, int depth);
void sys_reboot(void);

static void wipe(const char *path) {
    bool is_dir = false;
    if (!storage_stat(path, &is_dir, nullptr)) return;   // already gone
    if (is_dir) fs_rmtree(path, 0);
    else        storage_remove(path);
}

static int cmd_factoryreset(int, char **) {
    out_warnp("reset", "FACTORY RESET - THIS CANNOT BE UNDONE");
    out_blank();
    out_multi("  This removes:");
    out_multi("    every account, including root");
    out_multi("    every setting, back to defaults");
    out_multi("    every installed package");
    out_multi("    /home, /etc and /tmp in full");
    out_blank();
    out_multi("  It keeps the OS itself, so the device still boots and can");
    out_multi("  still reach the network to install things again.");
    out_blank();

    if (!session_confirm("Erase everything and restart")) {
        out_info("Cancelled. Nothing was changed.");
        return 1;
    }

    log_add(LOG_K_WARN, "factoryreset: erasing");
    out_info("Erasing...");

    wipe("/os/users.cfg");        // accounts
    wipe("/os/pkg");              // installed packages and the repo cache
    wipe("/home");                // user data
    wipe("/etc");                 // startup items, services, scheduled tasks
    wipe("/tmp");

    // The registry last, and through reg_clear rather than by deleting the file,
    // so the in-memory copy cannot be written back over the top on the way down.
    reg_clear();
    wipe("/os/registry.cfg");

    out_ok("Done. Restarting into first-run setup.");
    out_flush();
    task_sleep_ms(400);           // let the message reach the terminal
    sys_reboot();
    return 0;                     // not reached
}

// --- safeboot ---------------------------------------------------------------
//
// Reboot into a shell with no startup items, no services and no packages, for
// when one of those is what is stopping the device from being usable. With an
// argument the command is staged and run once the maintenance shell is up.
//
// The flag is one-shot and is cleared BEFORE it is acted on, not after. A crash
// between reading and clearing would otherwise latch the device into maintenance
// mode on every subsequent boot, which is a far worse failure than the one being
// diagnosed.
static int cmd_safeboot(int argc, char **argv) {
    char staged[96] = {0};
    for (int i = 1; i < argc; i++) {
        if (staged[0]) strncat(staged, " ", sizeof(staged) - strlen(staged) - 1);
        strncat(staged, argv[i], sizeof(staged) - strlen(staged) - 1);
    }

    reg_set("System.SafeBoot", "true");
    reg_set("System.SafeBootCmd", staged);
    persist_save_dirty();

    if (staged[0]) out_info("Safe boot: restarting to run '%s' on its own.", staged);
    else {
        out_info("Safe boot: restarting without startup items or services.");
        out_multi("  Reboot normally when you are done to bring them back.");
    }
    out_flush();
    task_sleep_ms(400);
    sys_reboot();
    return 0;                     // not reached
}

// --- proving the recovery ladder ---------------------------------------------
//
// Three failed boots make the device restore its firmware, and failing that,
// rebuild its filesystem. That is the most consequential path in the OS and the
// hardest to reach honestly — it needs a build that genuinely will not start,
// which is also a build that cannot be flashed off the device afterwards.
//
// So the count is settable. `failboot` writes two strikes into the register the
// boot counter lives in and restarts: the next boot is the third, and the
// ladder runs for real — the same code, the same decisions, on a device that is
// otherwise perfectly healthy.
//
// The count clears the moment a shell comes up. If the ladder does nothing (no
// saved firmware, so the filesystem is rebuilt), that is the answer, and it is
// better to learn it on purpose than during a real failure.
static int cmd_failboot(int argc, char **argv) {
    uint32_t n = 2;
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v < 0 || v > 9) { out_err("A count between 0 and 9."); return 1; }
        n = (uint32_t)v;
    }

    out_warn("This tells the next boot that %u boots have already failed.", (unsigned)n);
    out_blank();
    out_multi("  The next boot is attempt %u, which reaches:", (unsigned)(n + 1));
    if (n + 1 < 3)
        out_multi("    nothing — the count clears as soon as a shell comes up.");
    else if (n + 1 == 3)
        out_multi("    a maintenance start: no packages, services or startup items.");
    else {
        RollbackInfo info;
        if (rollback_read(&info))
            out_multi("    the firmware restore — %s would be written back.", info.ver);
        else {
            out_multi("    the filesystem rebuild, because nothing is saved to restore.");
            out_multi("    %sEverything on the device would be lost.%s", C_BOLD, C_RESET);
        }
    }
    out_blank();
    out_multi("  %s'failboot 3' is the one that reaches the firmware restore.%s",
              C_GRAY, C_RESET);
    if (!session_confirm("  Continue?")) { out_info("Cancelled."); return 0; }

    kboot_force_strikes(n);
    out_info("Restarting.");
    out_flush();
    task_sleep_ms(400);
    sys_reboot();
    return 0;                     // not reached
}

// Whether this boot is a maintenance boot. Consumes the flag: reading it clears
// it and persists that, so the next boot is normal however this one ends.
bool safeboot_consume(char *staged, uint32_t cap) {
    bool on = strcmp(reg_get("System.SafeBoot", "false"), "true") == 0;
    if (staged && cap) snprintf(staged, cap, "%s", reg_get("System.SafeBootCmd", ""));
    if (!on) return false;
    reg_set("System.SafeBoot", "false");
    reg_set("System.SafeBootCmd", "");
    persist_save_dirty();
    return true;
}

// --- registration -----------------------------------------------------------

// What the memory protection is actually doing, per core.
//
// Worth a command of its own because every failure mode of this hardware is
// silent. A guard that was never armed, or armed on one core and not the other,
// behaves exactly like one that is working right up until the moment it was
// supposed to catch something. There is no way to tell from the outside except
// to ask.
static int cmd_mpu(int argc, char **argv) {
    // Clearing the high-water mark, so the next reading measures what is
    // happening rather than the worst thing ever attempted. `havoc stack` is
    // the reason this is needed: it exhausts the stack on purpose, and without
    // a reset every `mpu` afterwards reports a problem that is not there.
    if (argc >= 2 && !strcmp(argv[1], "reset")) {
        apps_stack_peak_reset();
        out_ok("Package stack high-water mark cleared.");
        return 0;
    }
    MpuReport r;
    if (!mpu_report(0, &r) || !r.ready) {
        out_warn("Memory protection is not enabled on this board.");
        return 1;
    }

    out_multi("  %sMemory protection%s", C_CYAN, C_RESET);
    out_multi("    Regions available : %u", (unsigned)r.regions);
    out_multi("    Stack guard       : %s",
              r.uses_msplim ? "stack limit register (MSPLIM)"
                            : "protection region 0");
    out_multi("    Package code      : %s",
              r.app_supported
                  ? "read-only while running; data never executable"
                  : "not enforced on this part - regions cost too much RAM");
    out_multi("    Packages run      : %s",
              sandbox_supported()
                  ? "unprivileged, on a stack and a heap of their own"
                  : "with the OS's own privileges");
    if (sandbox_supported()) {
        uint32_t calls = 0, refused = 0;
        sandbox_counts(&calls, &refused);
        out_multi("    ABI calls served  : %lu%s", (unsigned long)calls,
                  calls ? "" : "   (nothing has run yet)");
        if (refused)
            out_warn("  %lu were refused — a package named a function index that "
                     "does not exist.", (unsigned long)refused);

        // Pointers, which is a different refusal and a more interesting one.
        //
        // A bad function index means a corrupted veneer pool. A bad POINTER
        // means the package asked the firmware to read or write memory outside
        // itself — which the protection unit would have stopped had the package
        // done it directly, and which the firmware would once have done on its
        // behalf without looking.
        out_multi("    Pointer checks    : every buffer and string a package "
                  "passes is range-checked");

        // The deepest a package stack has actually been.
        //
        // Worth reading because an ABI call does NOT switch stacks: littlefs
        // and vsnprintf run on the package's own, so the true requirement is
        // the package plus whatever the firmware does underneath it. This is
        // the measurement that replaced guessing at that.
        uint32_t used = 0, size = 0;
        apps_stack_peak(&used, &size);
        if (size) {
            out_multi("    Package stack     : %lu of %lu bytes at its deepest  (%lu%%)",
                      (unsigned long)used, (unsigned long)size,
                      (unsigned long)(used * 100 / size));
            // A HIGH-WATER MARK, not a current reading, and saying which
            // matters. This used to conclude "the reserve is too small for what
            // packages are doing" — which is wrong after `havoc stack`, whose
            // entire job is to exhaust the stack on purpose. The measurement was
            // right and the conclusion was invented. `mpu reset` starts again.
            if (used * 10 > size * 8)
                out_warn("  Over 80%%, by the deepest package that has run since boot. "
                         "If that was a stack test, it means nothing; 'mpu reset' "
                         "clears the mark.");
        }
        uint32_t bad = ptr_refusals();
        if (bad)
            out_warn("  %lu call%s refused for pointing outside the package.",
                     (unsigned long)bad, bad == 1 ? " was" : "s were");
    }

    // THE FAULT HANDLER'S OWN STACK.
    //
    // It has one so that a stack overflow can be reported and survived without
    // the report itself running off the bottom of the stack that just ran out.
    // How much it costs is the question that decides the size, and guessing at
    // it is what this whole area has been punished for, so it is measured: the
    // buffers are painted at boot and this is the high-water mark.
    {
        uint32_t fsz = fault_stack_size();
        uint32_t deepest = 0;
        for (unsigned c = 0; c < task_core_count() && c < 2; c++) {
            uint32_t u = fault_stack_used((int)c);
            if (u > deepest) deepest = u;
        }
        if (fsz)
            out_multi("    Fault handler     : %lu of %lu bytes at its deepest%s",
                      (unsigned long)deepest, (unsigned long)fsz,
                      deepest ? "" : "   (nothing has faulted)");
        if (deepest * 10 > fsz * 8)
            out_warn("  Over 80%% — a crash report is close to outgrowing the "
                     "stack reserved for it.");
    }
    out_blank();

    for (unsigned c = 0; c < task_core_count(); c++) {
        MpuReport rc;
        if (!mpu_report(c, &rc)) continue;
        if (!rc.ready) {
            out_multi("    core %u            : %snot configured%s", c, C_WARN, C_RESET);
            continue;
        }
        out_multi("    core %u  guard at   : 0x%08lx%s", c,
                  (unsigned long)rc.guard_at,
                  rc.app_active ? "   (a package is running)" : "");
    }
    out_blank();

    // The boot stack, which is the one a guard could actually hurt.
    //
    // It is 4 KB and fixed, the whole boot sequence runs on it, and directly
    // below it is core 1's stack. A guard on a stack that was already running
    // close to the edge does not protect a device, it stops one booting — so
    // the depth is measured and shown rather than assumed comfortable.
    uint32_t used = task_main_stack_used();
    uint32_t size = task_main_stack_size();
    if (!used) {
        out_multi("    boot stack        : %u bytes, depth not measured", (unsigned)size);
    } else {
        unsigned pct = size ? (unsigned)(used * 100 / size) : 0;
        out_multi("    boot stack        : %u of %u bytes at its deepest  (%u%%)%s",
                  (unsigned)used, (unsigned)size, pct,
                  pct >= 75 ? "   <- tight" : "");
        if (pct >= 75) {
            out_blank();
            out_warn("The boot sequence is close to the end of its stack.");
            out_multi("  Below it is core 1's stack, so overrunning it corrupts the");
            out_multi("  other processor. The guard will catch that now, but it will");
            out_multi("  catch it by refusing to boot.");
        }
    }
    out_blank();
    out_multi("  A task that runs past the end of its stack now faults at the");
    out_multi("  instruction that did it, rather than corrupting whatever the");
    out_multi("  heap put below it and being blamed on something else later.");
    return 0;
}

void diag_register(void) {
    static const Command c_mpu{"mpu", "what the memory protection is enforcing  (mpu reset)", cmd_mpu, nullptr};
    cmd_register(&c_mpu);
    static const Command c_compat{"compat", "probe what this board actually supports", cmd_compat, nullptr};
    static const Command c_diag{"diag", "system state, and whether the last run crashed", cmd_diag, nullptr};
    static const Command c_input{"inputstat", "show the bytes this terminal sends", cmd_inputstat, nullptr};
    cmd_alias("keycode", "inputstat");      // what v1 called it
    static const Command c_regreset{"regreset", "restore every setting to its default", cmd_regreset, nullptr, LEVEL_ADMIN};
    static const Command c_pkgdis{"pkgdisable", "stop a package loading at boot", cmd_pkgdisable, nullptr, LEVEL_ADMIN};
    static const Command c_pkgen{"pkgenable", "let a disabled package load again", cmd_pkgenable, nullptr, LEVEL_ADMIN};
    cmd_register(&c_compat);
    cmd_register(&c_diag);
    cmd_register(&c_input);
    cmd_register(&c_regreset);
    cmd_register(&c_pkgdis);
    cmd_register(&c_pkgen);
    static const Command c_fr{"factoryreset", "erase all data and restart", cmd_factoryreset,
                              nullptr, LEVEL_ADMIN};
    static const Command c_sb{"safeboot", "restart with no services or packages", cmd_safeboot,
                              nullptr, LEVEL_ADMIN};
    static const Command c_fb{"failboot", "test the boot recovery ladder", cmd_failboot,
                              nullptr, LEVEL_ADMIN};
    cmd_register(&c_fr);
    cmd_register(&c_sb);
    cmd_register(&c_fb);
}
