// The three shapes the USB device can take, and the arithmetic that decides
// whether it enumerates.
//
// The device is a composite that changes at runtime: the serial console (CDC)
// is always present, and exactly one of the mass-storage drive (MSC) or the
// keyboard (HID) is present beside it — never both, never neither-plus-both.
// That "never both" is the whole safety story the maintainer asked for: a host
// that sees a keyboard AND a drive from one gadget, appearing and vanishing, is
// the confusion this avoids. It is enforced structurally — there is one current
// mode, so entering HID is leaving STORAGE.
//
// This header is the single source of truth for which interfaces each mode
// carries and how long its configuration descriptor is. usb_descriptors.c
// builds the real bytes and static_asserts each array against the numbers here;
// usbmode_test.cpp proves the invariants off-device. A wrong length is the
// classic "the device won't enumerate at all" bug, so it is worth pinning in a
// place a host test can reach.
#ifndef RPC_USBMODE_H
#define RPC_USBMODE_H

#include <stdint.h>

typedef enum {
    USB_MODE_CONSOLE = 0,   // CDC + the RPi-reset interface. The boot mode.
    USB_MODE_STORAGE = 1,   // CDC + reset + MSC. The download drive.
    USB_MODE_HID     = 2,   // CDC + reset + HID keyboard.
} UsbMode;

// Descriptor block lengths, mirrored from TinyUSB's TUD_*_DESC_LEN (checked
// against sdk/lib/tinyusb/src/device/usbd.h). usb_descriptors.c static_asserts
// these against the macros so a TinyUSB change cannot drift them silently.
#define USBMODE_CONFIG_LEN 9
#define USBMODE_CDC_LEN    66
#define USBMODE_RESET_LEN  9
#define USBMODE_MSC_LEN    23
#define USBMODE_HID_LEN    25

static inline int usbmode_has_cdc(UsbMode m) { (void)m; return 1; }   // always
static inline int usbmode_has_reset(UsbMode m) { (void)m; return 1; } // always
static inline int usbmode_has_msc(UsbMode m) { return m == USB_MODE_STORAGE; }
static inline int usbmode_has_hid(UsbMode m) { return m == USB_MODE_HID; }

// CDC is two interfaces (control + data); reset is one; MSC and HID one each.
static inline uint8_t usbmode_num_interfaces(UsbMode m) {
    uint8_t n = 2 /*CDC*/ + 1 /*reset*/;
    if (usbmode_has_msc(m)) n++;
    if (usbmode_has_hid(m)) n++;
    return n;
}

// wTotalLength: the config header plus every present interface block.
static inline uint16_t usbmode_total_len(UsbMode m) {
    uint16_t n = USBMODE_CONFIG_LEN + USBMODE_CDC_LEN + USBMODE_RESET_LEN;
    if (usbmode_has_msc(m)) n += USBMODE_MSC_LEN;
    if (usbmode_has_hid(m)) n += USBMODE_HID_LEN;
    return n;
}

// The mode a device must come up in: never HID. A gadget that enumerates as a
// keyboard the instant it is plugged into a machine would type its payload into
// whatever has focus, at power-on, with nobody having asked. So boot is a mode
// that cannot type, and HID is only ever reached by an explicit request that
// does not survive a reboot.
static inline UsbMode usbmode_boot(void) { return USB_MODE_CONSOLE; }

// The invariant, as one predicate so a test and the switch agree on it: MSC and
// HID are never both present, CDC always is.
static inline int usbmode_ok(UsbMode m) {
    if (!usbmode_has_cdc(m)) return 0;
    if (usbmode_has_msc(m) && usbmode_has_hid(m)) return 0;
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
