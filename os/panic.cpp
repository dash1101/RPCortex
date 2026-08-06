// The panic handler.
//
// Two problems with the SDK default, both of which bit on a real board.
//
// First, pico_malloc panics on ANY allocation failure (PICO_MALLOC_PANIC
// defaults to 1). That turns every careful `if (!buf) { out_err(...) }` in this
// OS into dead code — the device dies before the check runs. `meminfo` found it
// the loud way: its largest-block probe deliberately asks for allocations it
// expects to fail, and the board panicked mid-sentence. PICO_MALLOC_PANIC is now
// 0, so malloc returns null and the OS handles it. An operating system that
// cannot survive a failed allocation is not one.
//
// Second, the default panic spins forever with interrupts off. USB stops being
// serviced, so the message is cut off part-way and the terminal wedges hard
// enough to need killing — which is exactly what happened. This handler prints
// the whole message, gives USB time to drain it, and then reboots. A device that
// comes back is far more useful than one that hangs holding a clue it never
// finished writing.

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "blackbox.h"
#include "pico/bootrom.h"
#include "out.h"
#include "task.h"
#include "pico/multicore.h"

#include <stdio.h>
#include <stdarg.h>

// The app that was running when things went wrong, if any (apps.cpp sets it).
extern "C" volatile const char *g_current_app;

extern "C" void __attribute__((noreturn)) rpc_panic_handler(const char *fmt, ...) {
    // Interrupts are already off by the time a panic reaches here. printf still
    // works because stdio_usb's write path is polled, but only while something
    // keeps draining it — hence the sleep below rather than an immediate reset.
    // Core 1 is still running whatever it was doing. Stop it before printing,
    // so it cannot take a lock, touch flash, or interleave its own output into
    // the middle of the report.
    multicore_reset_core1();

    printf("\n");
    out_fatal("PANIC");

    if (fmt) {
        printf("%s   ", C_FAIL);
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("%s\n", C_RESET);
    }

    if (g_current_app)
        out_multi("   while running package '%s'", (const char *)g_current_app);

    out_multi("   Press any key to reboot, or B for the bootloader.");
    printf("\n");
    fflush(stdout);

    // Wait for a key rather than rebooting on a fixed timer. The message is the
    // only evidence of what went wrong, and a device that reboots while it is
    // still being read has thrown that away — which is exactly what happened
    // when this was a 3-second countdown.
    //
    // The wait is bounded anyway. A panic reached with interrupts masked leaves
    // USB unserviced, so no key can ever arrive; rebooting after a minute means
    // an unattended device recovers on its own instead of hanging until someone
    // notices. A countdown is printed so the wait never looks like a freeze.
    const int WAIT_S = 60;
    for (int left = WAIT_S; left > 0; left--) {
        for (int t = 0; t < 100; t++) {
            // Deliberately kept alive: this wait is the whole point of the
            // screen, and letting the watchdog cut it short would throw away the
            // message it exists to show.
            //
            // BOTH watchdogs. task_watchdog_feed satisfies the hardware one;
            // the graded one measures the STALL CLOCK, which only bb_note_yield
            // moves. Feeding one and not the other cut a sixty-second countdown
            // to three, with the reboot blamed on a task stalling at whatever
            // phase note happened to be left over:
            //
            //   [!] PANIC
            //   [!] watchdog: 'shell' pid 3 core 0 stalled 3002 ms at
            //       'app_main returned'
            //
            // Nobody could reach the bootloader prompt this screen offers.
            task_watchdog_feed();
            bb_note_yield(task_now_ms());
            int c = getchar_timeout_us(10000);
            if (c != PICO_ERROR_TIMEOUT) {
                if (c == 'b' || c == 'B') {
                    printf("\n");
                    out_multi("   Entering the bootloader...");
                    fflush(stdout);
                    sleep_ms(200);
                    rom_reset_usb_boot(0, 0);
                }
                goto reboot;
            }
        }
        // \r and clear-to-end, so the countdown rewrites one line instead of
        // scrolling the message that matters off the screen.
        printf("\r%s   rebooting automatically in %2d s%s\033[K", C_GRAY, left, C_RESET);
        fflush(stdout);
    }

reboot:
    printf("\n");
    out_multi("   Rebooting.");
    fflush(stdout);
    sleep_ms(150);
    watchdog_reboot(0, 0, 0);
    while (true) { tight_loop_contents(); }
}
