#include "powerpolicy.h"

PowerCheck power_check(unsigned ms, int wake_pin, unsigned pin_count, unsigned min_ms) {
    // A sleep with neither a deadline nor a wake pin never ends. That is not a
    // sleep, it is a device that still has power and will never answer again,
    // and it has to be refused rather than obeyed.
    if (ms == 0 && wake_pin < 0) return POWER_NO_WAKE;

    // Zero is checked above, so this cannot mistake "no deadline" for "a
    // deadline of zero" — which are opposite things and would refuse exactly
    // the requests that are fine.
    if (ms > 0 && ms < min_ms)   return POWER_TOO_SHORT;

    if (wake_pin >= 0 && (unsigned)wake_pin >= pin_count) return POWER_BAD_PIN;
    return POWER_OK;
}

const char *power_check_str(PowerCheck c) {
    switch (c) {
        case POWER_TOO_SHORT: return "shorter than the hardware will honour";
        case POWER_NO_WAKE:   return "no duration and no wake pin, so nothing would wake it";
        case POWER_BAD_PIN:   return "not a pin on this board";
        default:              return "ok";
    }
}
