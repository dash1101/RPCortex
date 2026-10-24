#include "idle.h"

uint32_t idle_timeout_ms(const char *s) {
    if (!s || !*s) return 0;
    uint32_t minutes = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;          // sign, space, letters: off
        minutes = minutes * 10 + (uint32_t)(*p - '0');
        if (minutes > IDLE_MAX_MINUTES) minutes = IDLE_MAX_MINUTES;   // clamp as we go
    }
    return minutes * 60u * 1000u;
}

bool idle_expired(uint32_t last_input_ms, uint32_t now_ms, uint32_t limit_ms) {
    if (limit_ms == 0) return false;
    return (uint32_t)(now_ms - last_input_ms) >= limit_ms;
}
