#include "kernel.h"
#include "storage.h"
#include "persist.h"
#include "registry.h"
#include "users.h"
#include "out.h"
#include "logring.h"
#include "hardware/watchdog.h"
#include "task.h"
#include "../shell/rollback.h"
#include "registry.h"

#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include "pico/stdlib.h"
#include "pico/aon_timer.h"
#include <time.h>
#include <string.h>

extern "C" {
extern char __StackLimit;
extern char __bss_end__;
}

// Boot messages go through the same tagged, coloured output every other part of
// the OS uses, with a [POST] prefix — which is what v1's power-on self test
// looked like, and most of why a boot felt like something happening rather than
// a wall of grey text scrolling past.
void klog(LogLevel level, const char *fmt, ...) {
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    switch (level) {
        case LOG_WARN:  out_warnp("POST", "%s", msg); break;
        case LOG_ERROR: out_errp ("POST", "%s", msg); break;
        default:
            out_okp("POST", "%s", msg);
            // out_ok is not logged (routine success would flood the ring), but
            // the boot narrative IS worth keeping — it is the context for
            // whatever went wrong afterwards.
            log_addf(LOG_K_BOOT, "%s", msg);
            break;
    }
}

// Progress rather than result — v1 drew the distinction, and it is what makes a
// boot readable: [:] is "doing", [@] is "done".
static void kstep(const char *fmt, ...) {
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    out_infop("POST", "%s", msg);
}

uint32_t heap_total(void) {
    return (uint32_t)(&__StackLimit - &__bss_end__);
}

uint32_t heap_free(void) {
    struct mallinfo mi = mallinfo();
    return heap_total() - (uint32_t)mi.uordblks;
}

// Called once the shell is running. Until then every boot counts as a failure,
// which is what makes the three-strikes rebuild safe.
void kboot_succeeded(void) { watchdog_hw->scratch[4] = 0; }

// Same register, different meaning, and worth its own name. A rollback resets
// the count deliberately: the restored firmware has not failed anything, and
// letting it inherit a counter already at three would have it rebuild the
// filesystem on the first boot after being brought in to help.
void kboot_clear_strikes(void) { watchdog_hw->scratch[4] = 0; }

// For proving the recovery ladder works without breaking a device to do it.
// Sets the count to one short of the threshold, so the next boot is the one
// that triggers.
void kboot_force_strikes(uint32_t n) {
    watchdog_hw->scratch[4] = 0x52504300u | (n & 0xFFu);
}

// --- a restart that was asked for -------------------------------------------
//
// EVERY deliberate restart here goes through watchdog_reboot, because that is
// how a Cortex-M reboots itself on this part. So the hardware records "the
// watchdog did it" for `reboot`, `safeboot`, `factoryreset` and a firmware
// update alike — and the next boot announced that something had stopped
// responding, on a device that had done exactly what it was told.
//
// A scratch register survives the reset, which is the whole trick. Set on the
// way out, read and cleared on the way in. Deliberately NOT the same one the
// boot-strike counter uses: that one is cleared when the shell comes up, and a
// flag that outlives its own reason is worse than no flag.
#define REBOOT_MAGIC 0x52504352u        // 'RPCR'

void kboot_expect_reboot(void) { watchdog_hw->scratch[5] = REBOOT_MAGIC; }

// Set by the recovery ladder when repeated failures make it worth starting with
// nothing loaded. Not a registry setting on purpose: it applies to THIS boot
// only, and a flag written to flash could outlive the reason for it.
static bool g_forced_maintenance = false;
bool kboot_maintenance(void) { return g_forced_maintenance; }

static bool asked_for_it(void) {
    bool yes = watchdog_hw->scratch[5] == REBOOT_MAGIC;
    watchdog_hw->scratch[5] = 0;        // once, so the boot after it is honest
    return yes;
}

bool kboot(void) {
    // stdio + USB are already up (main brought them up so boot messages are
    // visible). Here: mount storage, and report the machine.
    // Why the device restarted. The single most useful line after a failure,
    // and it is free: the watchdog hardware remembers whether it was the one
    // that did it.
    if (watchdog_caused_reboot() && !asked_for_it()) {
        klog(LOG_WARN, "Last restart was the WATCHDOG - something stopped responding.");
        klog(LOG_WARN, "'logdump' has what was happening before it.");
    }

    task_watchdog_feed();
    kstep("Checking the machine...");
    klog(LOG_INFO, "%s  %u KB RAM, %u KB free", PICO_BOARD,
         heap_total() / 1024, heap_free() / 1024);

    // The filesystem starts at a fixed offset. If the firmware has grown past
    // it, mounting would read the tail of the firmware as a filesystem and
    // writing would destroy the firmware. Refuse, loudly, rather than either.
    if (storage_firmware_bytes() >= storage_reserve_bytes()) {
        klog(LOG_ERROR, "Firmware is %u KB but only %u KB is reserved for it.",
             storage_firmware_bytes() / 1024, storage_reserve_bytes() / 1024);
        klog(LOG_ERROR, "The filesystem would overlap the firmware. Not mounting.");
        return false;
    }

    // --- the last resort ----------------------------------------------------
    //
    // A boot counter in a watchdog scratch register, which survives a reset. It
    // goes up here and is cleared once the shell is actually running, so it only
    // grows when boots are FAILING. Three failures means the filesystem is what
    // is stopping the device, and reformatting is the only move left that does
    // not need a second computer and a nuke image.
    //
    // Losing the files is bad. Needing another machine to make the device boot
    // at all is worse, and that is the situation this exists to end.
    const uint32_t BOOT_MAGIC = 0x52504300u;
    uint32_t attempts = 0;
    if ((watchdog_hw->scratch[4] & 0xFFFFFF00u) == BOOT_MAGIC)
        attempts = watchdog_hw->scratch[4] & 0xFFu;
    attempts++;
    watchdog_hw->scratch[4] = BOOT_MAGIC | (attempts & 0xFFu);

    if (attempts >= 3)
        klog(LOG_ERROR, "Boot attempt %u without reaching a shell.", (unsigned)attempts);

    // Fed at every step: mounting or rebuilding a filesystem can take seconds,
    // and the scheduler is not running yet to do it automatically.
    task_watchdog_feed();
    kstep("Mounting storage...");
    bool rebuilt = false;
    if (!storage_init(true)) {
        klog(LOG_ERROR, "Storage would not mount.");
        klog(LOG_WARN, "Rebuilding the filesystem. Files are lost; the device boots.");
        if (!storage_format_and_mount()) {
            klog(LOG_ERROR, "The flash itself will not take a filesystem.");
            return false;
        }
        klog(LOG_INFO, "Filesystem rebuilt.");
        rebuilt = true;
    }
    klog(LOG_INFO, "Filesystem mounted  (%u KB free)", storage_free_bytes() / 1024);

    // Load the registry and accounts from flash. On a first boot both come back
    // empty; the session layer seeds root/guest and writes them.
    task_watchdog_feed();
    kstep("Reading the registry...");
    persist_load_all();
    klog(LOG_INFO, "%u setting%s, %u account%s",
         (unsigned)reg_count(), reg_count() == 1 ? "" : "s",
         (unsigned)users_count(), users_count() == 1 ? "" : "s");

    // SAY IT IF THE DEVICE HEALED ITSELF.
    //
    // A restore that happens quietly teaches nobody that the flash is going,
    // and the next time it happens there may be no shadow copy left to use.
    // This is one of the few boot lines worth interrupting somebody for.
    {
        const PersistRepair *r = persist_repair_report();
        if (r->registry_restored) {
            klog(LOG_WARN, "The settings file was unreadable; the backup was used.");
            log_add(LOG_K_WARN, "persist: registry restored from backup");
        }
        if (r->users_restored) {
            klog(LOG_WARN, "The accounts file was unreadable; the backup was used.");
            log_add(LOG_K_WARN, "persist: accounts restored from backup");
        }
        if (r->registry_lost)
            klog(LOG_WARN, "No settings could be read. Defaults are in use.");
        if (r->users_lost)
            klog(LOG_WARN, "No accounts could be read. Setup will run again.");
        if (r->registry_restored || r->users_restored)
            klog(LOG_WARN, "Both copies are rewritten now. 'fscheck' shows the state.");
    }

    // --- the recovery ladder, cheapest rung first ---------------------------
    //
    // Reached only when three boots in a row have failed to bring the OS up.
    //
    // The order matters more than either step does. If the filesystem MOUNTED
    // and the device still cannot start, the filesystem is the one part that
    // has just demonstrated it works — so wiping it is simultaneously the most
    // destructive move available and the least likely to be the right one. The
    // firmware is the thing that changed.
    //
    // So the old firmware goes back first, and the filesystem is rebuilt only
    // if there was no firmware to go back to. Restoring costs a minute;
    // rebuilding costs everything on the device, and it belongs last.
    //
    // After the registry is loaded, deliberately: a rollback writes a note for
    // the next boot to read, and writing that note before the registry has been
    // read would save an EMPTY registry over the real one — losing every
    // setting on the device in the middle of trying to rescue it.
    if (attempts >= 3 && !rebuilt) {
        task_watchdog_feed();

        // Rung one: load nothing.
        //
        // The most common reason a working device stops starting is something
        // that was added to it — a package, a service, a startup command — and
        // this is what safeboot does by hand for exactly that. Doing it
        // automatically costs nothing, changes nothing, and undoes itself on the
        // next boot. If the device comes up, the person gets a shell and can see
        // for themselves, which beats any amount of guessing done down here.
        if (attempts == 3) {
            klog(LOG_WARN, "Starting with nothing loaded: no packages, services");
            klog(LOG_WARN, "or startup items. Whichever of those it is, skip it.");
            g_forced_maintenance = true;
        } else {
            // Rung two: the firmware. Reached only when a bare boot failed too,
            // which rules out everything the device had installed on it.
            rollback_try_at_boot();        // does not return when it succeeds

            // Rung three, and last for a reason: this is the only step that
            // costs the person their files.
            klog(LOG_ERROR, "There is no saved firmware left to fall back to.");
            klog(LOG_WARN, "Rebuilding the filesystem. Files are lost; the device boots.");
            reg_clear();
            users_clear();
            if (!storage_format_and_mount()) {
                klog(LOG_ERROR, "The flash itself will not take a filesystem.");
                return false;
            }
            // A fresh filesystem has earned its own attempts. Without this the
            // count is already past the threshold, and the next boot rebuilds
            // the empty filesystem it just made.
            kboot_clear_strikes();
            klog(LOG_INFO, "Filesystem rebuilt.");
        }
    }

    // Keep the registry honest about what is running. After an update the new
    // firmware boots against the old values, and `ver` would keep reporting the
    // release it replaced — the same drift v1 had to fix.
    if (strcmp(reg_get("Settings.Version", ""), RPC_OS_VERSION) != 0)
        reg_set("Settings.Version", RPC_OS_VERSION);
    if (strcmp(reg_get("System.Codename", ""), RPC_OS_CODENAME) != 0)
        reg_set("System.Codename", RPC_OS_CODENAME);

    // Start the always-on clock so `date` works from boot. A default epoch, not
    // a real time — the user sets it, or NTP will once networking lands. Started
    // only if it is not already running across a warm reset.
    if (!aon_timer_is_running()) {
        struct tm t; memset(&t, 0, sizeof(t));
        t.tm_year = 2026 - 1900; t.tm_mon = 0; t.tm_mday = 1;
        aon_timer_start_calendar(&t);
    }
    if (strcmp(reg_get("System.Clock_Set", "false"), "true") != 0)
        out_warnp("POST", "The clock is not set. Use 'date set' or 'ntp sync'.");

    task_watchdog_feed();
    out_okp("POST", "System ready.");
    out_blank();
    return true;
}
