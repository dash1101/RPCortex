// usbdrive — whether the filesystem is offered to whatever this is plugged into.
//
// The drive is convenient and it is also the widest thing this device exposes:
// plug it into a machine and that machine can read every file on it, including
// saved networks and account hashes. A device meant to be carried around and
// plugged into things should be able to say no, and remember having said it.

#include "command.h"
#include "out.h"
#include "registry.h"

#include <string.h>

#if CFG_TUD_MSC
bool usbmsc_enabled(void);
void usbmsc_set_enabled(bool on);
void usbmsc_report(bool verbose);
#endif

static int cmd_usbdrive(int argc, char **argv) {
#if !CFG_TUD_MSC
    (void)argc; (void)argv;
    out_err("This build has no USB drive support.");
    return 1;
#else
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        // The listing only on request: on a full device it is a hundred lines,
        // and the answer to "is it on" is the first two.
        usbmsc_report(argc >= 2 && strcmp(argv[1], "status") == 0);
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        usbmsc_set_enabled(true);
        out_ok("The drive is on.");
        out_multi("  %sUnplug and replug to make the host pick it up.%s", C_GRAY, C_RESET);
        return 0;
    }

    if (strcmp(argv[1], "off") == 0) {
        usbmsc_set_enabled(false);
        out_ok("The drive is off. No file on this device will be served over USB.");
        // Being precise about what "off" does. The drive letter may well stay
        // on the host's screen until it is unplugged, and someone who reads
        // that as "it is still exposed" would be wrong.
        out_multi("  %sThe host may still show a drive until it is unplugged; it is "
                  "empty and stays empty.%s", C_GRAY, C_RESET);
        out_multi("  %sThe console is unaffected.%s", C_GRAY, C_RESET);
        return 0;
    }

    out_err("Usage: usbdrive [on | off | status]");
    return 1;
#endif
}

void usbdrive_register(void) {
    static const Command c{"usbdrive", "usbdrive [on | off | status]", cmd_usbdrive, nullptr};
    cmd_register(&c);
}
