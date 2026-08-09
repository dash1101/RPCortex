#include "joinguard.h"

uint32_t join_guard_strikes(uint32_t stored, bool prev_stopped_in_join) {
    if (!prev_stopped_in_join) return stored;
    // Capped rather than allowed to run away. Nothing above the limit means
    // anything different, and a counter that wraps to zero after four billion
    // boots is a joke with a very long setup and a bad punchline.
    if (stored >= JOIN_STRIKES_MAX) return stored;
    return stored + 1;
}

JoinGuardAction join_guard_decide(uint32_t strikes) {
    if (strikes == 0) return JOIN_GO;
    if (strikes < JOIN_STRIKES_MAX) return JOIN_RETRY;
    return JOIN_STAND_BACK;
}
