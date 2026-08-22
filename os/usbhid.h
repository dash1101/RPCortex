// The USB keyboard: what turns it on, what types through it, and the one place
// its reports may be sent from.
//
// The device is a static composite (see usbmode.h): the keyboard interface is
// always enumerated, but idle. A "mode" says which function is switched on, and
// the drive and the keyboard are never on together — usb_mode_enter refuses the
// collision. Typing is a foreground operation: the shell task fills a queue and
// blocks until it drains, while the usb task on core 0 does the actual report
// submission, because that is the only core the USB device stack may be touched
// from (the same rule that pins the usb task — see usbdev.cpp).
#ifndef RPC_USBHID_H
#define RPC_USBHID_H

#include "usbmode.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The active USB function. Read anywhere; changed only through the two calls
// below. Starts at CONSOLE and returns there after every operation.
UsbMode usb_mode_current(void);

// Switch a function on, refusing the collision usbmode_can_enter() forbids:
// STORAGE while the keyboard is typing, HID while the drive is open. Returns
// false when refused — the caller explains which mode is in the way. Entering
// HID also arms a fresh, empty keystroke queue.
bool usb_mode_enter(UsbMode m);

// Switch back to CONSOLE, but only from the mode named — a leave for a mode that
// already ended does nothing, so a late cleanup cannot cancel a newer session.
void usb_mode_leave(UsbMode m);

// Serviced by the usb task (core 0) once per turn, the ONLY place a HID report
// is submitted. Drains the keystroke queue: presses and releases keys, and
// honours in-stream delays. A no-op unless HID is the active mode.
void usbhid_service(void);

// Type a literal string as keystrokes, US layout. Runs on the shell task: fills
// the queue the usb task drains and blocks (yielding) while it does, stopping
// early if stop() returns nonzero. Non-printable characters are skipped.
// Returns how many characters were enqueued. Does NOT enter/leave HID mode —
// the caller owns that — and does NOT wait for the last key to be sent; call
// usbhid_flush for that.
int usbhid_type_text(const char *text, int (*stop)(void));

// Run a DuckyScript, start to finish, enqueuing its keystrokes and delays.
// error_line (if given) receives the first bad line, or 0. Returns lines run.
int usbhid_run_ducky(const char *script, int (*stop)(void), int *error_line);

// Wait until the usb task has sent everything queued and released the last key,
// so the caller can leave HID mode with nothing still in flight. Yields; stops
// early on stop().
void usbhid_flush(int (*stop)(void));

#ifdef __cplusplus
}
#endif

#endif  // RPC_USBHID_H
