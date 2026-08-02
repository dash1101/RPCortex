#include "fmt.h"

#include <stdio.h>
#include <time.h>

void fmt_size(uint32_t bytes, char *out, uint32_t cap) {
    if (bytes < 1024) {
        snprintf(out, cap, "%uB", (unsigned)bytes);
        return;
    }
    // Tenths held in integers: a float here would pull in soft-float formatting
    // for a value that only ever needs one decimal place.
    //
    // The "drop the decimal" test is on the UNROUNDED value, which is what v1
    // did (`k >= 10` before formatting). It matters at exactly one boundary:
    // 10239 bytes is 9.999K, so it stays in the decimal branch and prints
    // "10.0K" rather than "10K". Testing the rounded value instead would print
    // "10K" there — tidier, and not what Vela shows.
    if (bytes < 1048576u) {
        if (bytes >= 10u * 1024u) {
            snprintf(out, cap, "%uK", (unsigned)((bytes + 512) / 1024));
        } else {
            uint32_t tenths = (bytes * 10 + 512) / 1024;
            snprintf(out, cap, "%u.%uK", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
        }
        return;
    }
    if (bytes >= 10u * 1048576u) {
        snprintf(out, cap, "%uM", (unsigned)(((uint64_t)bytes + 524288) / 1048576u));
    } else {
        uint32_t tenths = (uint32_t)(((uint64_t)bytes * 10 + 524288) / 1048576u);
        snprintf(out, cap, "%u.%uM", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
    }
}

void fmt_time(uint32_t epoch, char *out, uint32_t cap) {
    if (epoch == 0) { snprintf(out, cap, "-"); return; }
    time_t e = (time_t)epoch;
    struct tm *t = gmtime(&e);
    if (!t) { snprintf(out, cap, "-"); return; }
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
}
