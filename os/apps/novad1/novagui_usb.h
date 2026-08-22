// Desc: The BadUSB launcher — list DuckyScript payloads, show USB mode, run one.
// File: novagui_usb.h
//
// Drives the USB HID firmware another agent owns, through its shell commands and
// nothing else: `usbmode` READS which function is active (console / storage /
// keyboard) and `badusb <file>` runs a DuckyScript payload from the badusb folder.
// There is no USB call in the package ABI; this parses the text those commands
// print, the same way the radio screens parse `lora` and `subghz`.
//
// Entering the keyboard is the firmware's business and TRANSIENT: `badusb` enters
// HID, types, and stands the keyboard back down by itself, and it refuses with a
// clear message if the download drive is open. So the launcher does not switch or
// manage modes — it reads the mode for the header and it runs `badusb`. Mutual
// exclusion is the firmware's to enforce.
//
// The commands are driven with fw_shell_run so this compiles without that agent's
// code. If they are absent — an older firmware — the screen says so rather than
// failing silently, which is the whole difference between "not on this build" and
// "broken".
#ifndef NOVA_GUI_USB_H
#define NOVA_GUI_USB_H

namespace nova {
namespace screens {

void open_badusb(void);

// Parsing `usbmode`'s reply, split out so the host suite can prove it against the
// exact strings without the screen or the ABI.
namespace usb {
enum Mode { USB_UNKNOWN, USB_CONSOLE, USB_STORAGE, USB_KEYBOARD };
Mode        parse_mode(const char *text);
const char *mode_name(Mode m);
}

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_USB_H
