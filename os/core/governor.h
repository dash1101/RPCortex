// The clock governor's decision: which clock the device should run at now.
//
// Pure, so it host-tests; shell/governor.cpp feeds it once a second and does
// what it says. The rules, in order:
//
//   1. Off, or something clock-timed is in use (an SPI, I2C or UART block, a PWM
//      slice, a PIO program, the SD card): the base clock. The base is what the
//      device runs at when nothing is being saved: the boot clock, or what
//      `pulse set` last asked for.
//   2. Auto, and not known to be on battery: the base clock. Plugged in, there is
//      nothing to save. "Not known" counts as plugged in, so a reading that is
//      not there yet never slows the device down.
//   3. Busy (load at or above GOV_HIGH): the base clock, at once.
//   4. Quiet (load at or below GOV_LOW) for GOV_SETTLE_MS: the floor.
//   5. In between: stay where it is.
//
// It only ever goes down from the base, never above it.
#ifndef RPC_GOVERNOR_H
#define RPC_GOVERNOR_H

#include <stdint.h>

enum GovMode { GOV_OFF, GOV_AUTO, GOV_ON };

#define GOV_HIGH       50      // percent: scale back up at once
#define GOV_LOW        15      // percent: quiet enough to scale down...
#define GOV_SETTLE_MS  5000    // ...once it has stayed that quiet this long
#define GOV_FLOOR_MHZ  48

enum GovReason {
    GOVR_OFF,          // the governor is off
    GOVR_HELD,         // something clock-timed is in use
    GOVR_PLUGGED,      // auto, and not on battery
    GOVR_BUSY,         // load high
    GOVR_SETTLING,     // quiet, waiting out GOV_SETTLE_MS
    GOVR_QUIET,        // quiet long enough: at the floor
    GOVR_STEADY,       // in between: unchanged
};

struct GovIn {
    GovMode  mode;
    int      power;      // 1 USB, 0 battery, -1 not known
    bool     held;
    unsigned load;       // percent, the last second
    uint32_t now_ms;
    unsigned base_mhz;
    unsigned floor_mhz;
    unsigned cur_mhz;
};

struct GovState {
    bool     quiet;        // the load has been low since quiet_since
    uint32_t quiet_since;
};

// The clock to run at, in MHz. `why` (optional) says which rule decided.
unsigned gov_decide(const GovIn *in, GovState *st, GovReason *why);

bool gov_parse_mode(const char *s, GovMode *out);    // "off" "auto" "on"
const char *gov_mode_str(GovMode m);
const char *gov_reason_str(GovReason r);

#endif
