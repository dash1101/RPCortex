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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(PICO_CYW43_SUPPORTED) && PICO_CYW43_SUPPORTED

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"

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
    int st = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
    out_multi("  Radio  : on");
    out_multi("  Link   : %s%s%s",
              st == CYW43_LINK_UP ? C_CYAN : C_WARN, link_text(st), C_RESET);
    out_multi("  Network: %s", reg_get("WiFi.Active", "(none)"));
    if (netif_default && netif_is_up(netif_default)) {
        out_multi("  Address: %s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
        out_multi("  Gateway: %s", ip4addr_ntoa(netif_ip4_gw(netif_default)));
        out_multi("  Netmask: %s", ip4addr_ntoa(netif_ip4_netmask(netif_default)));
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

// RSSI as bars, because -47 dBm means nothing to most people and ▂▄▆█ does.
static const char *signal_bars(int16_t rssi) {
    if (rssi >= -55) return "▂▄▆█";
    if (rssi >= -65) return "▂▄▆ ";
    if (rssi >= -75) return "▂▄  ";
    return "▂   ";
}

static int wifi_scan(void) {
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
        sleep_ms(50);
    }
    if (!g_scan.n) { out_warn("No networks found."); return 1; }

    // Strongest first — insertion sort over at most SCAN_MAX entries.
    for (uint32_t i = 1; i < g_scan.n; i++) {
        ScanEntry key = g_scan.e[i];
        uint32_t j = i;
        while (j > 0 && g_scan.e[j - 1].rssi < key.rssi) { g_scan.e[j] = g_scan.e[j - 1]; j--; }
        g_scan.e[j] = key;
    }

    out_info("%u network%s found:", (unsigned)g_scan.n, g_scan.n == 1 ? "" : "s");
    for (uint32_t i = 0; i < g_scan.n; i++) {
        const ScanEntry &e = g_scan.e[i];
        out_multi("  %s  %-32s %sch %-3u  %4d dBm%s%s", signal_bars(e.rssi), e.ssid,
                  C_GRAY, (unsigned)e.channel, (int)e.rssi, C_RESET,
                  e.auth == CYW43_AUTH_OPEN ? "  open" :
                  (saved_find(e.ssid) >= 0 ? "  saved" : ""));
    }
    return 0;
}

// --- connect / disconnect ---------------------------------------------------

static int wifi_connect(const char *ssid, const char *pw) {
    if (!radio_up()) return 1;

    // No password given: fall back to a saved one before assuming an open
    // network, so `wifi connect home` works after the first time.
    char saved_pw[REG_VAL_MAX];
    if (!pw && saved_get(ssid, saved_pw, sizeof(saved_pw)) && saved_pw[0]) pw = saved_pw;

    uint32_t auth = (pw && pw[0]) ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    out_info("Connecting to '%s'...", ssid);
    int rc = cyw43_arch_wifi_connect_timeout_ms(ssid, (pw && pw[0]) ? pw : nullptr,
                                                auth, JOIN_TIMEOUT);
    if (rc != 0) {
        int st = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        out_err("Could not connect to '%s' — %s.", ssid, link_text(st));
        return 1;
    }
    reg_set("WiFi.Active", ssid);
    saved_put(ssid, pw);
    out_ok("Connected to '%s'.", ssid);
    if (netif_default) out_multi("  Address: %s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
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
void net_autoconnect(void) {
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
                  g_scan.e[i].auth == CYW43_AUTH_OPEN ? "open" : "secured");
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
        open_net = g_scan.e[n - 1].auth == CYW43_AUTH_OPEN;
    }

    char pw[REG_VAL_MAX] = {0};
    if (!open_net) session_prompt("  Password", pw, sizeof(pw), true);
    return wifi_connect(ssid, pw[0] ? pw : nullptr);
}

static int cmd_wifi(int argc, char **argv) {
    if (argc < 2)                       return wifi_status();
    const char *sub = argv[1];

    if (!strcmp(sub, "status"))         return wifi_status();
    if (!strcmp(sub, "scan"))           return wifi_scan();
    if (!strcmp(sub, "list"))           return wifi_list();
    if (!strcmp(sub, "disconnect"))     return wifi_disconnect();

    if (!strcmp(sub, "on"))  { return radio_up() ? 0 : 1; }
    if (!strcmp(sub, "off")) { wifi_disconnect(); radio_down(); out_ok("Radio off."); return 0; }

    if (!strcmp(sub, "connect")) {
        if (argc < 3) { out_warn("Usage: wifi connect <ssid> [password]"); return 1; }
        return wifi_connect(argv[2], argc >= 4 ? argv[3] : nullptr);
    }
    if (!strcmp(sub, "forget")) {
        if (argc < 3) { out_warn("Usage: wifi forget <ssid>"); return 1; }
        if (!saved_forget(argv[2])) { out_err("'%s' is not saved.", argv[2]); return 1; }
        out_ok("Forgot '%s'.", argv[2]);
        return 0;
    }
    if (!strcmp(sub, "auto")) {
        if (argc < 3) {
            out_multi("  Autoconnect : %s", reg_get("WiFi.Auto", "false"));
            return 0;
        }
        bool on = !strcmp(argv[2], "on");
        if (!on && strcmp(argv[2], "off")) { out_warn("Usage: wifi auto on|off"); return 1; }
        reg_set("WiFi.Auto", on ? "true" : "false");
        out_ok("Autoconnect %s.", on ? "on" : "off");
        return 0;
    }

    out_warn("Usage: wifi [status | scan | connect <ssid> [pw] | disconnect |");
    out_multi("             list | forget <ssid> | auto on|off | on | off]");
    return 1;
}

#else   // no CYW43 on this board

void net_autoconnect(void) {}
bool net_available(void) { return false; }
int  net_setup_scan_and_join(void) { return 1; }

static int cmd_wifi(int, char **) {
    out_err("This board has no wireless hardware.");
    return 1;
}

#endif

void net_register(void) {
    static const Command c{"wifi", "wireless status and connection", cmd_wifi, nullptr};
    cmd_register(&c);
}
