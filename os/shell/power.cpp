// Power management.
//
// Sleeping is not like the other hardware work. Every call added so far does
// something and returns; this one STOPS THE MACHINE, and everything the OS
// relies on being true while it runs stops with it. Four things in particular,
// each of which has to be handled before the chip is allowed to go quiet:
//
//   the watchdog   fed by the scheduler, 8 s to reboot, and nothing feeds it
//                  while asleep. Left running it turns any sleep longer than
//                  eight seconds into a reset.
//   core 1         spins in its idle loop yielding thousands of times a second.
//                  A core doing that saves no power and the second core is not
//                  a thing the sleep API knows about.
//   the console    USB drops when the clocks stop. The host sees the device
//                  disappear; on wake it comes back as a new connection.
//   time           the system timer stops. The OS's idea of "now" would jump
//                  backwards relative to the world, and every task deadline
//                  measured against it would be wrong.
//
// Waking is the same list in reverse, and in the right order: clocks, then
// stdio, then the second core, then the watchdog last — because re-enabling the
// watchdog before the scheduler is running again is a reset with extra steps.
//
// Run on hardware, and the first attempt failed on every call: the deadline was
// built with make_timeout_time_ms, which reads the SYSTEM timer, and the SDK
// requires one from the ALWAYS-ON timer. They are different clocks and the check
// is exact — `to_us_since_boot(t) % 1000 == 0`, because the AON timer counts
// milliseconds. What that returned, at every duration, was
// PICO_ERROR_INVALID_DATA. Whether the chip actually sleeps is still unproven;
// what is now known is that it is being asked properly.

#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "logring.h"
#include "blackbox.h"
#include "persist.h"
#include "powerpolicy.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/low_power.h"
#include "pico/aon_timer.h"

void task_start_core1(void);
void task_watchdog_start(void);

// The shortest sleep worth taking. Below this the wake-up costs more than the
// sleep saves, and the SDK will not honour it anyway — RP2040 wants two seconds
// from the always-on timer, RP2350 ten milliseconds.
#define POWER_MIN_MS PICO_LOW_POWER_MIN_AON_SLEEP_TIME_MS

// The rules about what may be asked for live in core/powerpolicy.cpp, which has
// no hardware in it and is therefore host-tested. This file is the part that
// cannot be.

// --- the sleep itself -------------------------------------------------------

// extern "C": these are the ABI entries, defined in api.cpp with C linkage.
extern "C" int fw_gpio_usable(unsigned pin);
extern "C" unsigned fw_gpio_count(void);

static bool g_slept;              // for `power status`
static uint32_t g_slept_ms;

int power_sleep(unsigned ms, int wake_pin, int wake_high, bool dormant) {
    PowerCheck c = power_check(ms, wake_pin, fw_gpio_count(), POWER_MIN_MS);
    if (c != POWER_OK) {
        out_err("Cannot sleep: %s.", power_check_str(c));
        if (c == POWER_TOO_SHORT)
            out_multi("  The shortest this board will take is %u ms.", (unsigned)POWER_MIN_MS);
        return 1;
    }
    if (wake_pin >= 0 && !fw_gpio_usable((unsigned)wake_pin)) {
        out_err("GPIO %d belongs to the board, not to you.", wake_pin);
        return 1;
    }
    // A timed sleep wakes on the always-on timer, and that timer has to be
    // running. Checked here rather than left to come back as an error code
    // after the console has already been told the device is going away.
    if (wake_pin < 0 && !aon_timer_is_running()) {
        out_err("The always-on timer is not running, so there is nothing to wake on.");
        out_multi("  'ntp sync' or 'date set' starts it. A pin wake works without it.");
        return 1;
    }

    out_warnp("power", "Sleeping. The USB connection will drop and come back.");
    if (ms) out_multi("  For %u ms%s.", ms, wake_pin >= 0 ? ", or until the pin changes" : "");
    else    out_multi("  Until GPIO %d goes %s.", wake_pin, wake_high ? "high" : "low");
    log_addf(LOG_K_WARN, "power: sleeping %u ms, wake pin %d", ms, wake_pin);

    // Everything that has to be true before the clocks stop, in order.
    persist_save_dirty();            // settings, while there is still a device
    out_flush();                     // the message above, before USB goes
    sleep_ms(50);                    // ...and time for the host to receive it

    watchdog_disable();              // nothing will feed it while we are out
    multicore_reset_core1();         // stop the other core spinning

    uint64_t before_us = time_us_64();

    // Keep nothing running: every clock this does not need is a clock drawing
    // current. The SDK restores them on wake.
    clock_dest_bitset_t keep = {0};

    int rc;
    if (wake_pin >= 0) {
        rc = dormant
           ? low_power_dormant_until_gpio_pin_state((uint)wake_pin, /*edge*/true,
                                                    wake_high != 0,
                                                    DORMANT_CLOCK_SOURCE_DEFAULT, &keep)
           : low_power_sleep_until_gpio_pin_state((uint)wake_pin, /*edge*/true,
                                                  wake_high != 0, &keep, /*exclusive*/false);
    } else {
        // THE DEADLINE HAS TO COME FROM THE ALWAYS-ON TIMER, not from the one
        // make_timeout_time_ms reads.
        //
        // They are different clocks. The AON timer counts MILLISECONDS, and the
        // SDK checks that a deadline handed to it came from there — the test is
        // literally `to_us_since_boot(t) % 1000 == 0`. make_timeout_time_ms
        // builds on time_us_64, which lands on a whole millisecond about one
        // time in a thousand.
        //
        // So every sleep returned PICO_ERROR_INVALID_DATA (-16), at every
        // duration, deterministically. It read as the hardware refusing and was
        // arithmetic.
        absolute_time_t until = delayed_by_ms(aon_timer_get_absolute_time(), ms);
        rc = dormant
           ? low_power_dormant_until_aon_timer(until, DORMANT_CLOCK_SOURCE_DEFAULT, &keep)
           : low_power_sleep_until_aon_timer(until, &keep, /*exclusive*/false);
    }

    // --- awake ---------------------------------------------------------------
    //
    // Order matters as much here as it did going down. The watchdog goes LAST:
    // starting it before the scheduler is running again means the first thing
    // the woken device does is fail to feed it.
    stdio_init_all();
    for (int i = 0; i < 200 && !stdio_usb_connected(); i++) sleep_ms(10);

    task_start_core1();
    task_watchdog_start();

    uint64_t after_us = time_us_64();
    g_slept = true;
    g_slept_ms = (uint32_t)((after_us - before_us) / 1000ull);

    if (rc != 0) {
        out_errp("power", "The sleep did not complete cleanly (%d).", rc);
        log_addf(LOG_K_ERR, "power: sleep returned %d", rc);
        return 1;
    }

    out_okp("power", "Awake after about %u ms.", (unsigned)g_slept_ms);
    log_addf(LOG_K_OK, "power: awake after %u ms", (unsigned)g_slept_ms);
    return 0;
}

unsigned power_min_sleep_ms(void) { return (unsigned)POWER_MIN_MS; }

// --- the command -------------------------------------------------------------

static int parse_uint(const char *s, int fallback) {
    if (!s || !*s) return fallback;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return fallback;
        v = v * 10 + (*s - '0');
        if (v > 100000000) return fallback;
    }
    return v;
}

static int cmd_power(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    if (!strcmp(sub, "status")) {
        out_info("Power");
        out_multi("  Clock            %u MHz", (unsigned)(clock_get_hz(clk_sys) / 1000000));
        out_multi("  Shortest sleep   %u ms on this board", (unsigned)POWER_MIN_MS);
        if (g_slept) out_multi("  Last sleep       about %u ms", (unsigned)g_slept_ms);
        else         out_multi("  Last sleep       none this boot");
        return 0;
    }

    bool dormant = !strcmp(sub, "dormant");
    if (!strcmp(sub, "sleep") || dormant) {
        // `power sleep 5000` or `power sleep pin 14 high`
        if (argc > 2 && !strcmp(argv[2], "pin")) {
            if (argc < 4) { out_multi("Usage: power %s pin <gpio> [high|low]", sub); return 1; }
            int pin  = parse_uint(argv[3], -1);
            int high = (argc > 4 && !strcmp(argv[4], "low")) ? 0 : 1;
            return power_sleep(0, pin, high, dormant);
        }
        int ms = argc > 2 ? parse_uint(argv[2], -1) : -1;
        if (ms < 0) { out_multi("Usage: power %s <ms> | pin <gpio> [high|low]", sub); return 1; }
        return power_sleep((unsigned)ms, -1, 1, dormant);
    }

    out_multi("Usage:");
    out_multi("  power status                   clock, limits, last sleep");
    out_multi("  power sleep <ms>               sleep, keeping memory and state");
    out_multi("  power sleep pin <gpio> [high|low]");
    out_multi("  power dormant <ms>             deeper; stops more clocks");
    out_multi("  Both drop the USB connection until the device wakes.");
    return sub == argv[1] ? 1 : 0;
}

void power_register(void) {
    static const Command c{"power", "sleep, and what the board draws", cmd_power,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
