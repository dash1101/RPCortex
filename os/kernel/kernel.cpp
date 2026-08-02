#include "kernel.h"
#include "storage.h"

#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include "pico/stdlib.h"

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
    return true;
}
