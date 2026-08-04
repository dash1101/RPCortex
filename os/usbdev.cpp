// The USB device task.
//
// Small, and load-bearing for a reason that is not obvious from the size.
//
// `pico_stdio_usb` normally calls tud_task() from a low-priority interrupt, so
// that USB keeps being serviced whatever the application is doing. That is the
// right default for a program, and the wrong one here: every mass-storage
// callback would then run in interrupt context, where g_fs_lock cannot be taken
// (lock_acquire yields) and flash_safe_execute cannot run at all.
//
// The escape the MSC interface appears to offer — return 0 for "not ready" and
// be called again — does not exist in practice. tud_task_ext loops until its
// event queue is empty and returning 0 queues the retry immediately, so the
// callback is re-entered inside the same interrupt while the task that was
// meant to satisfy it never gets the core back. Scheduling here is cooperative;
// that is a hang, not a slow path. There is no non-blocking deferral in the MSC
// read API, and that single fact is what moves tud_task() out of the interrupt.
//
// So PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK is 0 and this task drives it
// instead. It calls stdio_flush() rather than tud_task() directly, which is not
// a detour: stdio_flush reaches tud_task through the stdio driver's out_flush,
// and therefore under the SDK's own stdio_usb_mutex — the same lock the printf
// and getchar paths take when they call tud_task. Driving tud_task directly
// would run it concurrently with a printf on the other core and corrupt the
// device stack. Nothing is reimplemented, and nothing is re-entered.
//
// The cost is that USB is only serviced while this task runs. Anything that
// holds a core for a long time without yielding stalls enumeration as well as
// the shell — which is a bug in that code either way, and the watchdog and the
// stall-killer already exist to find it.

#include "task.h"

#include <stdio.h>

#include "pico/stdio.h"

extern "C" void usb_task_start(void);

// Provided by usbmsc.cpp when the drive is compiled in, and a no-op otherwise
// so this file does not have to know whether it was.
#if CFG_TUD_MSC
extern "C" void usbmsc_service(void);
#else
static inline void usbmsc_service(void) {}
#endif

// Pinned to core 0, and it has to be.
//
// The tempting version of this task is unpinned, so that USB keeps turning on
// core 1 while a shell command holds core 0. That is wrong for a reason that is
// invisible from here and expensive to find on hardware.
//
// TinyUSB guards its own state by disabling the USB interrupt — usbd.c calls
// dcd_int_disable around the paths that touch it — and on this part
// dcd_int_disable is irq_set_enabled(USBCTRL_IRQ, false). Every SDK interrupt
// API "affects the executing core only" (hardware/irq.h). So tud_task() on
// core 1 would disable the interrupt on a core where it was never enabled,
// while core 0's USB interrupt carries on firing into the state tud_task is
// halfway through changing. The result is not a clean failure; it is
// intermittent enumeration and stalled transfers that read like descriptor
// bugs.
//
// stdio_init_all enabled the interrupt on core 0, so core 0 is where the device
// stack runs. This is the same shape as the two bugs already found in this
// tree: flash writes that only worked from the core that had registered, and
// cyw43's async_context being bound to the core that initialised it.
static int usb_task(void *) {
    while (true) {
        // Anything the drive needs that takes the filesystem lock for a while
        // happens HERE, before the device stack is entered — see usbmsc.cpp for
        // why the order matters.
        usbmsc_service();
        // Pushes anything buffered out and, on the way, gives the device stack
        // its turn: enumeration, control transfers, and every mass-storage
        // callback happen inside here, in thread context.
        stdio_flush();
        task_yield();
    }
    return 0;
}

void usb_task_start(void) {
    // TASK_STACK_MIN is enough for stdio_flush and the CDC path, but not for
    // what the mass-storage callbacks do underneath it: a read walks littlefs,
    // which is the deepest call chain this task will ever make.
    if (task_spawn("usb", "(kernel)", usb_task, nullptr,
                   TASK_STACK_DEF, AFFINITY_CORE0) < 0) {
        printf("  could not start the USB task -- the console will stall\n");
    }
}
