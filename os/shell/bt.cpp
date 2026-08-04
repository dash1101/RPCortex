// Bluetooth.
//
// The same CYW43439 that does WiFi does this, which is why it is possible at
// all — and it is the single capability v1 could not reach. MicroPython's
// `bluetooth` module is BLE only, so Classic, and therefore anything that plays
// audio, needed C or a custom firmware build.
//
// btstack does the protocol work. What this file does is the part btstack
// cannot know about: fitting a stack with its own run loop into an OS that
// already has a scheduler, a radio owner and a watchdog.
//
// The rule from the WiFi work applies unchanged and for the same reason: ONE
// task inside the radio at a time, on the core that initialised it. Bluetooth
// and WiFi share the chip, the SPI link to it, and the async_context that
// drives both — so a Bluetooth scan while a download is running is two tasks in
// the same driver, which is exactly what took weeks to fix once already.
//
// DEVICE-UNCONFIRMED. Everything below builds and the parsing is host-tested;
// none of it has been run against a radio.

#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "logring.h"
#include "lock.h"
#include "btname.h"
#include "interrupt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI

#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"
#include "btstack.h"

// The radio owner, shared with WiFi rather than duplicated. Declared here
// because net.cpp owns it: two locks over one chip would defeat the point.
void net_op_acquire(void);
void net_op_release(void);
bool net_core_ok(void);

static bool g_bt_up;
static bool g_scanning;

// --- what a scan collects ---------------------------------------------------

#define BT_MAX_SEEN 24

struct BtSeen {
    uint8_t  addr[6];
    char     name[BT_NAME_MAX];
    int      rssi;
    bool     classic;      // found by inquiry rather than by LE scan
};

static BtSeen g_seen[BT_MAX_SEEN];
static uint32_t g_n_seen;

static void seen_add(const uint8_t *addr, const char *name, int rssi, bool classic) {
    // Dedupe by address and keep the strongest sighting, the same way the WiFi
    // scan does: a radio reports the same device many times and a list with
    // forty copies of one phone in it is not a list.
    for (uint32_t i = 0; i < g_n_seen; i++) {
        if (memcmp(g_seen[i].addr, addr, 6) != 0) continue;
        if (rssi > g_seen[i].rssi) g_seen[i].rssi = rssi;
        if (name && name[0] && !g_seen[i].name[0])
            snprintf(g_seen[i].name, sizeof(g_seen[i].name), "%s", name);
        return;
    }
    if (g_n_seen >= BT_MAX_SEEN) return;
    BtSeen &s = g_seen[g_n_seen++];
    memcpy(s.addr, addr, 6);
    snprintf(s.name, sizeof(s.name), "%s", name ? name : "");
    s.rssi = rssi;
    s.classic = classic;
}

// --- the stack --------------------------------------------------------------

static btstack_packet_callback_registration_t g_hci_cb;

static void hci_handler(uint8_t type, uint16_t, uint8_t *packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
                g_bt_up = true;
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            bd_addr_t addr;
            gap_event_advertising_report_get_address(packet, addr);
            int rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);

            // The name, when the advertisement carries one. Plenty do not, and
            // an unnamed device is still worth listing — an address and a
            // signal is enough to tell you something is there.
            char name[BT_NAME_MAX] = {0};
            const uint8_t *data = gap_event_advertising_report_get_data(packet);
            uint8_t len = gap_event_advertising_report_get_data_length(packet);
            bt_name_from_ad(data, len, name, sizeof(name));

            seen_add(addr, name, rssi, /*classic*/false);
            break;
        }

        case GAP_EVENT_INQUIRY_RESULT: {
            bd_addr_t addr;
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            char name[BT_NAME_MAX] = {0};
            if (gap_event_inquiry_result_get_name_available(packet)) {
                int n = gap_event_inquiry_result_get_name_len(packet);
                if (n > (int)sizeof(name) - 1) n = (int)sizeof(name) - 1;
                memcpy(name, gap_event_inquiry_result_get_name(packet), n);
                name[n] = 0;
            }
            int rssi = gap_event_inquiry_result_get_rssi_available(packet)
                     ? (int8_t)gap_event_inquiry_result_get_rssi(packet) : 0;
            seen_add(addr, name, rssi, /*classic*/true);
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            g_scanning = false;
            break;

        default:
            break;
    }
}

// Bring the radio up. Lazy, like the WiFi side and for the same reason: a
// device that never touches Bluetooth should not pay for the stack being
// initialised or the firmware blob being loaded.
static bool bt_up(void) {
    if (g_bt_up) return true;
    if (!net_core_ok()) {
        out_err("Bluetooth is not available from this core.");
        return false;
    }

    if (cyw43_arch_init() != 0) {
        out_err("Could not start the wireless chip.");
        return false;
    }

    l2cap_init();
    sm_init();

    g_hci_cb.callback = &hci_handler;
    hci_add_event_handler(&g_hci_cb);
    hci_power_control(HCI_POWER_ON);

    // The stack comes up asynchronously; it is not usable until it says so.
    for (int i = 0; i < 300 && !g_bt_up; i++) task_sleep_ms(10);
    if (!g_bt_up) {
        out_err("The Bluetooth stack did not come up.");
        return false;
    }

    log_add(LOG_K_OK, "bt: stack up");
    return true;
}

// Exposed so the audio sink can require the stack without duplicating any of
// bringing it up.
bool bt_stack_up(void) { return bt_up(); }

// --- commands ---------------------------------------------------------------

static void print_seen(void) {
    if (!g_n_seen) {
        out_warn("Nothing answered.");
        out_multi("  Devices only appear while they are advertising or discoverable.");
        return;
    }
    out_info("%u device%s", (unsigned)g_n_seen, g_n_seen == 1 ? "" : "s");
    for (uint32_t i = 0; i < g_n_seen; i++) {
        const BtSeen &s = g_seen[i];
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s.addr[0], s.addr[1], s.addr[2], s.addr[3], s.addr[4], s.addr[5]);
        if (s.rssi) out_multi("  %s  %-4s %4d dBm  %s", mac,
                              s.classic ? "BR" : "LE", s.rssi, s.name);
        else        out_multi("  %s  %-4s          %s", mac,
                              s.classic ? "BR" : "LE", s.name);
    }
}

static int bt_scan(bool classic, unsigned seconds) {
    net_op_acquire();
    if (!net_core_ok()) { net_op_release(); out_err("Not available from this core."); return 1; }
    if (!bt_up()) { net_op_release(); return 1; }

    g_n_seen = 0;
    g_scanning = true;

    if (classic) {
        // Inquiry length is in 1.28 s units, which is the specification's unit
        // and not a rounding mistake.
        gap_inquiry_start((uint8_t)((seconds * 100) / 128 + 1));
    } else {
        gap_set_scan_parameters(0, 0x0030, 0x0030);
        gap_start_scan();
    }

    out_info("Scanning for %u seconds...", seconds);
    uint32_t until = task_now_ms() + seconds * 1000;
    while (task_now_ms() < until) {
        if (intr_check()) break;
        task_sleep_ms(100);
    }

    if (classic) gap_inquiry_stop();
    else         gap_stop_scan();
    g_scanning = false;

    net_op_release();
    print_seen();
    return 0;
}

static int cmd_bt(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";

    if (!strcmp(sub, "status")) {
        out_info("Bluetooth");
        out_multi("  Stack     %s", g_bt_up ? "up" : "not started");
        out_multi("  Radios    shared with WiFi on the same chip");
        if (g_n_seen) out_multi("  Last scan %u device(s)", (unsigned)g_n_seen);
        return 0;
    }

    if (!strcmp(sub, "scan")) {
        unsigned secs = 6;
        bool classic = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "classic") || !strcmp(argv[i], "br")) classic = true;
            else if (!strcmp(argv[i], "le")) classic = false;
            else {
                int v = atoi(argv[i]);
                if (v > 0 && v <= 60) secs = (unsigned)v;
            }
        }
        return bt_scan(classic, secs);
    }

    out_multi("Usage:");
    out_multi("  bt status              is the stack up");
    out_multi("  bt scan [le|classic] [seconds]");
    out_multi("  LE finds anything advertising; classic finds discoverable devices.");
    return argc > 1 ? 1 : 0;
}

#else   // no radio on this board

static int cmd_bt(int, char **) {
    out_err("This board has no Bluetooth hardware.");
    return 1;
}

#endif

void bt_register(void) {
    static const Command c{"bt", "Bluetooth status and scanning", cmd_bt,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
