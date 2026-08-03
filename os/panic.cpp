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
#include "out.h"

#include <stdio.h>
#include <stdarg.h>

// The app that was running when things went wrong, if any (apps.cpp sets it).
extern "C" volatile const char *g_current_app;

extern "C" void __attribute__((noreturn)) rpc_panic_handler(const char *fmt, ...) {
    // Interrupts are already off by the time a panic reaches here. printf still
    // works because stdio_usb's write path is polled, but only while something
    // keeps draining it — hence the sleep below rather than an immediate reset.
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

    out_multi("   Rebooting in 3 seconds. Hold BOOTSEL now to flash instead.");
    printf("\n");

    // Long enough for the host to pull the last of the output off the USB
    // endpoint before the device disappears.
    for (int i = 0; i < 300; i++) {
        sleep_ms(10);
        // stdio_usb needs its task pumped in some configurations; a putchar of
        // nothing is the cheapest way to keep the path warm without adding to
        // the message.
        fflush(stdout);
    }

    watchdog_reboot(0, 0, 0);
    while (true) { tight_loop_contents(); }
}
