// The scheduler's platform seam on RP2040 / RP2350.
//
// task_ctx_switch is in task_switch.S; everything else it needs is here.

#include "task.h"

#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "lock.h"
#include "logring.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "out.h"
#include <stdio.h>

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

// --- the watchdog -----------------------------------------------------------
//
// Fed from the scheduler, which runs constantly: every yield, every sleep, every
// interrupt check. If nothing yields for WATCHDOG_MS the device has stopped
// making progress — a deadlock, a runaway loop, a task that will never come back
// — and rebooting is strictly better than sitting there dark until someone
// unplugs it.
//
// Generous on purpose. The longest legitimate gap between yields is a flash
// erase-and-write, which is under 200 ms; 8 seconds cannot be hit by anything
// that is actually working.
#define WATCHDOG_MS 8000

static bool g_wd_on;

void task_watchdog_start(void) {
    watchdog_enable(WATCHDOG_MS, /*pause_on_debug*/true);
    g_wd_on = true;
}

void task_watchdog_feed(void) {
    if (g_wd_on) watchdog_update();
}

// --- stack overflow ---------------------------------------------------------

void task_stack_overflow(const char *name, uint32_t size) {
    // Interrupts stay on: this needs USB to deliver the message, and the damage
    // is already done so there is nothing left to protect.
    printf("\n");
    out_fatal("STACK OVERFLOW in task '%s'", name ? name : "?");
    out_multi("   It used more than its %u byte stack and has written past the end.",
              (unsigned)size);
    out_multi("   Memory below it is corrupt, so continuing would put the damage");
    out_multi("   somewhere it would be blamed on something else.");
    out_multi("   Rebooting.");
    log_addf(LOG_K_ERR, "stack overflow in '%s' (%u bytes)", name ? name : "?",
             (unsigned)size);
    for (int i = 0; i < 60; i++) { sleep_ms(10); fflush(stdout); }
    watchdog_reboot(0, 0, 0);
    while (true) { tight_loop_contents(); }
}

uint32_t task_this_core(void) {
    return get_core_num();
}

static volatile bool g_core1_up;

uint32_t task_core_count(void) {
    return g_core1_up ? 2 : 1;
}

// --- the cross-core critical section ----------------------------------------
//
// One of the RP2's hardware spinlocks. It is held for a handful of instructions
// inside lock_try/lock_release and never across a yield, so it cannot be the
// thing that stalls anything. Interrupts are masked while it is held, because
// an interrupt handler that touched the same spinlock on the same core would
// deadlock against itself.
static uint32_t g_hw_save;
static spin_lock_t *g_hw;

extern "C" void lock_hw_enter(void) {
    if (!g_hw) g_hw = spin_lock_instance(spin_lock_claim_unused(true));
    g_hw_save = spin_lock_blocking(g_hw);
}

extern "C" void lock_hw_exit(void) {
    if (g_hw) spin_unlock(g_hw, g_hw_save);
}

// --- core 1 -----------------------------------------------------------------
//
// Core 1 runs the same scheduler. It has no task of its own to start with, so
// it parks in a loop that yields: reschedule() picks any task whose affinity
// allows core 1, runs it, and comes back here when that task yields. There is
// no separate "core 1 task list" — one table, two cores taking from it.
static void core1_main(void) {
    // Register with the flash subsystem BEFORE running anything. Without this
    // flash_safe_execute cannot park this core, and every filesystem write from
    // core 0 would be refused — or, worse, done anyway.
    flash_safe_execute_core_init();

    while (true) {
        task_yield();
        // Nothing runnable for this core right now. Sleeping briefly rather
        // than spinning keeps it off the memory bus, which core 0 is using.
        busy_wait_us(200);
    }
}

void task_start_core1(void) {
    if (g_core1_up) return;
    // The flag is set BEFORE launching, so that by the time core 1 first calls
    // task_yield the scheduler already knows there are two cores. Setting it
    // after would leave a window where core 1 is running but reschedule still
    // believes it is alone.
    g_core1_up = true;
    multicore_launch_core1(core1_main);
}
