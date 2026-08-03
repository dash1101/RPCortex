// The log ring: wrapping, ordering, and the drop count.
//
// A ring that reports its oldest entry wrongly after wrapping is worse than no
// log — it presents a partial history as a complete one, which is how you end up
// chasing the wrong cause.

#include "logring.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++; if (!c) { fails++; printf("  FAIL: %s\n", m); }
}
static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (got && strcmp(got, want) == 0) return;
    fails++;
    printf("  FAIL: %s  (got '%s', want '%s')\n", what, got ? got : "(null)", want);
}

int main(void) {
    log_init();
    log_clear();
    ck(log_count() == 0, "a fresh ring is empty");
    ck(log_at(0) == nullptr, "reading past the end gives nothing");
    ck(log_dropped() == 0, "nothing dropped yet");

    log_add(LOG_K_INFO, "first");
    log_add(LOG_K_WARN, "second");
    ck(log_count() == 2, "two lines held");
    eq(log_at(0)->text, "first",  "oldest is first");
    eq(log_at(1)->text, "second", "then the next");
    ck(log_at(1)->kind == LOG_K_WARN, "the kind is kept");
    ck(log_at(2) == nullptr, "and nothing beyond");

    log_addf(LOG_K_ERR, "n=%d s=%s", 42, "x");
    eq(log_at(2)->text, "n=42 s=x", "formatted lines work");

    // Fill exactly to capacity: nothing should be dropped yet.
    log_clear();
    char buf[32];
    for (uint32_t i = 0; i < LOG_LINES; i++) {
        snprintf(buf, sizeof(buf), "line%u", (unsigned)i);
        log_add(LOG_K_INFO, buf);
    }
    ck(log_count() == LOG_LINES, "the ring fills to capacity");
    ck(log_dropped() == 0, "a full ring has dropped nothing");
    eq(log_at(0)->text, "line0", "oldest is still the first written");
    snprintf(buf, sizeof(buf), "line%u", (unsigned)(LOG_LINES - 1));
    eq(log_at(LOG_LINES - 1)->text, buf, "newest is the last written");

    // One past capacity: the oldest goes, and the count of losses is honest.
    log_add(LOG_K_INFO, "overflow1");
    ck(log_count() == LOG_LINES, "the count stays at capacity");
    ck(log_dropped() == 1, "one line recorded as dropped");
    eq(log_at(0)->text, "line1", "the oldest line was the one dropped");
    eq(log_at(LOG_LINES - 1)->text, "overflow1", "the newest is at the end");

    // Wrap several times over; ordering must still be oldest-first.
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "w%d", i);
        log_add(LOG_K_INFO, buf);
    }
    ck(log_count() == LOG_LINES, "still at capacity after wrapping repeatedly");
    ck(log_dropped() == 101, "every dropped line is counted");
    eq(log_at(LOG_LINES - 1)->text, "w99", "the newest line is the last one added");
    snprintf(buf, sizeof(buf), "w%d", 100 - (int)LOG_LINES);
    eq(log_at(0)->text, buf, "the oldest surviving line is the right one");

    // A line longer than the slot must be truncated, not overflow it.
    log_clear();
    char big[LOG_LINE_MAX * 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    log_add(LOG_K_INFO, big);
    ck(strlen(log_at(0)->text) == LOG_LINE_MAX - 1, "an over-long line is truncated to fit");

    log_clear();
    ck(log_count() == 0 && log_dropped() == 0, "clear resets both counters");

    printf("  logring: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
