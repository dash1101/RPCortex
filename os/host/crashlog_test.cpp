// The crash report: what lands in /os/crash.log after a bad shutdown.
//
// This text is the only record of a crash that nobody was watching, so the
// failure that matters is a report that is quietly wrong or quietly cut short:
// a header that names the wrong task, a log that is out of order, a buffer
// overrun on a full ring, or a truncation that stops mid-line.

#include "crashlog.h"
#include "logring.h"
#include "blackbox.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}
static bool has(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }
// Where `a` appears relative to `b`: a report must list the log oldest-first.
static bool before(const char *hay, const char *a, const char *b) {
    const char *pa = strstr(hay, a), *pb = strstr(hay, b);
    return pa && pb && pa < pb;
}

static BlackBox sample() {
    BlackBox b;
    memset(&b, 0, sizeof(b));
    b.pid = 7;
    b.core = 1;
    snprintf(b.task, sizeof(b.task), "wifi-join");
    snprintf(b.cmd, sizeof(b.cmd), "wifi connect home");
    snprintf(b.phase, sizeof(b.phase), "tls handshake");
    b.yields = 1234;
    b.stack_used = 900;
    b.stack_size = 3072;
    return b;
}

int main(void) {
    log_init();
    log_clear();
    log_add(LOG_K_INFO, "alpha");
    log_add(LOG_K_WARN, "bravo");
    log_add(LOG_K_ERR,  "charlie");

    static char out[CRASHLOG_MAX];

    // --- the header names what happened --------------------------------------
    BlackBox b = sample();
    size_t n = crashlog_format(out, sizeof(out), &b);
    ck(n == strlen(out), "the returned length is the string length");
    ck(has(out, "Task      : wifi-join (pid 7, core 1)"), "names the task, pid and core");
    ck(has(out, "Command   : wifi connect home"), "names the command line");
    ck(has(out, "Reached   : tls handshake"), "names the last checkpoint");
    ck(has(out, "Yields    : 1234"), "reports the yield count");
    ck(has(out, "Stack     : 900 of 3072"), "reports stack use");
    ck(!has(out, "Deadlock") && !has(out, "Recovery") && !has(out, "Longest"),
       "leaves out the lines that do not apply");

    // --- the log follows, oldest first, with the right tags ------------------
    ck(before(out, "alpha", "bravo") && before(out, "bravo", "charlie"), "log lines are oldest first");
    ck(has(out, "[:] alpha") && has(out, "[?] bravo") && has(out, "[!] charlie"),
       "each line carries its kind tag");
    ck(before(out, "Previous run", "Log, oldest first"), "header comes before the log");

    // --- the optional lines appear when their fields are set -----------------
    b.hw_twice = 1; b.hw_twice_core = 1; b.hw_twice_pc = 0x1000abcd;
    b.max_stall_ms = 6200; b.max_stall_pc = 0x20001234; b.stall_crit = 1;
    b.stuck = BB_STUCK_PACKAGE;
    crashlog_format(out, sizeof(out), &b);
    ck(has(out, "taken twice on core 1, from 1000abcd"), "reports the deadlock and where it came from");
    ck(has(out, "6200 ms held without yielding at 20001234 (holding a lock)"), "reports the longest stall");
    ck(has(out, "stopped inside a package call"), "says a package call could not be recovered");
    b.stuck = BB_STUCK_TASK;
    crashlog_format(out, sizeof(out), &b);
    ck(has(out, "the task could not be ended"), "says a task could not be ended");

    // --- no black box is still a usable report -------------------------------
    crashlog_format(out, sizeof(out), nullptr);
    ck(has(out, "no black-box record"), "a missing record is stated, not crashed on");
    ck(has(out, "alpha"), "the log is still there without it");

    // --- a ring that has wrapped says so -------------------------------------
    log_clear();
    char buf[32];
    for (int i = 0; i < (int)LOG_LINES + 5; i++) {
        snprintf(buf, sizeof(buf), "line%d", i);
        log_add(LOG_K_INFO, buf);
    }
    crashlog_format(out, sizeof(out), &b);
    ck(has(out, "5 earlier lines were dropped"), "dropped lines are counted in the report");
    ck(!has(out, "line0\n") && has(out, "line5"), "the oldest surviving line is the right one");
    ck(has(out, "line52"), "the newest line made it in");

    // --- a full, widest-case ring fits the advertised bound ------------------
    log_clear();
    char wide[LOG_LINE_MAX];
    memset(wide, 'w', sizeof(wide) - 1); wide[sizeof(wide) - 1] = 0;
    for (int i = 0; i < (int)LOG_LINES; i++) log_add(LOG_K_ERR, wide);
    b = sample(); b.hw_twice = 1; b.max_stall_ms = 9; b.stuck = BB_STUCK_PACKAGE;
    size_t full = crashlog_format(out, sizeof(out), &b);
    ck(!has(out, "cut short"), "CRASHLOG_MAX is enough for a full ring at its widest");
    ck(full < sizeof(out), "and it stays inside the buffer");

    // --- a small buffer is cut on a line boundary, never overrun -------------
    // Guard bytes after the buffer prove nothing was written past `cap`.
    for (size_t cap : {1u, 2u, 16u, 100u, 300u, 700u, 1500u}) {
        static char arena[CRASHLOG_MAX + 64];
        memset(arena, 0x5a, sizeof(arena));
        size_t got = crashlog_format(arena, cap, &b);
        bool guarded = true;
        for (size_t i = cap; i < cap + 16; i++) if ((unsigned char)arena[i] != 0x5a) guarded = false;
        ck(guarded, "nothing is written past the buffer");
        ck(got < cap, "the length is always inside the buffer");
        ck(arena[got] == 0, "always NUL-terminated");
        if (cap >= 700 && got > 0)
            ck(arena[got - 1] == '\n', "a cut report ends at a line boundary");
    }
    size_t small = crashlog_format(out, 600, &b);
    ck(has(out, "cut short"), "a report that did not fit says so");
    ck(small < 600, "and the notice itself fits");
    ck(crashlog_format(nullptr, 10, &b) == 0, "a null buffer is refused");
    ck(crashlog_format(out, 0, &b) == 0, "a zero-size buffer is refused");

    printf("  crashlog: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
