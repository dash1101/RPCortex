// Per-target glue that the pure cores need. On the RP2350/RP2040 these are
// backed by SDK hardware; the host tests provide their own.
#include "pico/rand.h"
#include "pico/aon_timer.h"
#include "registry.h"

#include <stdint.h>
#include <time.h>

extern "C" uint32_t rpc_rand32(void) { return get_rand_32(); }

// Wall-clock time for file timestamps, or 0 when it is not trustworthy.
//
// kboot starts the always-on clock at a fixed date so `date` works from boot,
// which means the clock is always RUNNING but is not always RIGHT. Stamping
// files with that placeholder would fill a listing with one identical wrong
// date and make `ls` look broken. So a timestamp is only claimed once someone
// has actually set the clock — System.Clock_Set, written by `date set` — and
// until then files are left unstamped and show "-".
extern "C" uint32_t rpc_now_epoch(void) {
    const char *set = reg_get("System.Clock_Set", "false");
    if (!set || set[0] != 't') return 0;
    struct tm t;
    if (!aon_timer_get_time_calendar(&t)) return 0;
    time_t e = mktime(&t);
    return (e <= 0) ? 0 : (uint32_t)e;
}
