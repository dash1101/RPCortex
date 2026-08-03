// The scheduler's platform seam on RP2040 / RP2350.
//
// task_ctx_switch is in task_switch.S; everything else it needs is here.

#include "task.h"

#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/platform.h"

// The frame task_ctx_switch pops: nine words, of which only the last matters.
//
// The eight below it land in r4-r11 (in a different order on ARMv6-M, which is
// exactly why their VALUES are don't-care — a task starts with garbage in its
// callee-saved registers, and the first thing any compiled function does is
// establish its own). The ninth is popped into PC, so it is the entry point.
//
// Sizing: sp starts at top-36 and the pop of nine words leaves it at `top`,
// which is 8-byte aligned as AAPCS requires at a function boundary. It is
// transiently unaligned during the pop itself, which is allowed.
#define CTX_WORDS 9

void *task_ctx_init(void *stack_top, TaskEntry entry) {
    uint32_t *sp = (uint32_t *)stack_top - CTX_WORDS;
    memset(sp, 0, CTX_WORDS * 4);
    // Thumb bit: POP into PC performs an interworking branch and faults if bit 0
    // is clear. A C function pointer already carries it, but the cast is where
    // that would silently be lost, so it is set explicitly.
    sp[CTX_WORDS - 1] = (uint32_t)(uintptr_t)entry | 1u;
    return sp;
}

uint32_t task_now_ms(void) {
    return (uint32_t)(time_us_64() / 1000ull);
}

uint32_t task_this_core(void) {
    return get_core_num();
}

uint32_t task_core_count(void) {
    // One for now. Both chips have two cores and the scheduler already honours
    // affinity, so bringing core 1 up is additive: launch it with its own
    // scheduler loop and return 2 here. It is deliberately NOT the first step —
    // the shared structures (littlefs, the registry, the command table) need
    // their locks first, and a second core running against unlocked state fails
    // in ways that are very hard to see.
    //
    // Nothing above depends on this being 2, which is the point: a single-core
    // part runs the same OS with the same features.
    return 1;
}
