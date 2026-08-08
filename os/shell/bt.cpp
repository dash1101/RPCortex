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
// Run on hardware, and it took the chip down: bt_up called cyw43_arch_init
// itself, so a `bt scan` after the boot-time WiFi join re-initialised a chip
// that was already running — a second firmware download over a bus with a live
// async_context on it. The driver hard asserted on the bus errors. Bluetooth
// goes through net_radio_up now, which is the one place that knows.
//
// CONFIRMED on a Pico 2 W: `bt scan le 5` returned 24 devices in an ordinary
// room, with names where the advertisement carried one and blank where it did
// not — which is most of them. The whole call takes about twelve seconds for a
// five-second scan, because the stack takes up to three to come up on first use
// and the results take a while to print at 115200.
//
// TRANSMITTING is here too, which the scan path never does: `bt advertise`
// broadcasts a chosen AD payload for a bounded time. The peripheral role was
// always compiled in (ENABLE_LE_PERIPHERAL) and gap_advertisements_* were
// always linked; they were simply never wired to a command. They are now.
//
// Still DEVICE-UNCONFIRMED: classic inquiry, which needs a discoverable BR/EDR
// device in range to say anything about; and the whole advertise TRANSMIT path
// — the payloads are host-tested byte for byte (btadv_test) but that a board
// radiates them, and that a phone raises a card, needs a handset in range.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

// ===================== advertising payload builders =========================
//
// The bytes that go on the air. Kept as pure functions with no btstack or
// hardware dependency for one reason: the radio cannot be exercised on the
// host, but the PAYLOAD can, and a wrong AD byte is a silent failure — the
// transmit succeeds and nothing happens. btadv_test.cpp includes just this
// section (via BT_ADV_BUILDER_ONLY) and checks every field.
//
// A BLE legacy advertisement is at most 31 octets, a run of (length, type,
// data...) elements. Everything below fills that raw buffer.
//
// The two "ping" payloads are the proximity-spam beacons the v1 Nova D1 offered
// as "Ping iPhone" / "Ping Android": a crafted advertisement that makes a phone
// nearby raise a pairing/setup card. The formats are public and unauthenticated
// — which is exactly why the card can be spoofed at all — and are grounded in:
//
//   Apple Continuity / Proximity Pairing (message type 0x07):
//     kavishdevar/librepods "BLE Advertisements" (the byte-level field table)
//     and the widely-mirrored Flipper `ble_spam` continuity.c template.
//   Google Fast Pair:
//     Google's own spec, "Fast Pair — Provider advertising signal"
//     (developers.google.com/nearby/fast-pair/specifications/service/provider):
//     Service Data (0x16) for UUID 0xFE2C, then a 24-bit big-endian model ID.
//
// No byte here is invented; the model IDs are real registered devices, which is
// what the receiving phone looks up to draw the card.

// A small deterministic PRNG. The ping beacons want a few bytes that differ per
// burst — a fresh address and the opaque tail — so a phone treats each as a new
// device instead of suppressing a repeat. Seeded by the caller (firmware passes
// the clock; the test passes a constant) so a payload is reproducible in a test.
[[maybe_unused]] static uint32_t adv_rng(uint32_t *s) {
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

// Flags(3) + local name. A connectable device carries a name; this is what
// `bt advertise name` puts on the air. Returns the AD length written, or 0 if
// even an empty name will not fit. `*truncated` is set when the name had to be
// shortened, and the element type switches to Shortened Local Name (0x08).
[[maybe_unused]] static size_t bt_ad_build_name(uint8_t *out, size_t cap,
                                                const char *name, bool *truncated) {
    if (truncated) *truncated = false;
    if (cap < 5) return 0;                       // flags(3) + empty-name element(2)
    size_t i = 0;
    out[i++] = 0x02; out[i++] = 0x01; out[i++] = 0x06;   // Flags: LE General Disc, no BR/EDR
    size_t room = cap < 31 ? cap : 31;
    size_t maxn = room - i - 2;                  // 2 = element length byte + type byte
    size_t nl = name ? strlen(name) : 0;
    bool trunc = false;
    if (nl > maxn) { nl = maxn; trunc = true; }
    out[i++] = (uint8_t)(nl + 1);
    out[i++] = trunc ? 0x08 : 0x09;              // shortened vs complete local name
    if (name && nl) memcpy(out + i, name, nl);
    i += nl;
    if (truncated) *truncated = trunc;
    return i;
}

// Parse a hex string ("1eff4c00", "1e ff 4c" or "1e:ff:..") into raw AD bytes.
// Returns the count, or 0 on a bad digit, an odd number of digits, or more than
// 31 bytes. This is the primitive `bt advertise raw` exposes and the one the
// ping beacons are a named convenience over.
[[maybe_unused]] static size_t bt_ad_from_hex(uint8_t *out, size_t cap, const char *hex) {
    size_t n = 0;
    int hi = -1;
    for (const char *p = hex; p && *p; p++) {
        char c = *p;
        if (c == ' ' || c == ':' || c == '-' || c == '_') continue;
        int v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return 0;                           // not hex
        if (hi < 0) hi = v;
        else {
            if (n >= cap || n >= 31) return 0;
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) return 0;                        // odd number of digits
    return n;
}

// Apple proximity-pairing beacon (the "AirPods"/setup card). 31 octets:
//   1E FF 4C 00 07 19 | <25-byte proximity-pairing message>
// 1E is the element length, FF is Manufacturer Specific Data, 4C 00 is Apple's
// company id (little-endian 0x004C), 07 is the Continuity Proximity Pairing
// type, 19 is the 25-byte length that follows. The message is
//   prefix(1) model(2,BE) status(1) batt(1) charge(1) lid(1) colour(2) tail(16).
// Only company id + type 0x07 + a known model id are needed to raise the card;
// the 16-byte tail is opaque before an actual connection, so it is random per
// burst. `model` is a 16-bit AirPods-family id, e.g. 0x0E20 = AirPods Pro.
[[maybe_unused]] static size_t bt_ad_apple_ping(uint8_t *out, size_t cap,
                                                uint16_t model, uint32_t seed) {
    if (cap < 31) return 0;
    uint32_t r = seed;
    out[0] = 0x1E; out[1] = 0xFF; out[2] = 0x4C; out[3] = 0x00;
    out[4] = 0x07; out[5] = 0x19;
    out[6] = 0x01;                               // prefix: a paired device
    out[7] = (uint8_t)(model >> 8);              // model id, big-endian
    out[8] = (uint8_t)(model & 0xFF);
    out[9] = 0x55;                               // status byte, as the tools use
    out[10] = (uint8_t)adv_rng(&r);              // battery
    out[11] = (uint8_t)adv_rng(&r);              // charging / flags
    out[12] = (uint8_t)adv_rng(&r);              // lid counter
    out[13] = 0x00; out[14] = 0x00;              // colour
    for (int j = 15; j < 31; j++) out[j] = (uint8_t)adv_rng(&r);  // opaque tail
    return 31;
}

// Google Fast Pair beacon (the Android pairing half-sheet). 14 octets:
//   03 03 2C FE | 06 16 2C FE <model24,BE> | 02 0A <tx>
// First element is the 16-bit Service UUID list carrying Fast Pair's 0xFE2C
// (little-endian 2C FE); second is Service Data (0x16) for that UUID followed by
// a 24-bit big-endian model id — the discoverable-mode format straight from
// Google's Provider spec; third is a TX Power element, which real providers
// include. `model24` is a real registered id, e.g. 0xCD8256 = Bose NC 700 — the
// value the phone looks up to name the card.
[[maybe_unused]] static size_t bt_ad_fastpair_ping(uint8_t *out, size_t cap,
                                                   uint32_t model24, uint32_t seed) {
    if (cap < 14) return 0;
    uint32_t r = seed;
    out[0] = 0x03; out[1] = 0x03; out[2] = 0x2C; out[3] = 0xFE;   // UUID list: 0xFE2C
    out[4] = 0x06; out[5] = 0x16; out[6] = 0x2C; out[7] = 0xFE;   // service data: 0xFE2C
    out[8]  = (uint8_t)((model24 >> 16) & 0xFF);                  // model id, big-endian
    out[9]  = (uint8_t)((model24 >> 8) & 0xFF);
    out[10] = (uint8_t)(model24 & 0xFF);
    out[11] = 0x02; out[12] = 0x0A;                              // TX Power element
    out[13] = (uint8_t)(0xB0 | (adv_rng(&r) & 0x0F));           // a plausible level
    return 14;
}

// A static random address (top two bits set), so each ping looks like a fresh
// device rather than a repeat a phone has already dismissed. Not part of the AD
// data — it is the address the advert is sent FROM — but built here so it is
// deterministic under test alongside the payloads.
[[maybe_unused]] static void bt_adv_rand_addr(uint8_t out[6], uint32_t seed) {
    uint32_t r = seed;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)adv_rng(&r);
    out[0] |= 0xC0;                              // static random: top two bits = 11
}

#ifndef BT_ADV_BUILDER_ONLY

#include "command.h"
#include "out.h"
#include "task.h"
#include "registry.h"
#include "logring.h"
#include "lock.h"
#include "btname.h"
#include "interrupt.h"

#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI

#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"
#include "btstack.h"

// The radio owner, shared with WiFi rather than duplicated. Declared here
// because net.cpp owns it: two locks over one chip would defeat the point.
void net_op_acquire(void);
void net_op_release(void);
bool net_core_ok(void);
// Brings the shared chip up if it is not already, honours the radio lock, and
// records which core owns it. Bluetooth must go through this rather than call
// cyw43_arch_init itself — see the note beside it in net.cpp.
bool net_radio_up(void);

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

    // The WIFI side owns the chip. Asking it means one place knows whether the
    // firmware blob has already been loaded, the radio lock is honoured for
    // Bluetooth as well, and the owning core is recorded once.
    if (!net_radio_up()) return false;

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

// --- advertising ------------------------------------------------------------
//
// Transmitting. gap_advertisements_set_data does NOT copy the buffer it is
// handed — the pointer has to stay valid for as long as the advert is live — so
// the payload lives here as a file-static, never on a caller's stack.
//
// Bounded by construction: a start records a deadline and a watcher task turns
// the radio off when it passes, so a `bt advertise` that is never followed by a
// `bt advertise stop` still stops on its own. The watcher does NOT hold the
// radio lock while it waits, only for the instant it takes to disable — so WiFi
// keeps working through the advertising window, which is the normal case for
// this chip (both radios, one die). What it DOES serialise is BLE scan against
// BLE advertise: a scan refuses while an advert is live, and a start waits
// behind a scan because both take net_op and a scan holds it end to end.

static uint8_t  g_adv_data[31];        // the live AD payload; must outlive the advert
static uint8_t  g_adv_len;
static bool     g_adv_active;
static uint32_t g_adv_until;           // task_now_ms deadline
static char     g_adv_desc[24];        // what to call it in `bt status`
static bool     g_adv_watch;           // a watcher task is running

// Turn the radio off. Caller holds net_op.
static void adv_disable_locked(void) {
    if (!g_adv_active) return;
    gap_advertisements_enable(0);
    g_adv_active = false;
}

// The bounded-time guard. Every check and every write to the advertising state
// happens under net_op, the same lock a start takes, so start and watcher never
// race: a start that extends the deadline is seen whole, and a watcher deciding
// to exit sets g_adv_watch in the same critical section — there is no window
// where it has quit but still looks alive to the next start.
static int adv_watch_task(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(100);
        net_op_acquire();
        if (!g_adv_active) { g_adv_watch = false; net_op_release(); return 0; }
        if ((int32_t)(task_now_ms() - g_adv_until) >= 0) {
            adv_disable_locked();
            g_adv_watch = false;
            net_op_release();
            log_add(LOG_K_OK, "bt: advertising ended");
            return 0;
        }
        net_op_release();
    }
}

// Start advertising `data` for `secs`. `use_random` sends it from a fresh static
// random address (the ping beacons want this; a named device does not). Returns
// 0 on success and prints nothing on it — the caller prints the parseable line,
// because only it knows whether this was a name, a raw payload or a ping.
static int bt_adv_start(const uint8_t *data, uint8_t len, uint8_t adv_type,
                        bool use_random, unsigned secs, const char *desc) {
    if (!len || len > 31) { out_err("Nothing to advertise."); return 1; }

    net_op_acquire();                  // waits out any running scan; serialises HCI
    if (!net_core_ok()) { net_op_release(); out_err("Not available from this core."); return 1; }
    if (!bt_up())       { net_op_release(); return 1; }

    // Replace whatever was advertising rather than stack a second advert.
    adv_disable_locked();

    memcpy(g_adv_data, data, len);
    g_adv_len = len;

    if (use_random) {
        bd_addr_t addr;
        bt_adv_rand_addr(addr, (uint32_t)task_now_ms() ^ 0xA11CEu);
        gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_STATIC);
        gap_random_address_set(addr);
    } else {
        gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_OFF);
    }

    // 0x00A0 = 100 ms, which every advertising type accepts (non-connectable has
    // a higher floor than connectable does). channel_map 0x07 is all three
    // advertising channels; filter policy 0 accepts any scanner.
    bd_addr_t null_addr = {0, 0, 0, 0, 0, 0};
    gap_advertisements_set_params(0x00A0, 0x00A0, adv_type, 0, null_addr, 0x07, 0);
    gap_advertisements_set_data(g_adv_len, g_adv_data);
    gap_advertisements_enable(1);

    g_adv_active = true;
    g_adv_until  = task_now_ms() + secs * 1000;
    snprintf(g_adv_desc, sizeof(g_adv_desc), "%s", desc ? desc : "advert");

    if (!g_adv_watch) {
        if (task_spawn("bt-adv", "(kernel)", adv_watch_task, nullptr,
                       TASK_STACK_DEF, AFFINITY_ANY) >= 0) {
            g_adv_watch = true;
        } else {
            // An advert with nothing to stop it is the one thing not allowed.
            adv_disable_locked();
            net_op_release();
            out_err("No room for the advertising timer; not started.");
            return 1;
        }
    }

    net_op_release();
    return 0;
}

static int bt_adv_stop_cmd(void) {
    net_op_acquire();
    bool was = g_adv_active;
    adv_disable_locked();
    net_op_release();
    if (was) out_ok("Advertising stopped");
    else     out_info("Nothing was advertising.");
    return 0;
}

// Seconds for an advert, clamped, so a fat-fingered "ping ios 99999" cannot
// leave the radio on for a day. The default is the caller's, not this.
static unsigned adv_secs(const char *s) {
    int v = atoi(s);
    if (v < 1)   v = 1;
    if (v > 300) v = 300;
    return (unsigned)v;
}

// The advertise subcommand family. Split out of cmd_bt to keep that readable.
static int bt_advertise(int argc, char **argv) {
    const char *what = argc > 2 ? argv[2] : "";

    if (!strcmp(what, "stop")) return bt_adv_stop_cmd();

    if (!strcmp(what, "name")) {
        if (argc < 4) { out_err("Usage: bt advertise name <text> [seconds]"); return 1; }
        const char *name = argv[3];
        unsigned secs = argc > 4 ? adv_secs(argv[4]) : 10;
        uint8_t ad[31];
        bool trunc = false;
        size_t n = bt_ad_build_name(ad, sizeof(ad), name, &trunc);
        if (!n) { out_err("That name will not fit in an advertisement."); return 1; }
        // The shown name is what actually went on air (possibly shortened), so
        // the status line does not claim a name the radio never carried.
        char shown[27];
        size_t nl = strlen(name), maxn = 31 - 3 - 2;
        if (nl > maxn) nl = maxn;
        memcpy(shown, name, nl);
        shown[nl] = 0;
        if (bt_adv_start(ad, (uint8_t)n, 0x00 /*ADV_IND, connectable*/, false, secs, "name"))
            return 1;
        out_ok("Advertising as '%s' for %us", shown, secs);
        if (trunc) out_warn("Name shortened to fit 31 bytes.");
        return 0;
    }

    if (!strcmp(what, "raw")) {
        if (argc < 4) { out_err("Usage: bt advertise raw <hexbytes> [seconds]"); return 1; }
        uint8_t ad[31];
        size_t n = bt_ad_from_hex(ad, sizeof(ad), argv[3]);
        if (!n) { out_err("Not valid hex, or longer than 31 bytes."); return 1; }
        unsigned secs = argc > 4 ? adv_secs(argv[4]) : 10;
        if (bt_adv_start(ad, (uint8_t)n, 0x03 /*ADV_NONCONN_IND, a beacon*/, false, secs, "raw"))
            return 1;
        out_ok("Advertising %u-byte payload for %us", (unsigned)n, secs);
        return 0;
    }

    if (!strcmp(what, "ping")) {
        const char *plat = argc > 3 ? argv[3] : "";
        unsigned secs = argc > 4 ? adv_secs(argv[4]) : 10;
        uint8_t ad[31];
        size_t n = 0;
        const char *label = "";
        uint32_t seed = (uint32_t)task_now_ms() ^ 0x50494E47u;   // 'PING'
        if (!strcmp(plat, "ios") || !strcmp(plat, "iphone") || !strcmp(plat, "apple")) {
            n = bt_ad_apple_ping(ad, sizeof(ad), 0x0E20 /*AirPods Pro*/, seed);
            label = "iOS";
        } else if (!strcmp(plat, "android") || !strcmp(plat, "fastpair")) {
            n = bt_ad_fastpair_ping(ad, sizeof(ad), 0xCD8256 /*Bose NC 700*/, seed);
            label = "Android";
        } else {
            out_err("Usage: bt advertise ping ios|android [seconds]");
            return 1;
        }
        if (!n) { out_err("Could not build the ping payload."); return 1; }
        // Non-connectable and from a random address: a beacon, not a device you
        // can connect to. This is the crafted proximity-pairing advertisement —
        // see the builder comments for the sourced byte format.
        if (bt_adv_start(ad, (uint8_t)n, 0x03 /*ADV_NONCONN_IND*/, true, secs, label))
            return 1;
        out_ok("Advertising %s ping for %us", label, secs);
        return 0;
    }

    out_multi("Usage:");
    out_multi("  bt advertise name <text> [seconds]     a named, connectable device");
    out_multi("  bt advertise raw <hexbytes> [seconds]  an arbitrary AD payload");
    out_multi("  bt advertise ping ios|android [secs]   a proximity-pairing card");
    out_multi("  bt advertise stop");
    return argc > 2 ? 1 : 0;
}

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

    // Scanning and advertising both drive the LE radio, and this OS keeps one
    // task in it at a time. A start waits behind a scan (it takes net_op, which
    // this holds end to end); the other direction cannot block the same way, so
    // it is refused explicitly rather than left to run two operations at once.
    if (g_adv_active) {
        net_op_release();
        out_err("Advertising is on; stop it first (bt advertise stop).");
        return 1;
    }

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
        if (g_adv_active) {
            int32_t left = (int32_t)(g_adv_until - task_now_ms());
            if (left < 0) left = 0;
            out_multi("  Advert    %s, %ds left", g_adv_desc, (int)(left / 1000));
        }
        if (g_n_seen) out_multi("  Last scan %u device(s)", (unsigned)g_n_seen);
        return 0;
    }

    if (!strcmp(sub, "advertise") || !strcmp(sub, "adv")) return bt_advertise(argc, argv);

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
    out_multi("  bt advertise name|raw|ping|stop ...    transmit; 'advertise' for detail");
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
    static const Command c{"bt", "Bluetooth status, scanning and advertising", cmd_bt,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}

#endif  // BT_ADV_BUILDER_ONLY
