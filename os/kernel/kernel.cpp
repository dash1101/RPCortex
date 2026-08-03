#include "kernel.h"
#include "storage.h"
#include "persist.h"
#include "registry.h"
#include "users.h"
#include "out.h"
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
        default:        out_okp  ("POST", "%s", msg); break;
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

bool kboot(void) {
    // stdio + USB are already up (main brought them up so boot messages are
    // visible). Here: mount storage, and report the machine.
    kstep("Checking the machine...");
    klog(LOG_INFO, "%s  %u KB RAM, %u KB free", PICO_BOARD,
         heap_total() / 1024, heap_free() / 1024);

    kstep("Mounting storage...");
    if (!storage_init(true)) {
        klog(LOG_ERROR, "Storage would not mount.");
        return false;
    }
    klog(LOG_INFO, "Filesystem mounted  (%u KB free)", storage_free_bytes() / 1024);

    // Load the registry and accounts from flash. On a first boot both come back
    // empty; the session layer seeds root/guest and writes them.
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

    out_okp("POST", "System ready.");
    out_blank();
    return true;
}
