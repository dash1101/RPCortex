#include "kernel.h"
#include "storage.h"
#include "persist.h"
#include "registry.h"
#include "users.h"

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

static const char *level_tag(LogLevel l) {
    switch (l) {
        case LOG_WARN:  return "[!]";
        case LOG_ERROR: return "[x]";
        default:        return "[.]";
    }
}

void klog(LogLevel level, const char *fmt, ...) {
    printf("%s ", level_tag(level));
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
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
    klog(LOG_INFO, "RPCortex v2 kernel");
    klog(LOG_INFO, "board %s  heap %u/%u KB free",
         PICO_BOARD, heap_free() / 1024, heap_total() / 1024);

    if (!storage_init(true)) {
        klog(LOG_ERROR, "storage mount failed");
        return false;
    }
    klog(LOG_INFO, "storage mounted  %u KB free", storage_free_bytes() / 1024);

    // Load the registry and accounts from flash. On a first boot both come back
    // empty; the session layer seeds root/guest and writes them.
    persist_load_all();
    klog(LOG_INFO, "registry %u keys, %u accounts",
         (unsigned)reg_count(), (unsigned)users_count());

    // Start the always-on clock so `date` works from boot. A default epoch, not
    // a real time — the user sets it, or NTP will once networking lands. Started
    // only if it is not already running across a warm reset.
    if (!aon_timer_is_running()) {
        struct tm t; memset(&t, 0, sizeof(t));
        t.tm_year = 2026 - 1900; t.tm_mon = 0; t.tm_mday = 1;
        aon_timer_start_calendar(&t);
    }
    return true;
}
