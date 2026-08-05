#include "kernel.h"
#include "storage.h"
#include "persist.h"
#include "registry.h"
#include "users.h"
#include "out.h"
#include "logring.h"
#include "hardware/watchdog.h"
#include "task.h"
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

    bool force_format = false;
    if (attempts >= 3) {
        klog(LOG_ERROR, "Boot attempt %u without reaching a shell.", (unsigned)attempts);
        klog(LOG_ERROR, "Assuming the filesystem is at fault and rebuilding it.");
        force_format = true;
    }

    // Fed at every step: mounting or rebuilding a filesystem can take seconds,
    // and the scheduler is not running yet to do it automatically.
    task_watchdog_feed();
    kstep("Mounting storage...");
    if (!storage_init(true) || force_format) {
        if (!force_format) klog(LOG_ERROR, "Storage would not mount.");
        klog(LOG_WARN, "Rebuilding the filesystem. Files are lost; the device boots.");
        if (!storage_format_and_mount()) {
            klog(LOG_ERROR, "The flash itself will not take a filesystem.");
            return false;
        }
        klog(LOG_INFO, "Filesystem rebuilt.");
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
