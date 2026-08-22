// The USB mode arithmetic: the console is always present, the drive and the
// keyboard are never present together, the device never boots as a keyboard,
// and each mode's descriptor length and interface count are exactly right. A
// wrong length here is a device that will not enumerate at all, so the numbers
// are pinned where a host test can reach them.
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

    // The console (CDC) is present in every mode — lose it and the serial link
    // that drives the whole device disappears with it.
    for (UsbMode m : modes) ck(usbmode_has_cdc(m), "CDC present in every mode");

    // The drive and the keyboard are never present at the same time. That is the
    // safety rule the maintainer asked for, stated three ways: no mode carries
    // both, exactly one carries the drive, exactly one carries the keyboard.
    int with_msc = 0, with_hid = 0, with_both = 0;
    for (UsbMode m : modes) {
        if (usbmode_has_msc(m) && usbmode_has_hid(m)) with_both++;
        if (usbmode_has_msc(m)) with_msc++;
        if (usbmode_has_hid(m)) with_hid++;
    }
    ck(with_both == 0, "no mode carries both drive and keyboard");
    ck(with_msc == 1, "exactly one mode is the drive");
    ck(with_hid == 1, "exactly one mode is the keyboard");
    ck(usbmode_has_msc(USB_MODE_STORAGE), "STORAGE is the drive");
    ck(usbmode_has_hid(USB_MODE_HID), "HID is the keyboard");

    // The device must never come up able to type. Boot is the console mode.
    ck(usbmode_boot() == USB_MODE_CONSOLE, "boot mode is console");
    ck(usbmode_boot() != USB_MODE_HID, "boot mode is never the keyboard");

    // Interface counts: CDC is two interfaces, reset one, and the drive or
    // keyboard one more.
    ck(usbmode_num_interfaces(USB_MODE_CONSOLE) == 3, "console has 3 interfaces");
    ck(usbmode_num_interfaces(USB_MODE_STORAGE) == 4, "storage has 4 interfaces");
    ck(usbmode_num_interfaces(USB_MODE_HID)     == 4, "hid has 4 interfaces");

    // wTotalLength: the config header plus each present block. These are the
    // bytes the host reads to size the configuration; a wrong one is a failed
    // enumeration.
    ck(usbmode_total_len(USB_MODE_CONSOLE) == 9 + 66 + 9,        "console length = 84");
    ck(usbmode_total_len(USB_MODE_STORAGE) == 9 + 66 + 9 + 23,   "storage length = 107");
    ck(usbmode_total_len(USB_MODE_HID)     == 9 + 66 + 9 + 25,   "hid length = 109");

    // The three named modes are all valid; the predicate the switch shares
    // agrees.
    for (UsbMode m : modes) ck(usbmode_ok(m), "each named mode is valid");

    // Names, so a status line and a log read the same word.
    ck(usbmode_name(USB_MODE_CONSOLE)[0] == 'c', "console names itself");
    ck(usbmode_name(USB_MODE_STORAGE)[0] == 's', "storage names itself");
    ck(usbmode_name(USB_MODE_HID)[0]     == 'h', "hid names itself");

    printf(fails ? "  %d checks, %d FAILED\n" : "  %d checks\n", checks, fails);
    return fails ? 1 : 0;
}
