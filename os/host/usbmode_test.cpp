// The USB mode arithmetic: the device boots with neither the drive nor the
// keyboard active, the two are never active together, and the one static
// composite descriptor is exactly as long as the host expects. The mutual
// exclusion is the safety rule the maintainer asked for, so the whole
// can-enter matrix is checked, and a little state machine proves that however
// you drive it you never end up with both on.
#include "../core/usbmode.h"
#include <stdio.h>

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", what); fails++; }
}

int main(void) {
    printf("usbmode_test - USB mode arithmetic\n");

    const UsbMode modes[] = { USB_MODE_CONSOLE, USB_MODE_STORAGE, USB_MODE_HID };

    // Boot is console: neither function on. A device that came up typing would
    // type into whatever had focus the instant it was plugged in.
    ck(usbmode_boot() == USB_MODE_CONSOLE, "boot mode is console");
    ck(!usbmode_storage_active(usbmode_boot()), "boot: drive not active");
    ck(!usbmode_hid_active(usbmode_boot()),     "boot: keyboard not active");

    // Each mode switches on exactly its own function, and no mode has both on.
    ck(usbmode_storage_active(USB_MODE_STORAGE), "STORAGE activates the drive");
    ck(usbmode_hid_active(USB_MODE_HID),         "HID activates the keyboard");
    for (UsbMode m : modes)
        ck(!(usbmode_storage_active(m) && usbmode_hid_active(m)), "no mode has both on");

    // The can-enter matrix, in full. Leaving to console is always allowed;
    // re-entering the current mode is a no-op and allowed; STORAGE is refused
    // while HID is active and HID while STORAGE is.
    ck(usbmode_can_enter(USB_MODE_CONSOLE, USB_MODE_CONSOLE), "console->console ok");
    ck(usbmode_can_enter(USB_MODE_CONSOLE, USB_MODE_STORAGE), "console->storage ok");
    ck(usbmode_can_enter(USB_MODE_CONSOLE, USB_MODE_HID),     "console->hid ok");
    ck(usbmode_can_enter(USB_MODE_STORAGE, USB_MODE_CONSOLE), "storage->console ok");
    ck(usbmode_can_enter(USB_MODE_STORAGE, USB_MODE_STORAGE), "storage->storage ok (no-op)");
    ck(!usbmode_can_enter(USB_MODE_STORAGE, USB_MODE_HID),    "storage->hid REFUSED");
    ck(usbmode_can_enter(USB_MODE_HID, USB_MODE_CONSOLE),     "hid->console ok");
    ck(!usbmode_can_enter(USB_MODE_HID, USB_MODE_STORAGE),    "hid->storage REFUSED");
    ck(usbmode_can_enter(USB_MODE_HID, USB_MODE_HID),         "hid->hid ok (no-op)");

    // Drive the mode the way the firmware does — only ever moving when
    // can_enter allows it — and assert the drive and keyboard are never both on
    // at any step. A refused request leaves the mode where it was.
    {
        UsbMode cur = usbmode_boot();
        const UsbMode want[] = {
            USB_MODE_STORAGE, USB_MODE_HID /*refused*/, USB_MODE_CONSOLE,
            USB_MODE_HID, USB_MODE_STORAGE /*refused*/, USB_MODE_CONSOLE,
        };
        bool ever_both = false;
        for (UsbMode w : want) {
            if (usbmode_can_enter(cur, w)) cur = w;
            if (usbmode_storage_active(cur) && usbmode_hid_active(cur)) ever_both = true;
        }
        ck(!ever_both, "driving the mode never leaves both functions on");
        // The two refused steps mean we land back at console, having passed
        // through storage and hid but never jumped between them directly.
        ck(cur == USB_MODE_CONSOLE, "the sequence ends at console");
    }

    // The one static composite: five interfaces, and a configuration descriptor
    // that is the sum of every block. A wrong total is a failed enumeration.
    ck(USBMODE_COMPOSITE_ITFS == 5, "composite has 5 interfaces");
    ck(USBMODE_COMPOSITE_LEN == 9 + 66 + 9 + 23 + 25, "composite length = 132");

    // Every named mode is valid; the predicate the switch shares agrees.
    for (UsbMode m : modes) ck(usbmode_ok(m), "each named mode is valid");

    // Names, so a status line and a log read the same word.
    ck(usbmode_name(USB_MODE_CONSOLE)[0] == 'c', "console names itself");
    ck(usbmode_name(USB_MODE_STORAGE)[0] == 's', "storage names itself");
    ck(usbmode_name(USB_MODE_HID)[0]     == 'h', "hid names itself");

    printf(fails ? "  %d checks, %d FAILED\n" : "  %d checks\n", checks, fails);
    return fails ? 1 : 0;
}
