#include "governor.h"

#include <string.h>

unsigned gov_decide(const GovIn *in, GovState *st, GovReason *why) {
    GovReason r;
    unsigned floor = in->floor_mhz < in->base_mhz ? in->floor_mhz : in->base_mhz;
    unsigned want;

    if (in->mode == GOV_OFF)                           { r = GOVR_OFF;     want = in->base_mhz; }
    else if (in->held)                                 { r = GOVR_HELD;    want = in->base_mhz; }
    else if (in->mode == GOV_AUTO && in->power != 0)   { r = GOVR_PLUGGED; want = in->base_mhz; }
    else if (in->load >= GOV_HIGH)                     { r = GOVR_BUSY;    want = in->base_mhz; }
    else if (in->load <= GOV_LOW) {
        if (!st->quiet) { st->quiet = true; st->quiet_since = in->now_ms; }
        if ((uint32_t)(in->now_ms - st->quiet_since) >= GOV_SETTLE_MS) { r = GOVR_QUIET; want = floor; }
        else { r = GOVR_SETTLING; want = in->cur_mhz; }
    } else                                             { r = GOVR_STEADY;  want = in->cur_mhz; }

    // Anything but quiet starts the wait for quiet again.
    if (r != GOVR_QUIET && r != GOVR_SETTLING) st->quiet = false;
    // Never above the base, never below the floor.
    if (want > in->base_mhz) want = in->base_mhz;
    if (want < floor) want = floor;
    if (why) *why = r;
    return want;
}

bool gov_parse_mode(const char *s, GovMode *out) {
    if (!s) return false;
    if (!strcmp(s, "off"))  { *out = GOV_OFF;  return true; }
    if (!strcmp(s, "auto")) { *out = GOV_AUTO; return true; }
    if (!strcmp(s, "on"))   { *out = GOV_ON;   return true; }
    return false;
}

const char *gov_mode_str(GovMode m) {
    return m == GOV_OFF ? "off" : m == GOV_ON ? "on" : "auto";
}

const char *gov_reason_str(GovReason r) {
    switch (r) {
        case GOVR_OFF:      return "off";
        case GOVR_HELD:     return "held: something clock-timed is in use";
        case GOVR_PLUGGED:  return "plugged in: nothing to save";
        case GOVR_BUSY:     return "busy";
        case GOVR_SETTLING: return "quiet, settling";
        case GOVR_QUIET:    return "quiet: at the floor";
        case GOVR_STEADY:   return "steady";
    }
    return "?";
}
