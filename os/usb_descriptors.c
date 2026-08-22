// The USB descriptors, because the device is a composite one.
//
// `pico_stdio_usb` ships a perfectly good set for a plain CDC device, and this
// file exists only because a drive has to appear alongside the console. Setting
// PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0 compiles those out and leaves
// everything else in that library in place — the stdio driver, tusb_init(), the
// connection tracking. This is a documented switch rather than a fight with the
// linker, and it is the whole of what the "replace the USB layer" plan turned
// out to require.
//
// Interfaces:
//
//   0, 1  CDC        the console
//   2     vendor     BOOTSEL reset, the interface picotool drives
//   3     MSC        the drive          (only when CFG_TUD_MSC is on)
//
// ITF 2 is not a free choice. pico_usb_reset builds the Microsoft OS 2.0
// descriptor around PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF, which is 2, and a
// mismatch between that and the interface number written here is a device
// Windows enumerates but will not bind the reset driver to. Everything else the
// reset interface needs — the BOS descriptor, the app driver callback — still
// comes from the SDK; the one part it cannot supply is a line in a
// configuration descriptor it no longer owns.

#include "pico/stdio_usb.h"
#include "pico/unique_id.h"
#include "pico/usb_reset.h"
#include "tusb.h"

// The pure model of the modes and their descriptor lengths. usb_descriptors.c
// is the one place that builds the real bytes, so it is the one place that
// static_asserts them against the model.
#include "usbmode.h"

// Only when the SDK's own descriptors are switched off. Left conditional so a
// build that turns the composite device back off drops this file's contents
// rather than colliding with them.
#if !PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS

#define USBD_VID 0x2E8A          // Raspberry Pi, the silicon vendor

// NOT the SDK's 0x0009.
//
// Hosts cache a device's interface layout against VID:PID. Windows in
// particular keeps the enumerated interface set and will reuse it, so shipping
// a four-interface composite device under the product ID that has always meant
// "two interfaces, CDC only" produces a machine that refuses to enumerate the
// drive until its cache is cleared by hand — on the developer's machine first,
// which is exactly where it is most confusing.
//
// A locally chosen value in Raspberry Pi's vendor space. That is what every
// Pico-based firmware does and it is fine for development, but it is squatting:
// a shipping product wants its own allocation, and there is a note in
// USBMSC-DESIGN.md saying so.
#define USBD_PID 0x0011

#define USBD_MANUFACTURER "Nova Labs"
#define USBD_PRODUCT      "RPCortex"

#define USBD_ITF_CDC       0     // and 1 — CDC is two interfaces
#define USBD_ITF_RPI_RESET 2
#if CFG_TUD_MSC
#define USBD_ITF_MSC       3
#endif
#if CFG_TUD_HID
// After the drive, so MSC keeps interface 3 — the layout the shipped firmware
// enumerated, and the number a host may have cached against this VID:PID. The
// keyboard takes the next free one.
#define USBD_ITF_HID       (3 + (CFG_TUD_MSC ? 1 : 0))
#endif
// CDC (interfaces 0,1) + reset (2), plus the drive and the keyboard when each
// is compiled in.
#define USBD_ITF_MAX       (3 + (CFG_TUD_MSC ? 1 : 0) + (CFG_TUD_HID ? 1 : 0))

// The SDK asserts this relationship for its own descriptors; it holds here for
// the same reason, and getting it wrong is silent.
static_assert(USBD_ITF_RPI_RESET == PICO_USB_RESET_MS_OS_20_DESCRIPTOR_ITF,
              "the reset interface number must match the one the MS OS 2.0 descriptor names");

#define USBD_CDC_EP_CMD  0x81
#define USBD_CDC_EP_OUT  0x02
#define USBD_CDC_EP_IN   0x82
#define USBD_MSC_EP_OUT  0x03
#define USBD_MSC_EP_IN   0x83
// The keyboard's one interrupt-IN endpoint. It has its own address because the
// interface is always present alongside the drive — 0x83 belongs to MSC, so
// reusing it would be a collision, not a saving.
#define USBD_HID_EP_IN   0x84

#define USBD_CDC_CMD_MAX_SIZE    8
#define USBD_CDC_IN_OUT_MAX_SIZE 64
#define USBD_MSC_IN_OUT_MAX_SIZE 64      // full speed: 64 is the only legal bulk size
#define USBD_HID_EP_SIZE         8       // the boot keyboard report is 8 bytes
#define USBD_HID_POLL_MS         10      // how often the host reads the keyboard

#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN \
                       + (CFG_TUD_MSC ? TUD_MSC_DESC_LEN : 0) \
                       + (CFG_TUD_HID ? TUD_HID_DESC_LEN : 0))

// Bus-powered, and honest about it: the CYW43 radio and the flash together draw
// far more than the 100 mA a device may assume before it is configured.
#define USBD_MAX_POWER_MA 250

#define USBD_STR_0         0x00
#define USBD_STR_MANUF     0x01
#define USBD_STR_PRODUCT   0x02
#define USBD_STR_SERIAL    0x03
#define USBD_STR_CDC       0x04
#define USBD_STR_RPI_RESET 0x05
#define USBD_STR_MSC       0x06
#define USBD_STR_HID       0x07

static const tusb_desc_device_t usbd_desc_device = {
    .bLength         = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    // 0x0210 rather than 0x0200 because a Microsoft OS 2.0 descriptor is only
    // read from a device that claims USB 2.1. The reset interface needs it for
    // driverless access on Windows, and pico_usb_reset supplies the descriptor
    // itself — but a device that advertises 2.1 and then has no BOS descriptor
    // to offer fails to enumerate at all, so the two have to agree.
#if PICO_USB_RESET_SUPPORT_MS_OS_20_DESCRIPTOR
    .bcdUSB = 0x0210,
#else
    .bcdUSB = 0x0200,
#endif
    // Composite. The class lives in the interface association descriptors, not
    // here, and a host that is told otherwise binds the whole device to one
    // driver and never sees the second function.
    .bDeviceClass    = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor        = USBD_VID,
    .idProduct       = USBD_PID,
    .bcdDevice       = 0x0200,     // RPCortex v2
    .iManufacturer   = USBD_STR_MANUF,
    .iProduct        = USBD_STR_PRODUCT,
    .iSerialNumber   = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

#if CFG_TUD_HID
// A plain US boot keyboard. The report is [modifier, reserved, key1..key6],
// which is exactly what tud_hid_keyboard_report sends and what hidkey.h
// produces keycodes for. No report ID, so a report is the 8 bytes and nothing
// in front of them.
static const uint8_t usbd_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

// TinyUSB asks for the report descriptor by this callback rather than from the
// configuration descriptor, so the keyboard needs both: the interface block
// below and this.
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return usbd_hid_report;
}

// The HID class requires these two even for a send-only keyboard; they are not
// weak. GET_REPORT is a host asking the device to hand back a report on the
// control pipe — we never do, so it is answered with nothing. SET_REPORT is how
// the host tells the keyboard its LED state (caps / num / scroll lock); there
// are no LEDs here, so it is accepted and dropped.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}
#endif

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_MAX, USBD_STR_0, USBD_DESC_LEN,
                          0 /* bus powered */, USBD_MAX_POWER_MA),

    TUD_CDC_DESCRIPTOR(USBD_ITF_CDC, USBD_STR_CDC, USBD_CDC_EP_CMD,
                       USBD_CDC_CMD_MAX_SIZE, USBD_CDC_EP_OUT, USBD_CDC_EP_IN,
                       USBD_CDC_IN_OUT_MAX_SIZE),

    TUD_RPI_RESET_DESCRIPTOR(USBD_ITF_RPI_RESET, USBD_STR_RPI_RESET),

#if CFG_TUD_MSC
    TUD_MSC_DESCRIPTOR(USBD_ITF_MSC, USBD_STR_MSC, USBD_MSC_EP_OUT,
                       USBD_MSC_EP_IN, USBD_MSC_IN_OUT_MAX_SIZE),
#endif

#if CFG_TUD_HID
    // Boot protocol, so the keyboard is usable before a full HID driver loads —
    // in a BIOS, a bootloader, a login screen — which is where a payload most
    // often needs to type.
    TUD_HID_DESCRIPTOR(USBD_ITF_HID, USBD_STR_HID, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(usbd_hid_report), USBD_HID_EP_IN, USBD_HID_EP_SIZE,
                       USBD_HID_POLL_MS),
#endif
};

// The pure model in usbmode.h and the bytes TinyUSB builds here must agree, or a
// host reads a length that does not match what follows and the device does not
// enumerate at all. These tie the two together at compile time — the per-block
// lengths always, and the composite totals when the whole composite is built.
static_assert(TUD_CONFIG_DESC_LEN   == USBMODE_CONFIG_LEN, "config header length drifted from usbmode.h");
static_assert(TUD_CDC_DESC_LEN      == USBMODE_CDC_LEN,    "CDC block length drifted from usbmode.h");
static_assert(TUD_RPI_RESET_DESC_LEN == USBMODE_RESET_LEN, "reset block length drifted from usbmode.h");
#if CFG_TUD_MSC
static_assert(TUD_MSC_DESC_LEN      == USBMODE_MSC_LEN,    "MSC block length drifted from usbmode.h");
#endif
#if CFG_TUD_HID
static_assert(TUD_HID_DESC_LEN      == USBMODE_HID_LEN,    "HID block length drifted from usbmode.h");
#endif
#if CFG_TUD_MSC && CFG_TUD_HID
static_assert(USBD_DESC_LEN          == USBMODE_COMPOSITE_LEN,  "composite length drifted from usbmode.h");
static_assert(USBD_ITF_MAX           == USBMODE_COMPOSITE_ITFS, "interface count drifted from usbmode.h");
static_assert(sizeof(usbd_desc_cfg)  == USBMODE_COMPOSITE_LEN,  "descriptor array is not the composite length");
#endif

// The serial number is the chip's unique id, so two boards on one machine are
// two devices rather than one that keeps changing its mind.
static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const usbd_desc_str[] = {
    [USBD_STR_MANUF]     = USBD_MANUFACTURER,
    [USBD_STR_PRODUCT]   = USBD_PRODUCT,
    [USBD_STR_SERIAL]    = usbd_serial_str,
    [USBD_STR_CDC]       = "RPCortex Console",
    [USBD_STR_RPI_RESET] = "Reset",
#if CFG_TUD_MSC
    [USBD_STR_MSC]       = "RPCortex Storage",
#endif
#if CFG_TUD_HID
    [USBD_STR_HID]       = "RPCortex Keyboard",
#endif
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return usbd_desc_cfg;
}

// The returned buffer has to outlive the transfer, which is why it is static
// rather than built on the stack.
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
#define USBD_DESC_STR_MAX 24
    static uint16_t desc_str[USBD_DESC_STR_MAX];

    if (!usbd_serial_str[0]) {
        pico_get_unique_board_id_string(usbd_serial_str, sizeof(usbd_serial_str));
    }

    uint8_t len;
    if (index == 0) {
        desc_str[1] = 0x0409;          // English
        len = 1;
    } else {
        if (index >= sizeof(usbd_desc_str) / sizeof(usbd_desc_str[0])) return NULL;
        const char *str = usbd_desc_str[index];
        // A gap in the table — an index that is defined but has no string —
        // would otherwise be dereferenced as a null pointer during enumeration,
        // which is a device that never appears rather than one missing a label.
        if (!str) return NULL;
        for (len = 0; len < USBD_DESC_STR_MAX - 1 && str[len]; ++len)
            desc_str[1 + len] = str[len];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}

#endif // !PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS
