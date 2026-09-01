// hid / badusb — the device as a USB keyboard.
//
// The Nova D1 can be a keyboard as well as a console and a drive, which is what
// makes a "rubber ducky" payload — a scripted burst of keystrokes — possible.
// It is deliberately blunt about the danger: nothing types on its own, the
// keyboard is idle at boot and only an explicit command here switches it on,
// and it can never be on at the same time as the download drive (usb_mode_enter
// refuses the collision). Whatever is typed goes into whatever window has focus
// on the host — usually NOT the serial console you are typing this into, so aim
// first.
//
// Grounded in the Hak5 DuckyScript 1.0 command set (REM, STRING, ENTER, chords,
// DELAY, DEFAULTDELAY), parsed by core/ducky.cpp and host-tested there.

#include "command.h"
#include "interrupt.h"
#include "out.h"
#include "path.h"
#include "storage.h"
#include "task.h"
#include "usbhid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fs_cwd(void);

#if CFG_TUD_HID

// Ctrl+C stops typing at the next keystroke or between script lines — the same
// stop the drive and the script runner use.
static int hid_stop(void) { return intr_check() ? 1 : 0; }

// Rejoin the words after a subcommand into one string. The shell split on
// whitespace, so runs of spaces collapse to one — fine for a phrase to type or
// a chord to press. (A payload with exact spacing goes in a file for badusb.)
static void join_args(int argc, char **argv, int from, char *out, size_t cap) {
    size_t len = 0;
    out[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from && len + 1 < cap) out[len++] = ' ';
        int n = snprintf(out + len, cap - len, "%s", argv[i]);
        if (n < 0) break;
        len += (size_t)n;
        if (len >= cap) { len = cap - 1; break; }
    }
    out[len] = 0;
}

// --- hid ---------------------------------------------------------------------

static void hid_usage(void) {
    out_multi("Usage:");
    out_multi("  hid type <text>     type a line as keystrokes");
    out_multi("  hid key <chord>     press one chord, e.g. hid key GUI r");
    out_multi("  hid status          is the keyboard idle, typing, or blocked?");
    out_multi("  badusb <file>       run a DuckyScript payload");
    out_multi("Keystrokes go to the host's focused window, not this console.");
}

static int cmd_hid(int argc, char **argv) {
    if (argc < 2) { hid_usage(); return 1; }
    const char *sub = argv[1];

    if (!strcmp(sub, "status")) {
        UsbMode m = usb_mode_current();
        const char *state = usbmode_hid_active(m)     ? "typing now" :
                            usbmode_storage_active(m) ? "idle -- the drive is open, so it is blocked" :
                                                        "idle";
        out_multi("  USB keyboard: present, %s.", state);
        return 0;
    }

    if (!strcmp(sub, "type")) {
        if (argc < 3) { out_err("Usage: hid type <text>"); return 1; }
        char text[256];
        join_args(argc, argv, 2, text, sizeof(text));
        if (!usb_mode_enter(USB_MODE_HID)) {
            out_err("The download drive is open. Close it first -- the keyboard "
                    "and the drive cannot be on together.");
            return 1;
        }
        int n = usbhid_type_text(text, hid_stop);
        usbhid_flush(hid_stop);
        usb_mode_leave(USB_MODE_HID);
        out_ok("Typed %d character%s into the focused window.", n, n == 1 ? "" : "s");
        return 0;
    }

    if (!strcmp(sub, "key")) {
        if (argc < 3) { out_err("Usage: hid key <chord>   e.g. hid key CTRL ALT DELETE"); return 1; }
        char chord[96];
        join_args(argc, argv, 2, chord, sizeof(chord));
        if (!usb_mode_enter(USB_MODE_HID)) {
            out_err("The download drive is open. Close it first.");
            return 1;
        }
        int err = 0;
        usbhid_run_ducky(chord, hid_stop, &err);
        usbhid_flush(hid_stop);
        usb_mode_leave(USB_MODE_HID);
        if (err) { out_err("Not a key or chord: %s", chord); return 1; }
        out_ok("Sent %s.", chord);
        return 0;
    }

    out_err("hid: unknown subcommand '%s'.", sub);
    hid_usage();
    return 1;
}

// --- usbmode -----------------------------------------------------------------
//
// A read-only view of which USB function is active. The modes are not switched
// here — the drive is opened by `download`, the keyboard used by `hid`/`badusb`
// — because each of those does real work around the switch (the drive syncs
// /usb, the keyboard types a payload and stands down). This is the status line
// a script or the GUI reads to know where things stand.
static int cmd_usbmode(int argc, char **argv) {
    (void)argv;
    UsbMode m = usb_mode_current();
    out_multi("  USB mode: %s", usbmode_name(m));
    out_multi("    console   always      serial console + BOOTSEL reset");
    out_multi("    storage   %s", usbmode_storage_active(m) ? "ON          download is running" : "off");
    out_multi("    keyboard  %s", usbmode_hid_active(m) ? "ON          typing a payload" : "off         present but idle");
    if (argc > 1)
        out_multi("  To switch: 'download' opens the drive; 'hid' or 'badusb' use the keyboard.");
    return 0;
}

// --- badusb ------------------------------------------------------------------

#define BADUSB_MAX 8192      // a payload larger than this is almost certainly a mistake

// Find the payload: an explicit path (anything with a slash) is taken as given;
// a bare name is looked for in the payload folders first, then the working
// directory. Returns the resolved path in `out` when it names a real file.
static bool payload_path(const char *arg, char *out, size_t cap) {
    bool is_dir = false; uint32_t sz = 0;
    if (strchr(arg, '/')) {
        path_resolve(fs_cwd(), arg, out, cap);
        return storage_stat(out, &is_dir, &sz) && !is_dir;
    }
    static const char *const dirs[] = { "/nova/badusb", "/badusb" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(out, cap, "%s/%s", dirs[i], arg);
        if (storage_stat(out, &is_dir, &sz) && !is_dir) return true;
    }
    path_resolve(fs_cwd(), arg, out, cap);
    return storage_stat(out, &is_dir, &sz) && !is_dir;
}

static int cmd_badusb(int argc, char **argv) {
    if (argc < 2) {
        out_multi("Usage: badusb <file>");
        out_multi("  Runs a DuckyScript payload as USB keystrokes. Reads the Hak5");
        out_multi("  1.0 set plus the common Flipper extensions (REM_BLOCK, REPEAT,");
        out_multi("  HOLD/RELEASE, ALTSTRING, SYSRQ), so a Flipper payload runs as is.");
        out_multi("  A bare name is found in /nova/badusb or /badusb; or give a path.");
        out_multi("  Aim the host at the target window first; Ctrl+C stops it.");
        return 1;
    }

    char full[160];
    if (!payload_path(argv[1], full, sizeof(full))) {
        out_err("No such payload: %s", argv[1]);
        return 1;
    }

    uint32_t size = 0;
    bool is_dir = false;
    storage_stat(full, &is_dir, &size);
    if (size >= BADUSB_MAX) {
        out_err("Payload is %lu bytes; the limit is %d.", (unsigned long)size, BADUSB_MAX);
        return 1;
    }

    char *text = (char *)malloc(BADUSB_MAX);
    if (!text) { out_err("Not enough memory to read the payload."); return 1; }
    uint32_t n = storage_read_file(full, (uint8_t *)text, BADUSB_MAX - 1);
    text[n] = 0;

    if (!usb_mode_enter(USB_MODE_HID)) {
        free(text);
        out_err("The download drive is open. Close it first -- the keyboard and "
                "the drive cannot be on together.");
        return 1;
    }

    out_blank();
    out_multi("  %s%sBADUSB%s  running %s", C_BOLD, C_CYAN, C_RESET, argv[1]);
    out_multi("  %sKeystrokes are going to the host's focused window. Ctrl+C stops.%s",
              C_GRAY, C_RESET);
    out_blank();

    int err = 0;
    int lines = usbhid_run_ducky(text, hid_stop, &err);
    usbhid_flush(hid_stop);
    usb_mode_leave(USB_MODE_HID);
    free(text);

    if (intr_check())
        out_warn("Stopped.");
    if (err)
        out_warn("Line %d was not a command and was skipped.", err);
    out_ok("Ran %d line%s.", lines, lines == 1 ? "" : "s");
    return 0;
}

#else  // no keyboard in this build

static int cmd_hid(int argc, char **argv) {
    (void)argc; (void)argv;
    out_err("This build has no USB keyboard support.");
    return 1;
}
static int cmd_badusb(int argc, char **argv) {
    (void)argc; (void)argv;
    out_err("This build has no USB keyboard support.");
    return 1;
}
static int cmd_usbmode(int argc, char **argv) {
    (void)argc; (void)argv;
    out_multi("  USB mode: console (this build has no drive or keyboard).");
    return 0;
}

#endif  // CFG_TUD_HID

void hid_register(void) {
    static const Command chid{"hid",
        "hid type <text> | hid key <chord> | hid status  -- act as a USB keyboard",
        cmd_hid};
    static const Command cbad{"badusb",
        "badusb <file>  -- run a DuckyScript payload as USB keystrokes",
        cmd_badusb};
    static const Command cmode{"usbmode",
        "usbmode  -- show which USB function is active (console/storage/keyboard)",
        cmd_usbmode};
    cmd_register(&chid);
    cmd_register(&cbad);
    cmd_register(&cmode);
}
