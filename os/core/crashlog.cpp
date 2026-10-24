#include "crashlog.h"
#include "kernel.h"
#include "logring.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *log_kind_tag(uint8_t k) {
    switch (k) {
        case LOG_K_ERR:  return "!";
        case LOG_K_WARN: return "?";
        case LOG_K_BOOT: return ".";
        case LOG_K_OK:   return "@";
        default:         return ":";
    }
}

namespace {

// Appends whole lines only. A line that does not fit is dropped along with
// everything after it, so a report cut short ends cleanly rather than halfway
// through a word.
struct Out {
    char  *buf;
    size_t cap;
    size_t len;
    bool   full;

    void line(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (full) return;
        char tmp[192];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if ((size_t)n >= sizeof(tmp)) n = (int)sizeof(tmp) - 1;
        if (len + (size_t)n + 1 >= cap) { full = true; return; }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
        buf[len++] = '\n';
        buf[len] = 0;
    }
};

}  // namespace

size_t crashlog_format(char *out, size_t cap, const BlackBox *bb) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    Out o{out, cap, 0, false};

    o.line("RPCortex %s \"%s\" build %s", RPC_OS_VERSION, RPC_OS_CODENAME, RPC_OS_BUILD);
    o.line("Crash report, written at boot #%u. Times below are milliseconds since each",
           (unsigned)log_boot_count());
    o.line("boot began, so they restart at every \"boot #\" marker.");
    o.line("%s", "");

    if (bb) {
        o.line("Previous run");
        o.line("  Task      : %s (pid %d, core %u)", bb->task, bb->pid, (unsigned)bb->core);
        if (bb->cmd[0])   o.line("  Command   : %s", bb->cmd);
        if (bb->phase[0]) o.line("  Reached   : %s", bb->phase);
        o.line("  Yields    : %u before it stopped", (unsigned)bb->yields);
        if (bb->hw_twice)
            o.line("  Deadlock  : hardware lock taken twice on core %u, from %08x",
                   (unsigned)bb->hw_twice_core, (unsigned)bb->hw_twice_pc);
        if (bb->max_stall_ms)
            o.line("  Longest   : %u ms held without yielding at %08x%s",
                   (unsigned)bb->max_stall_ms, (unsigned)bb->max_stall_pc,
                   bb->stall_crit ? " (holding a lock)" : "");
        if (bb->stack_size)
            o.line("  Stack     : %u of %u bytes used", (unsigned)bb->stack_used,
                   (unsigned)bb->stack_size);
        if (bb->stuck == BB_STUCK_PACKAGE)
            o.line("  Recovery  : none; stopped inside a package call");
        else if (bb->stuck == BB_STUCK_TASK)
            o.line("  Recovery  : none; the task could not be ended");
    } else {
        o.line("Previous run: no black-box record.");
    }
    o.line("%s", "");

    o.line("Log, oldest first");
    if (log_dropped())
        o.line("  (%u earlier line%s were dropped; the ring holds %u)",
               (unsigned)log_dropped(), log_dropped() == 1 ? "" : "s", (unsigned)LOG_LINES);
    for (uint32_t i = 0; i < log_count(); i++) {
        const LogLine *l = log_at(i);
        if (!l) break;
        o.line("%4u.%03u  [%s] %s", (unsigned)(l->at_ms / 1000), (unsigned)(l->at_ms % 1000),
               log_kind_tag(l->kind), l->text);
    }
    if (o.full) {
        // The notice has to fit too, so give back whole lines until it does. When
        // the buffer is too small for it even with nothing else in, leave it out:
        // an unterminated overrun is worse than a report that does not say so.
        const char *msg = "(report cut short: buffer full)\n";
        size_t m = strlen(msg);
        while (o.len > 0 && o.len + m + 1 > cap) {
            do { o.len--; } while (o.len > 0 && o.buf[o.len - 1] != '\n');
        }
        if (o.len + m + 1 <= cap) {
            memcpy(o.buf + o.len, msg, m);
            o.len += m;
        }
        o.buf[o.len] = 0;
    }
    return o.len;
}
