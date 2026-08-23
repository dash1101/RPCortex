// Which USB function is ACTIVE, and the rule that keeps the drive and the
// keyboard from ever being active together.
//
// The device is one static composite: the serial console (CDC), the BOOTSEL
// reset interface, the mass-storage drive (MSC) and the keyboard (HID) are all
// enumerated, all the time. Nothing here appears or disappears on the bus —
// that was the other design, and re-enumerating to swap a config tore down the
// console (the primary interface, PuTTY at 115200) on every switch and made
// download mode slow again. Both are properties the shipped drive fought hard
// to get, so this keeps them.
//
// Instead, a "mode" is which function is switched ON: the drive is empty and
// offered nothing until STORAGE, and the keyboard sends no keystrokes until
// HID. Exactly one is active at a time, and CONSOLE means neither — which is
// the whole safety story. "Never both at once" is
// "never both ACTIVE at once", enforced by there being one current mode and by
// usbmode_can_enter() refusing the one that would collide with the other.
//
// The keyboard being present-but-idle at boot is deliberate and safe: a HID
// interface that sends no reports types nothing, so the device cannot type a
// payload the instant it is plugged in. Only an explicit command switches HID
// on, and nothing here persists across a reboot.
//
// This header is pure so the arithmetic and the invariant are provable off the
// device (usbmode_test.cpp). usb_descriptors.c static_asserts the one static
// configuration descriptor against the lengths below.
#ifndef RPC_USBMODE_H
#define RPC_USBMODE_H

#include <stdint.h>

typedef enum {
    USB_MODE_CONSOLE = 0,   // neither the drive nor the keyboard is active. Boot.
    USB_MODE_STORAGE = 1,   // the download drive is offered and writable.
    USB_MODE_HID     = 2,   // the keyboard is typing.
} UsbMode;

// Descriptor block lengths, mirrored from TinyUSB's TUD_*_DESC_LEN (checked
// against sdk/lib/tinyusb/src/device/usbd.h). usb_descriptors.c static_asserts
// these against the macros so a TinyUSB change cannot drift them silently.
#define USBMODE_CONFIG_LEN 9
#define USBMODE_CDC_LEN    66
#define USBMODE_RESET_LEN  9
#define USBMODE_MSC_LEN    23
#define USBMODE_HID_LEN    25

// The one static composite carries every interface at once: CDC (two
// interfaces), reset (one), MSC (one), HID (one) — five in all — and its
// configuration descriptor is the sum of every block. These are what the host
// reads to size the configuration; a wrong total is a device that will not
// enumerate at all, which is why it is pinned where a host test can reach it.
#define USBMODE_COMPOSITE_ITFS 5
#define USBMODE_COMPOSITE_LEN \
    (USBMODE_CONFIG_LEN + USBMODE_CDC_LEN + USBMODE_RESET_LEN + \
     USBMODE_MSC_LEN + USBMODE_HID_LEN)

// Which function a mode switches on. The interfaces are always present; these
// say whether the drive is offered / the keyboard is typing.
static inline int usbmode_storage_active(UsbMode m) { return m == USB_MODE_STORAGE; }
static inline int usbmode_hid_active(UsbMode m)     { return m == USB_MODE_HID; }

// The mode a device must come up in: CONSOLE, so neither the drive nor the
// keyboard is active. A gadget that came up typing would type its payload into
// whatever had focus, at power-on, with nobody having asked. HID is only ever
// reached by an explicit request that does not survive a reboot.
static inline UsbMode usbmode_boot(void) { return USB_MODE_CONSOLE; }

// The invariant, as one predicate the switch and the test share: from the
// current mode, may we enter the requested one? Leaving to CONSOLE is always
// allowed; STORAGE is refused while the keyboard is active and HID while the
// drive is; re-entering the current mode is a no-op and allowed. This is what
// makes "the drive and the keyboard are never both active" true by
// construction — the collision is refused at the door.
static inline int usbmode_can_enter(UsbMode current, UsbMode requested) {
    if (requested == current) return 1;
    switch (requested) {
        case USB_MODE_CONSOLE: return 1;
        case USB_MODE_STORAGE: return current != USB_MODE_HID;
        case USB_MODE_HID:     return current != USB_MODE_STORAGE;
    }
    return 0;
}

// A mode value is one of the three, and never has both functions on. The second
// clause cannot fail for a valid enum, which is the point: it states the
// invariant so a test fails loudly if the enum ever grows a "both" member.
static inline int usbmode_ok(UsbMode m) {
    if (usbmode_storage_active(m) && usbmode_hid_active(m)) return 0;
    return m == USB_MODE_CONSOLE || m == USB_MODE_STORAGE || m == USB_MODE_HID;
}

static inline const char *usbmode_name(UsbMode m) {
    switch (m) {
        case USB_MODE_CONSOLE: return "console";
        case USB_MODE_STORAGE: return "storage";
        case USB_MODE_HID:     return "hid";
    }
    return "?";
}

#endif  // RPC_USBMODE_H
