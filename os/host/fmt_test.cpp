// Size and time formatting for the `ls` columns.
//
// Rounding boundaries are where a size formatter goes wrong, and the failure is
// cosmetic enough that nobody reports it and specific enough that nobody notices
// it is wrong until they do the arithmetic. So the boundaries are pinned here.

#include "fmt.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;

static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want) == 0) return;
    fails++;
    printf("  FAIL: %s  (got '%s', want '%s')\n", what, got, want);
}

static void sz(uint32_t n, const char *want) {
    char out[16];
    fmt_size(n, out, sizeof(out));
    char what[48];
    snprintf(what, sizeof(what), "fmt_size(%u)", (unsigned)n);
    eq(out, want, what);
}

int main(void) {
    // Bytes, below the first threshold.
    sz(0,    "0B");
    sz(1,    "1B");
    sz(512,  "512B");
    sz(1023, "1023B");

    // Kilobytes. Under 10 keeps one decimal, at or above 10 drops it — that
    // switch is what holds the column to seven characters.
    sz(1024,  "1.0K");
    sz(1536,  "1.5K");
    sz(2048,  "2.0K");
    sz(9216,  "9.0K");
    sz(10240, "10K");
    sz(12288, "12K");
    sz(99328, "97K");

    // The boundary. v1 tests `k >= 10` on the UNROUNDED value, so 10239 bytes
    // (9.999K) stays in the decimal branch and prints "10.0K", while 10240 is
    // the first to drop the decimal. Every expectation below was taken from
    // running v1's _fmt_size, not derived — that is the only way to be sure the
    // rounding matches rather than merely looking similar.
    sz(10239, "10.0K");
    sz(10188, "9.9K");
    sz(10035, "9.8K");

    // Just under a megabyte stays in K.
    sz(1048575, "1024K");

    // Megabytes, same decimal rule.
    sz(1048576, "1.0M");
    sz(1572864, "1.5M");
    sz(10485760, "10M");
    sz(36700160, "35M");

    // Time. Zero means "never recorded" and must read as a dash, not 1970 —
    // a date of 1970 in a listing looks like corruption.
    char t[32];
    fmt_time(0, t, sizeof(t));
    eq(t, "-", "fmt_time(0) is a dash");

    // 2026-08-02 14:30:00 UTC
    fmt_time(1785681000u, t, sizeof(t));
    eq(t, "2026-08-02 14:30:00", "fmt_time of a known epoch");

    // The epoch itself is a real timestamp, just an unlikely one.
    fmt_time(1u, t, sizeof(t));
    eq(t, "1970-01-01 00:00:01", "fmt_time of epoch+1");

    // The output must always fit the 19-character MODIFIED column.
    fmt_time(4102444800u, t, sizeof(t));       // 2100-01-01
    checks++;
    if (strlen(t) != 19) { fails++; printf("  FAIL: a far-future time is not 19 chars ('%s')\n", t); }

    printf("  fmt: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
