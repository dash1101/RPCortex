// No wireless chip in this image: radio.cpp's stand-in.
//
// Compiled on the boards without the CYW43 part, and on a W board built without
// both Wi-Fi and Bluetooth, so the commands and the code that ask about the
// radio still build and get a plain answer. There is nothing to own, so the
// one-owner rule is trivially kept and its tripwire is permanently silent.

#include "command.h"
#include "out.h"
#include "registry.h"
#include "logring.h"
#include "persist.h"
#include "radio.h"
#include "imagefeatures.h"

#include <string.h>

void net_op_acquire(void) {}
void net_op_release(void) {}
// No radio, so no core to be wrong about. The transport still calls it.
bool net_core_ok(void) { return true; }
// ...and nothing to own, so the rule is trivially kept and the tripwire is
// permanently silent.
bool net_op_held(void) { return true; }
void net_unowned_note(void) {}
uint32_t net_unowned_count(void) { return 0; }

bool radio_locked(void) { return false; }   // nothing to lock
int  radio_quiesce_begin(void) { return 1; }  // no bus to hold still
void radio_quiesce_end(int) {}
uint32_t radio_clock_changed(void) { return 0; }
uint32_t radio_bus_hz(void) { return 0; }
bool radio_up(void) { return false; }
void radio_down(void) {}
bool net_radio_up(void) { return false; }
bool net_radio_is_up(void) { return false; }
uint32_t net_radio_gen(void) { return 0; }

// radio [on|off|status]. The lock is still recorded, so a setting carried over
// from another image means the same thing here, but there is nothing to stop.
static int cmd_radio(int argc, char **argv) {
    const char *a = argc > 1 ? argv[1] : "status";

    if (strcmp(a, "off") == 0 || strcmp(a, "lock") == 0) {
        reg_set("System.RadioLock", "true");
        persist_save_dirty();
        out_ok("Radios locked off. This survives a reboot.");
        log_add(LOG_K_WARN, "radio: locked off");
        return 0;
    }
    if (strcmp(a, "on") == 0 || strcmp(a, "unlock") == 0) {
        reg_set("System.RadioLock", "false");
        persist_save_dirty();
        out_ok("Radio lock released.");
        log_add(LOG_K_INFO, "radio: lock released");
        return 0;
    }
    if (strcmp(a, "status") == 0) {
        out_infop("radio", feature_board_can(FEAT_WIFI)
                               ? "This image was built without Wi-Fi or Bluetooth."
                               : "This board has no radio.");
        return 0;
    }

    out_multi("Usage: radio off      lock every radio down (survives a reboot)");
    out_multi("       radio on       release the lock");
    out_multi("       radio status   show the current state");
    return a[0] ? 1 : 0;
}

void radio_register(void) {
    static const Command cr{"radio", "lock every radio off, or release it", cmd_radio,
                            nullptr, LEVEL_ADMIN};
    cmd_register(&cr);
    cmd_alias("airplane", "radio");
}
