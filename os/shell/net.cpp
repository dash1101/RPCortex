// Wireless — the v1 wifi.py command, rebuilt on the cyw43 driver directly.
//
// v1 reached WiFi through MicroPython's `network` module; here the driver is
// linked in, which removes a layer but adds a responsibility: the radio has to
// be brought up explicitly and torn down explicitly, and nothing else does it
// for us. So the chip is initialised lazily on the first command that needs it,
// not at boot — a device that never touches WiFi should not pay the ~5 KB of
// driver state or the firmware-blob load time.
//
// Saved networks live in the registry, as v1 kept them in its own config: up to
// NET_SAVED entries, each an SSID and its password. Those passwords are stored
// in the clear, exactly as v1 stored them, because the device has no secure
// element to seal them with — anyone holding the flash can read them either way,
// and pretending otherwise would be the only thing worse.
//
// This file compiles to a set of "no radio on this board" stubs when built for a
// board without the CYW43 part, so the RP2040/non-W images still build and the
// command still exists to say why it cannot work.

#include "command.h"
#include "out.h"
#include "registry.h"
#include "session.h"
#include "interrupt.h"
#include "task.h"
#include "logring.h"
#include "blackbox.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// RPC_HAS_WIFI comes from CMakeLists when the board has the CYW43 part.
// Deliberately NOT PICO_CYW43_SUPPORTED: that one is a CMake variable the board
// header sets via pico_board_cmake_set, so it never reaches the preprocessor and
// testing it here silently compiles the "no hardware" stub onto a Pico W.
#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI


#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"

// Every cyw43 and lwIP call has to hold this.
//
// The driver runs in threadsafe_background mode: its work happens in an
// interrupt that can preempt at any instruction, and lwIP's structures are
// shared with it. Reading netif_ip4_addr while DHCP is halfway through
// replacing it returns a pointer that was valid a moment ago and is not now —
// which faults as a bad data address somewhere that looks unrelated.
//
// This file had NO locking at all. That survived while only the shell touched
// the network, because one caller plus an interrupt is a narrow window. Giving
// the boot join its own task widened it to two callers and it started faulting
// within seconds of associating.
//
// Scoped, so no early return can take the lock and forget to give it back.
struct NetLock {
    NetLock()  { cyw43_arch_lwip_begin(); }
    ~NetLock() { cyw43_arch_lwip_end(); }
};

#define NET_SAVED     4        // saved networks, registry-backed
#define SCAN_MAX     24        // networks reported by one scan
#define JOIN_TIMEOUT 20000     // ms; a WPA2 join that has not landed by now failed

static bool g_radio_up = false;

// --- radio lifecycle --------------------------------------------------------

// Bring the chip up on demand. cyw43_arch_init loads the firmware blob over
// SPI, which takes a moment and is why this is not done at boot.
static bool radio_up(void) {
    if (g_radio_up) return true;
    if (cyw43_arch_init() != 0) {
        out_err("Could not start the wireless chip.");
        return false;
    }
    cyw43_arch_enable_sta_mode();
    g_radio_up = true;
    return true;
}

static void radio_down(void) {
    if (!g_radio_up) return;
    cyw43_arch_deinit();
    g_radio_up = false;
}

// --- saved networks ---------------------------------------------------------

static void saved_key(int i, const char *field, char *out, size_t cap) {
    snprintf(out, cap, "WiFi.Net%d_%s", i, field);
}

// The slot holding `ssid`, or -1. Also used to avoid saving a duplicate.
static int saved_find(const char *ssid) {
    char k[REG_KEY_MAX];
    for (int i = 0; i < NET_SAVED; i++) {
        saved_key(i, "SSID", k, sizeof(k));
        const char *v = reg_get(k, nullptr);
        if (v && v[0] && strcmp(v, ssid) == 0) return i;
    }
    return -1;
}

static int saved_free_slot(void) {
    char k[REG_KEY_MAX];
    for (int i = 0; i < NET_SAVED; i++) {
        saved_key(i, "SSID", k, sizeof(k));
        const char *v = reg_get(k, nullptr);
        if (!v || !v[0]) return i;
    }
    return -1;
}

// Save, replacing an existing entry for the same SSID. When every slot is full
// the oldest (slot 0) is reused — a device that has seen five networks should
// still remember the one it just successfully joined.
static void saved_put(const char *ssid, const char *pw) {
    int slot = saved_find(ssid);
    if (slot < 0) slot = saved_free_slot();
    if (slot < 0) slot = 0;
    char k[REG_KEY_MAX];
    saved_key(slot, "SSID", k, sizeof(k)); reg_set(k, ssid);
    saved_key(slot, "PW",   k, sizeof(k)); reg_set(k, pw ? pw : "");
}

static bool saved_get(const char *ssid, char *pw, size_t cap) {
    int slot = saved_find(ssid);
    if (slot < 0) return false;
    char k[REG_KEY_MAX];
    saved_key(slot, "PW", k, sizeof(k));
    snprintf(pw, cap, "%s", reg_get(k, ""));
    return true;
}

static bool saved_forget(const char *ssid) {
    int slot = saved_find(ssid);
    if (slot < 0) return false;
    char k[REG_KEY_MAX];
    saved_key(slot, "SSID", k, sizeof(k)); reg_set(k, "");
    saved_key(slot, "PW",   k, sizeof(k)); reg_set(k, "");
    return true;
}

// --- status -----------------------------------------------------------------

static const char *link_text(int st) {
    switch (st) {
        case CYW43_LINK_DOWN:    return "down";
        case CYW43_LINK_JOIN:    return "joined, waiting for an address";
        case CYW43_LINK_NOIP:    return "joined, no address";
        case CYW43_LINK_UP:      return "connected";
        case CYW43_LINK_FAIL:    return "failed";
        case CYW43_LINK_NONET:   return "no such network in range";
        case CYW43_LINK_BADAUTH: return "wrong password";
        default:                 return "unknown";
    }
}

static int wifi_status(void) {
    if (!g_radio_up) {
        out_multi("  Radio  : %soff%s", C_GRAY, C_RESET);
        out_multi("  Use 'wifi scan' or 'wifi connect <ssid>' to bring it up.");
        return 0;
    }
    int st;
    { NetLock _l; st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA); }
    out_multi("  Radio  : on");
    out_multi("  Link   : %s%s%s",
              st == CYW43_LINK_UP ? C_CYAN : C_WARN, link_text(st), C_RESET);
    out_multi("  Network: %s", reg_get("WiFi.Active", "(none)"));
    // Copied out under the lock, then printed. Printing while holding it would
    // keep the driver waiting for a serial line, and formatting straight from
    // netif fields lets DHCP change them mid-sentence.
    char addr[20] = "", gw[20] = "", mask[20] = "";
    bool up = false;
    {
        NetLock _l;
        up = netif_default && netif_is_up(netif_default);
        if (up) {
            snprintf(addr, sizeof(addr), "%s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
            snprintf(gw,   sizeof(gw),   "%s", ip4addr_ntoa(netif_ip4_gw(netif_default)));
            snprintf(mask, sizeof(mask), "%s", ip4addr_ntoa(netif_ip4_netmask(netif_default)));
        }
    }
    if (up) {
        out_multi("  Address: %s", addr);
        out_multi("  Gateway: %s", gw);
        out_multi("  Netmask: %s", mask);
    }
    return 0;
}

// --- scan -------------------------------------------------------------------

struct ScanEntry { char ssid[33]; int16_t rssi; uint16_t channel; uint8_t auth; };
struct ScanState { ScanEntry e[SCAN_MAX]; uint32_t n; };
static ScanState g_scan;

// The driver reports every beacon, so the same access point arrives many times.
// Dedupe by SSID and keep the strongest sighting, which is the one that predicts
// whether a join will work.
static int scan_cb(void *, const cyw43_ev_scan_result_t *r) {
    if (!r || r->ssid_len == 0) return 0;               // hidden network
    char ssid[33];
    uint8_t len = r->ssid_len > 32 ? 32 : r->ssid_len;
    memcpy(ssid, r->ssid, len); ssid[len] = 0;

    for (uint32_t i = 0; i < g_scan.n; i++) {
        if (strcmp(g_scan.e[i].ssid, ssid) != 0) continue;
        if (r->rssi > g_scan.e[i].rssi) {
            g_scan.e[i].rssi    = r->rssi;
            g_scan.e[i].channel = r->channel;
            g_scan.e[i].auth    = r->auth_mode;
        }
        return 0;
    }
    if (g_scan.n >= SCAN_MAX) return 0;
    ScanEntry &e = g_scan.e[g_scan.n++];
    snprintf(e.ssid, sizeof(e.ssid), "%s", ssid);
    e.rssi = r->rssi; e.channel = r->channel; e.auth = r->auth_mode;
    return 0;
}

// A scan result's auth_mode is NOT one of the CYW43_AUTH_* constants, despite
// the header pointing at them. cyw43_ll.c builds it as a three-bit summary of
// the beacon's information elements: bit 0 WEP, bit 1 WPA, bit 2 WPA2. The
// CYW43_AUTH_* values (0x00400004 and friends) are what you pass to JOIN. They
// are different encodings and comparing one against the other silently mislabels
// every secured network as "open".
#define SEC_WEP  0x01
#define SEC_WPA  0x02
#define SEC_WPA2 0x04

static const char *auth_text(uint8_t sec) {
    if (sec == 0)                             return "open";
    if ((sec & SEC_WPA2) && (sec & SEC_WPA))  return "WPA/WPA2";
    if (sec & SEC_WPA2)                       return "WPA2";
    if (sec & SEC_WPA)                        return "WPA";
    if (sec & SEC_WEP)                        return "WEP";
    return "secured";
}

// The join constant for a network we have scanned. Without a scan entry the
// mixed mode is the right default: it accepts WPA or WPA2, where the AES-only
// constant fails outright against a WPA-only access point.
static uint32_t auth_for(const char *ssid) {
    for (uint32_t i = 0; i < g_scan.n; i++) {
        if (strcmp(g_scan.e[i].ssid, ssid) != 0) continue;
        uint8_t sec = g_scan.e[i].auth;
        if (sec == 0) return CYW43_AUTH_OPEN;
        if (sec & SEC_WPA2) return (sec & SEC_WPA) ? CYW43_AUTH_WPA2_MIXED_PSK
                                                   : CYW43_AUTH_WPA2_AES_PSK;
        if (sec & SEC_WPA)  return CYW43_AUTH_WPA_TKIP_PSK;
    }
    return CYW43_AUTH_WPA2_MIXED_PSK;
}

static int scan_collect(void) {
    if (!radio_up()) return 1;
    g_scan.n = 0;
    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    out_info("Scanning...");
    if (cyw43_wifi_scan(&cyw43_state, &opts, nullptr, scan_cb) != 0) {
        out_err("Could not start a scan.");
        return 1;
    }
    // A scan takes a couple of seconds; bail out rather than hang if the driver
    // never clears the flag.
    absolute_time_t deadline = make_timeout_time_ms(15000);
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) break;
        if (intr_check()) break;
        // task_sleep_ms, not sleep_ms: a raw sleep parks the core without
        // reaching the scheduler, so nothing feeds the watchdog. A scan can run
        // for seconds, and the watchdog then asked the SHELL to stop — which
        // set a flag that made every later scan and ping give up instantly.
        task_sleep_ms(20);
    }
    if (!g_scan.n) { out_warn("No networks found."); return 1; }

    // Strongest first — insertion sort over at most SCAN_MAX entries.
    for (uint32_t i = 1; i < g_scan.n; i++) {
        ScanEntry key = g_scan.e[i];
        uint32_t j = i;
        while (j > 0 && g_scan.e[j - 1].rssi < key.rssi) { g_scan.e[j] = g_scan.e[j - 1]; j--; }
        g_scan.e[j] = key;
    }
    return 0;
}

// The user-facing scan: collect, then print v1's table.
static int wifi_scan(void) {
    if (scan_collect() != 0) return 1;

    // v1's table: RSSI, CH, SECURITY, SSID under a 48-hyphen rule.
    out_blank();
    out_multi("  %-4s  %-6s  %-10s  %s", "RSSI", "CH", "SECURITY", "SSID");
    out_multi("  %s------------------------------------------------%s", C_GRAY, C_RESET);
    for (uint32_t i = 0; i < g_scan.n; i++) {
        const ScanEntry &e = g_scan.e[i];
        out_multi("  %4d  %5u  %-10s  %s%s%s", (int)e.rssi, (unsigned)e.channel,
                  auth_text(e.auth), C_CYAN, e.ssid, C_RESET);
    }
    out_blank();
    out_multi("  %u network(s) found.", (unsigned)g_scan.n);
    out_blank();
    return 0;
}

// --- connect / disconnect ---------------------------------------------------

static int wifi_connect(const char *ssid, const char *pw, bool quiet = false);

// True on success. Wraps the join so the boot path can stay silent about it.
static bool wifi_connect_quiet(const char *ssid, const char *pw) {
    return wifi_connect(ssid, pw, /*quiet*/true) == 0;
}

static int wifi_connect(const char *ssid, const char *pw, bool quiet) {
    if (!radio_up()) return 1;

    // No password given: fall back to a saved one before assuming an open
    // network, so `wifi connect home` works after the first time.
    char saved_pw[REG_VAL_MAX];
    if (!pw && saved_get(ssid, saved_pw, sizeof(saved_pw)) && saved_pw[0]) pw = saved_pw;

    uint32_t auth = (pw && pw[0]) ? auth_for(ssid) : CYW43_AUTH_OPEN;
    if (!quiet) out_info("Connecting to '%s'...", ssid);

    // Async, then poll — NOT cyw43_arch_wifi_connect_timeout_ms.
    //
    // The blocking form does not return for as long as the join takes, which is
    // routinely several seconds and can be the full timeout. Nothing reaches
    // the scheduler in that window, so nothing feeds the watchdog, and it
    // reported the shell as unresponsive during an entirely normal WiFi setup.
    int rc;
    { NetLock _l; rc = cyw43_arch_wifi_connect_async(ssid, (pw && pw[0]) ? pw : nullptr, auth); }
    if (rc == 0) {
        absolute_time_t deadline = make_timeout_time_ms(JOIN_TIMEOUT);
        rc = -1;
        while (true) {
            int st;
            { NetLock _l; st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA); }
            if (st == CYW43_LINK_UP) { rc = 0; break; }
            if (st < 0) break;                       // failed or auth rejected
            if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) break;
            if (intr_check()) break;                 // Ctrl+C during a long join
            task_sleep_ms(50);   // a join takes seconds; polling faster buys nothing
        }
    }
    if (rc != 0) {
        int st;
        { NetLock _l; st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA); }
        if (!quiet) out_err("Could not connect to '%s' — %s.", ssid, link_text(st));
        return 1;
    }
    reg_set("WiFi.Active", ssid);
    saved_put(ssid, pw);
    if (!quiet) {
        out_ok("Connected to '%s'.", ssid);
        char a[20] = "";
        { NetLock _l; if (netif_default) snprintf(a, sizeof(a), "%s", ip4addr_ntoa(netif_ip4_addr(netif_default))); }
        if (a[0]) out_multi("  Address: %s", a);
    }
    return 0;
}

static int wifi_disconnect(void) {
    if (!g_radio_up) { out_warn("The radio is already off."); return 1; }
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    reg_set("WiFi.Active", "");
    out_ok("Disconnected.");
    return 0;
}

static int wifi_list(void) {
    char k[REG_KEY_MAX];
    int n = 0;
    for (int i = 0; i < NET_SAVED; i++) {
        saved_key(i, "SSID", k, sizeof(k));
        const char *v = reg_get(k, nullptr);
        if (!v || !v[0]) continue;
        if (!n) out_info("Saved networks:");
        out_multi("  %s%-32s%s%s", C_CYAN, v, C_RESET,
                  strcmp(v, reg_get("WiFi.Active", "")) == 0 ? "  active" : "");
        n++;
    }
    if (!n) out_multi("  (no saved networks)");
    return 0;
}

// Join a saved network at boot, if autoconnect is on.
//
// ONE attempt, not one per saved slot. Walking all four with a 20 s join timeout
// each is 80 seconds of a dead-looking console before the login prompt, which
// reads as a hang rather than as a feature — and someone sitting at a device
// that will not let them log in does not care that it is busy being helpful.
// The network tried is the one last connected to, falling back to the first
// saved; anything else is a deliberate `wifi connect` away.
void net_autoconnect_now(void);      // the inline fallback, defined below

// The boot join, run as its own task.
//
// Joining a network takes seconds — the radio has to associate, authenticate
// and complete DHCP, and none of that goes faster for being waited on. Doing it
// inline meant every boot stared at a blank screen for most of ten seconds
// before the login prompt appeared.
//
// So it runs beside the login instead. Success says nothing: a device that
// joined the network it was told to join is not news, and the prompt already
// shows it. Only a failure speaks, because that is the case where someone needs
// to do something.
static int autoconnect_task(void *) {
    char k[REG_KEY_MAX];
    int slot = -1;
    const char *last = reg_get("WiFi.Active", "");
    if (last[0]) slot = saved_find(last);
    if (slot < 0) {
        for (int i = 0; i < NET_SAVED; i++) {
            saved_key(i, "SSID", k, sizeof(k));
            const char *v = reg_get(k, nullptr);
            if (v && v[0]) { slot = i; break; }
        }
    }
    if (slot < 0) return 0;

    char ssid[33], pw[REG_VAL_MAX];
    saved_key(slot, "SSID", k, sizeof(k)); snprintf(ssid, sizeof(ssid), "%s", reg_get(k, ""));
    saved_key(slot, "PW",   k, sizeof(k)); snprintf(pw, sizeof(pw), "%s", reg_get(k, ""));
    if (!ssid[0]) return 0;

    if (wifi_connect_quiet(ssid, pw)) {
        log_addf(LOG_K_OK, "wifi: joined '%s' at boot", ssid);
        return 0;
    }
    // Loud, because this is the case that needs a person.
    out_warnp("wifi", "Could not join '%s' automatically.", ssid);
    out_multi("  'wifi connect %s' to retry, or 'wifi auto off' to stop trying.", ssid);
    log_addf(LOG_K_WARN, "wifi: boot join of '%s' failed", ssid);
    return 1;
}

void net_autoconnect(void) {
    if (strcmp(reg_get("WiFi.Auto", "false"), "true") != 0) return;

    // If the LAST run died in this task, do not start it again.
    //
    // Without this a fault during the join is a boot loop: the watchdog
    // reboots, autoconnect runs, it faults at the same point, forever — and the
    // device is unusable rather than merely offline. One skipped join costs a
    // manual `wifi connect`; a boot loop costs the whole device.
    //
    // Deliberately only skips the automatic attempt. Connecting by hand still
    // works, which is what someone will try first.
    const BlackBox *prev = bb_previous();
    if (prev && strcmp(prev->task, "wifi-join") == 0) {
        out_warnp("wifi", "The last boot crashed while connecting — not retrying automatically.");
        out_multi("  'wifi connect <ssid>' to try by hand, or 'wifi auto off' to stop.");
        log_add(LOG_K_WARN, "wifi: automatic join skipped after a crash in it");
        return;
    }

    // Spawned, not called. The login prompt appears immediately and the join
    // finishes underneath it — which is the whole point of having a scheduler.
    // TASK_STACK_NET, not TASK_STACK_DEF. This task runs the cyw43 driver,
    // lwIP's DHCP client and printf — the same paths the shell needs 8 KB for.
    //
    // Given 3 KB it overflowed, and a stack overflow does not announce itself:
    // it writes over whatever is below, which then reads as a corrupted
    // function pointer (a hard fault at an address that is not code), files
    // appearing at the root with names taken from string literals, and a shell
    // that behaves erratically in between. One cause, three symptoms that look
    // unrelated.
    //
    // The guard only catches an overflow at a YIELD, and the driver can run a
    // long way without one, so the size has to be right rather than watched.
    if (task_spawn("wifi-join", "(kernel)", autoconnect_task, nullptr,
                   TASK_STACK_NET, AFFINITY_ANY) >= 0) {
        out_infop("wifi", "Connecting to '%s' in the background...",
                  reg_get("WiFi.Active", "a saved network"));
        return;
    }
    // No room for a task: fall back to doing it inline rather than not at all.
    net_autoconnect_now();
}

void net_autoconnect_now(void) {
    if (strcmp(reg_get("WiFi.Auto", "false"), "true") != 0) return;

    char k[REG_KEY_MAX];
    int slot = -1;
    const char *last = reg_get("WiFi.Active", "");
    if (last[0]) slot = saved_find(last);
    if (slot < 0) {
        for (int i = 0; i < NET_SAVED; i++) {
            saved_key(i, "SSID", k, sizeof(k));
            const char *v = reg_get(k, nullptr);
            if (v && v[0]) { slot = i; break; }
        }
    }
    if (slot < 0) return;

    char ssid[33], pw[REG_VAL_MAX];
    saved_key(slot, "SSID", k, sizeof(k)); snprintf(ssid, sizeof(ssid), "%s", reg_get(k, ""));
    saved_key(slot, "PW",   k, sizeof(k)); snprintf(pw, sizeof(pw), "%s", reg_get(k, ""));
    if (!ssid[0]) return;

    wifi_connect(ssid, pw);      // reports its own success or failure
}

// --- first-run helper -------------------------------------------------------

bool net_available(void) { return true; }

// True when the interface is up AND has an address — "joined" is not the same as
// "usable", and a command that needs to send a packet cares about the second.
const char *net_active_ssid(void) { return reg_get("WiFi.Active", "(none)"); }

bool net_is_connected(void) {
    if (!g_radio_up) return false;
    // The address is the thing that matters: "joined" is not "usable", and a
    // command about to send a packet cares about the second. netif is asked
    // directly rather than trusting a link enum, because that is the state the
    // packet path will actually use.
    NetLock _l;
    return netif_default && netif_is_up(netif_default) &&
           !ip4_addr_isany(netif_ip4_addr(netif_default));
}

// The guided join used by first-run setup: scan, show a numbered list, let the
// user pick one (or 0 to type a hidden SSID), ask for the password, connect.
// This is v1's setup step 4, and it lives here rather than in session.cpp so the
// scan machinery has exactly one implementation.
int net_setup_scan_and_join(void) {
    if (wifi_scan() != 0) return 1;
    out_blank();
    for (uint32_t i = 0; i < g_scan.n; i++)
        out_multi("   %2u. %-24s %4d dBm  %s", (unsigned)(i + 1), g_scan.e[i].ssid,
                  (int)g_scan.e[i].rssi,
                  auth_text(g_scan.e[i].auth));
    out_multi("    0. Other / hidden network (type the name)");
    out_blank();

    char pick[8];
    session_prompt("  Network number", pick, sizeof(pick), false);
    if (pick[0] == 0) return 1;
    long n = strtol(pick, nullptr, 10);

    char ssid[33];
    bool open_net = false;
    if (n == 0) {
        session_prompt("  Network name", ssid, sizeof(ssid), false);
        if (ssid[0] == 0) return 1;
    } else {
        if (n < 1 || (uint32_t)n > g_scan.n) { out_warn("  No such entry."); return 1; }
        snprintf(ssid, sizeof(ssid), "%s", g_scan.e[n - 1].ssid);
        open_net = (g_scan.e[n - 1].auth == 0);
    }

    char pw[REG_VAL_MAX] = {0};
    if (!open_net) session_prompt("  Password", pw, sizeof(pw), true);
    return wifi_connect(ssid, pw[0] ? pw : nullptr);
}

// An SSID may contain spaces, and quoting it is not something anyone remembers
// to do. Everything after the subcommand is therefore joined back into one name,
// so `wifi connect my home network` works — which is how v1 behaved, because
// MicroPython handed it the rest of the line instead of a split argv.
//
// Quoting still works, and is the way to keep a leading or trailing space.
static void join_args(int argc, char **argv, int from, char *out, uint32_t cap) {
    out[0] = 0;
    uint32_t n = 0;
    for (int i = from; i < argc && n + 1 < cap; i++) {
        if (n && n + 1 < cap) out[n++] = ' ';
        uint32_t l = (uint32_t)strlen(argv[i]);
        if (n + l >= cap) l = cap - n - 1;
        memcpy(out + n, argv[i], l);
        n += l;
    }
    out[n] = 0;
}

// v1's join flow: a saved password is reused silently, otherwise the password is
// ASKED for rather than taken from the command line. That keeps the key out of
// the shell history, and it is the interaction people already know.
static int wifi_connect_interactive(const char *ssid, bool quiet) {
    char pw[REG_VAL_MAX] = {0};
    if (saved_get(ssid, pw, sizeof(pw)) && pw[0]) {
        if (!quiet) out_multi("Using saved password for '%s'.", ssid);
        return wifi_connect(ssid, pw);
    }
    if (quiet) {
        out_err("No saved password for '%s'. Use 'wifi add' first.", ssid);
        return 1;
    }
    session_prompt("Password (blank for open network)", pw, sizeof(pw), true);
    return wifi_connect(ssid, pw[0] ? pw : nullptr);
}

// wifi autoconnect — scan, then join the SAVED network with the strongest
// signal. Picking by signal rather than by slot order is what makes this useful
// on a device that moves between two known places.
static int wifi_autoconnect(bool quiet) {
    char k[REG_KEY_MAX];
    bool any = false;
    for (int i = 0; i < NET_SAVED && !any; i++) {
        saved_key(i, "SSID", k, sizeof(k));
        const char *v = reg_get(k, nullptr);
        if (v && v[0]) any = true;
    }
    if (!any) {
        if (quiet) out_err("No saved networks for autoconnect.");
        else       out_warn("No saved networks. Use 'wifi add <ssid>' first.");
        return 1;
    }

    if (!quiet) out_info("Scanning for saved networks...");
    if (scan_collect() != 0) return 1;

    const char *best = nullptr;
    int16_t best_rssi = -200;
    for (uint32_t i = 0; i < g_scan.n; i++) {
        if (saved_find(g_scan.e[i].ssid) < 0) continue;
        if (g_scan.e[i].rssi <= best_rssi) continue;
        best_rssi = g_scan.e[i].rssi;
        best = g_scan.e[i].ssid;
    }
    if (!best) { out_warn("None of the saved networks are in range."); return 1; }

    char pw[REG_VAL_MAX] = {0};
    saved_get(best, pw, sizeof(pw));

    // A saved entry with a blank password against a SECURED network means the
    // password was never captured — someone pressed Enter at the prompt. Joining
    // it as an open network fails with "no such network in range", which sends
    // people looking for a signal problem that does not exist. Say what actually
    // happened instead.
    if (!pw[0] && auth_for(best) != CYW43_AUTH_OPEN) {
        out_err("'%s' is saved without a password, but the network is secured.", best);
        out_multi("  Run 'wifi add %s' to store it.", best);
        return 1;
    }

    if (!quiet) out_multi("Strongest saved network: '%s'  (%d dBm)", best, (int)best_rssi);
    return wifi_connect(best, pw[0] ? pw : nullptr);
}

static void wifi_usage(void) {
    out_info("=== WiFi Commands ===");
    out_multi("  wifi status                Connection status");
    out_multi("  wifi scan                  Scan for nearby networks");
    out_multi("  wifi connect [-s] [ssid]   Connect to a network  (-s = quiet)");
    out_multi("  wifi autoconnect [-s]      Connect to strongest saved network  (-s = quiet)");
    out_multi("  wifi disconnect            Disconnect");
    out_multi("  wifi list                  List saved networks");
    out_multi("  wifi add <ssid>            Save a network");
    out_multi("  wifi forget <ssid>         Remove a saved network");
    out_multi("  wifi auto on|off           Rejoin a saved network at boot");
    out_multi("  wifi on | off              Power the radio up or down");
}

static int cmd_wifi(int argc, char **argv) {
    if (argc < 2) return wifi_status();
    const char *sub = argv[1];

    // -s after the subcommand means quiet, as v1 accepted it.
    bool quiet = false;
    int  first = 2;
    while (first < argc && !strcmp(argv[first], "-s")) { quiet = true; first++; }

    if (!strcmp(sub, "status"))     return wifi_status();
    if (!strcmp(sub, "scan"))       return wifi_scan();
    if (!strcmp(sub, "list"))       return wifi_list();
    if (!strcmp(sub, "disconnect")) return wifi_disconnect();
    if (!strcmp(sub, "help") || !strcmp(sub, "-h")) { wifi_usage(); return 0; }

    if (!strcmp(sub, "on"))  return radio_up() ? 0 : 1;
    if (!strcmp(sub, "off")) { wifi_disconnect(); radio_down(); out_ok("Radio off."); return 0; }

    if (!strcmp(sub, "autoconnect")) return wifi_autoconnect(quiet);

    if (!strcmp(sub, "connect")) {
        char ssid[33];
        if (first >= argc) {
            // No name given: offer the saved ones, as v1 did, and let a blank
            // answer take the first.
            wifi_list();
            session_prompt("SSID (blank to connect to first saved)", ssid, sizeof(ssid), false);
            if (ssid[0] == 0) {
                char k[REG_KEY_MAX];
                for (int i = 0; i < NET_SAVED; i++) {
                    saved_key(i, "SSID", k, sizeof(k));
                    const char *v = reg_get(k, nullptr);
                    if (v && v[0]) { snprintf(ssid, sizeof(ssid), "%s", v); break; }
                }
                if (ssid[0] == 0) { out_warn("No SSID entered."); return 1; }
            }
        } else {
            join_args(argc, argv, first, ssid, sizeof(ssid));
        }
        return wifi_connect_interactive(ssid, quiet);
    }

    if (!strcmp(sub, "add")) {
        if (first >= argc) { out_warn("Usage: wifi add <ssid>"); return 1; }
        char ssid[33];
        join_args(argc, argv, first, ssid, sizeof(ssid));
        char pw[REG_VAL_MAX] = {0};
        session_prompt("Password (blank for open network)", pw, sizeof(pw), true);
        saved_put(ssid, pw);
        out_ok("Saved '%s'.", ssid);
        // If a scan has been run and says this network is secured, a blank
        // password is almost certainly a mis-press. Better to say so now than
        // to let autoconnect fail confusingly later.
        if (!pw[0] && g_scan.n && auth_for(ssid) != CYW43_AUTH_OPEN)
            out_warn("  That network looks secured — a blank password will not connect.");
        return 0;
    }

    if (!strcmp(sub, "forget")) {
        if (first >= argc) { out_warn("Usage: wifi forget <ssid>"); return 1; }
        char ssid[33];
        join_args(argc, argv, first, ssid, sizeof(ssid));
        if (!saved_forget(ssid)) { out_err("'%s' is not saved.", ssid); return 1; }
        out_ok("Forgot '%s'.", ssid);
        return 0;
    }

    if (!strcmp(sub, "auto")) {
        if (first >= argc) {
            out_multi("  Autoconnect : %s", reg_get("WiFi.Auto", "false"));
            return 0;
        }
        bool on = !strcmp(argv[first], "on");
        if (!on && strcmp(argv[first], "off")) { out_warn("Usage: wifi auto on|off"); return 1; }
        reg_set("WiFi.Auto", on ? "true" : "false");
        out_ok("Autoconnect %s.", on ? "on" : "off");
        return 0;
    }

    out_warn("Unknown subcommand '%s'.", sub);
    wifi_usage();
    return 1;
}

#else   // no CYW43 part on this board

void net_autoconnect(void) {}
void net_autoconnect_now(void) {}
bool net_available(void) { return false; }
bool net_is_connected(void) { return false; }
const char *net_active_ssid(void) { return "(no radio)"; }
int  net_setup_scan_and_join(void) { return 1; }

static int cmd_wifi(int, char **) {
    out_err("This board has no wireless hardware.");
    return 1;
}

#endif

void net_register(void) {
    static const Command c{"wifi", "wireless status and connection", cmd_wifi, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
