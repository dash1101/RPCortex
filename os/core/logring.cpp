#include "logring.h"
#include "task.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_MAGIC 0x4C4F4731u   // 'LOG1'

// The whole ring in one struct so it can be placed as a unit in memory the C
// startup does not clear. On the device that is a .uninitialized_data section,
// which the RP2 linker script deliberately leaves alone through a warm reset —
// so the run-up to a crash is readable after the device comes back. On the host
// there is nothing to survive and the ring simply starts empty, which is right.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
  #define RPC_NOINIT __attribute__((section(".uninitialized_data.rpclog")))
#else
  #define RPC_NOINIT
#endif

struct LogRing {
    uint32_t magic;
    uint32_t head;        // next slot to write
    uint32_t count;       // lines held, up to LOG_LINES
    uint32_t dropped;
    uint32_t boot;        // how many times this ring has survived a restart
    LogLine  line[LOG_LINES];
};

static RPC_NOINIT LogRing g_ring;

bool log_init(void) {
    bool survived = (g_ring.magic == LOG_MAGIC &&
                     g_ring.head < LOG_LINES &&
                     g_ring.count <= LOG_LINES);
    if (!survived) {
        memset(&g_ring, 0, sizeof(g_ring));
        g_ring.magic = LOG_MAGIC;
        g_ring.boot  = 0;
    }
    g_ring.boot++;

    // A marker between runs. Timestamps are milliseconds since THIS boot, so
    // without one a dump of two runs looks like time going backwards halfway
    // down — which is precisely how it read.
    char line[LOG_LINE_MAX];
    snprintf(line, sizeof(line), "──── boot #%u ────", (unsigned)g_ring.boot);
    log_add(LOG_K_BOOT, line);
    return survived;
}

uint32_t log_boot_count(void) { return g_ring.boot; }

void log_add(LogKind kind, const char *text) {
    if (g_ring.magic != LOG_MAGIC) log_init();     // usable before log_init runs
    LogLine &l = g_ring.line[g_ring.head];
    l.at_ms = task_now_ms();
    l.kind  = (uint8_t)kind;
    snprintf(l.text, sizeof(l.text), "%s", text ? text : "");
    g_ring.head = (g_ring.head + 1) % LOG_LINES;
    if (g_ring.count < LOG_LINES) g_ring.count++;
    else                          g_ring.dropped++;
}

void log_addf(LogKind kind, const char *fmt, ...) {
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_add(kind, buf);
}

uint32_t log_count(void)   { return g_ring.count; }
uint32_t log_dropped(void) { return g_ring.dropped; }

const LogLine *log_at(uint32_t i) {
    if (i >= g_ring.count) return nullptr;
    // Oldest first. When the ring has wrapped the oldest sits at head; before
    // that it is at zero, and head equals count.
    uint32_t oldest = (g_ring.count == LOG_LINES) ? g_ring.head : 0;
    return &g_ring.line[(oldest + i) % LOG_LINES];
}

void log_clear(void) {
    g_ring.head = g_ring.count = g_ring.dropped = 0;
}
