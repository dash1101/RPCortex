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
#include "persist.h"
#include "lock.h"
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
#include "lwip/dhcp.h"

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

// --- one owner at a time ----------------------------------------------------
//
// NetLock above blocks the CORE. That is right for excluding the driver's
// interrupt, and fatal between tasks under cooperative scheduling: a task that
// blocks the core waiting for a lock another task holds has stopped the only
// thing that could release it.
//
// So the rule is that only ONE task is ever inside cyw43 or lwIP. This lock
// enforces it, and it is an RpcLock rather than the driver's — RpcLock YIELDS
// while it waits, so a task holding it keeps running and the waiter does not
// take the core down with it.
//
// Every network OPERATION takes it. Reads do not, because reads no longer
// touch lwIP at all — see NetStatus.
static RpcLock g_net_op;

// Exposed so the HTTP transport can hold ownership for the length of a
// connection. It lives here rather than there because this is where the rule
// is stated, and a second lock would defeat the point of having one.
// Taking the network lock also puts the caller on the radio's core, and gives
// it back on release.
//
// The one-owner rule was about TASKS, and that turned out to be half the rule.
// A package task is spawned with no affinity and, measured on a real device,
// runs on core 1 essentially always — so "one task at a time" was satisfied
// while every call was still arriving on the wrong core. Refusing was the
// honest thing to do while there was no way to move a task; now there is.
//
// The old affinity is remembered per task so a package that was free to run
// anywhere goes back to being free afterwards, rather than being quietly
// pinned to core 0 for the rest of its life by having once used the network.
// Declared below, with the radio lifecycle they belong to.
static bool g_radio_up;
static uint32_t g_radio_core;

#define NET_OWNERS 8
static struct { int pid; TaskAffinity was; } g_net_prev[NET_OWNERS];

void net_op_acquire(void) {
    lock_acquire(&g_net_op);

    uint32_t target = g_radio_up ? g_radio_core : 0;
    if (task_this_core() == target) return;

    int me = task_self();
    int slot = -1;
    for (int i = 0; i < NET_OWNERS; i++)
        if (!g_net_prev[i].pid) { slot = i; break; }

    // No room to remember where this task came from. Migrating anyway would
    // pin it to core 0 for the rest of its life, since nothing would ever put
    // its affinity back — a slow leak that ends with every task on one core.
    // Better to leave it where it is: net_core_ok then refuses the call with a
    // message, which is visible and recoverable.
    if (slot < 0) {
        out_warnp("net", "Too many tasks using the network at once.");
        return;
    }

    g_net_prev[slot].pid = me;
    g_net_prev[slot].was = task_affinity(me);

    // A failure here is not fatal either: net_core_ok still refuses the call,
    // so the outcome is a clear message rather than corruption.
    task_migrate_to(target);
}

void net_op_release(void) {
    int me = task_self();
    for (int i = 0; i < NET_OWNERS; i++) {
        if (g_net_prev[i].pid != me) continue;
        // Only on the OUTERMOST release. The lock is recursive, and restoring
        // on an inner one would drop this task back to core 1 halfway through
        // somebody else's connection.
        if (!lock_held_once(&g_net_op)) break;
        task_set_affinity(me, g_net_prev[i].was);
        g_net_prev[i].pid = 0;
        break;
    }
    lock_release(&g_net_op);
}

// What the last operation saw, in plain memory.
//
// This exists so that "are we online?" costs nothing and cannot deadlock.
// Five callers across four files ask that question — the prompt, sysinfo,
// compat, ping, the HTTP transport — and every one of them used to reach into
// lwIP from whatever task it happened to be on. Reading a struct instead means
// they can ask from anywhere, at any time, while a download is in flight.
struct NetStatus {
    volatile bool connected;
    char     ssid[33];
    char     ip[16];
    char     gw[16];
    char     mask[16];
    volatile int link;
};
static NetStatus g_status;

// Whether the radio has been powered up. Declared here because the status
// refresh below needs it.
// Which core brought the chip up. cyw43's threadsafe_background async_context
// records the core it was initialised on and drives lwIP from an IRQ there;
// calling in from the other core is undefined, and the SDK's own asserts for it
// compile out in a release build, so it fails silently rather than loudly.

// Refresh the cache from lwIP. Only ever called by whoever holds g_net_op.
static void status_refresh(void) {
    NetLock _l;
    g_status.link = g_radio_up ? cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA)
                               : CYW43_LINK_DOWN;
    bool up = g_radio_up && netif_default && netif_is_up(netif_default) &&
              !ip4_addr_isany(netif_ip4_addr(netif_default));
    if (up) {
        snprintf(g_status.ip,   sizeof(g_status.ip),   "%s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
        snprintf(g_status.gw,   sizeof(g_status.gw),   "%s", ip4addr_ntoa(netif_ip4_gw(netif_default)));
        snprintf(g_status.mask, sizeof(g_status.mask), "%s", ip4addr_ntoa(netif_ip4_netmask(netif_default)));
    } else {
        g_status.ip[0] = g_status.gw[0] = g_status.mask[0] = 0;
    }
    g_status.connected = up;
}

#define NET_SAVED     4        // saved networks, registry-backed
#define SCAN_MAX     24        // networks reported by one scan
#define JOIN_TIMEOUT 20000     // ms; a WPA2 join that has not landed by now failed


// --- radio lifecycle --------------------------------------------------------

// Bring the chip up on demand. cyw43_arch_init loads the firmware blob over
// SPI, which takes a moment and is why this is not done at boot.
// The privacy latch. Checked HERE rather than in the command, because every
// path that needs the chip comes through this one — including the background
// join at boot, which would otherwise bring the radio up behind a lock the user
// set deliberately. A switch that only stops the polite callers is not a switch.
bool radio_locked(void) {
    return strcmp(reg_get("System.RadioLock", "false"), "true") == 0;
}

static bool radio_up(void) {
    if (radio_locked()) {
        out_err("Radios are locked off. 'radio on' to release the lock.");
        return false;
    }
    if (g_radio_up) return true;
    if (cyw43_arch_init() != 0) {
        out_err("Could not start the wireless chip.");
        return false;
    }
    cyw43_arch_enable_sta_mode();
    g_radio_core = task_this_core();
    g_radio_up = true;
    return true;
}

// The one-owner rule is about TASKS. This is the other half of it: the core.
//
// g_net_op guarantees one task at a time inside the driver, which is not the
// same as one CORE — a task with AFFINITY_ANY can be scheduled onto either, and
// the driver only works on the one that initialised it. Everything that touches
// the network today is pinned to core 0 (the shell, and wifi-join since it
// started binding the chip), so this never fires; it exists because the day a
// package does its own networking from an unpinned task, the failure would
// otherwise be memory corruption with no message attached.
//
// Refusing is the right answer rather than migrating. Moving a running task to
// another core needs scheduler support that does not exist yet, and inventing
// it inside a lock is how the last round of faults happened.
bool net_core_ok(void) {
    return !g_radio_up || task_this_core() == g_radio_core;
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
    // `wifi` is the one command whose whole job is to report the truth right
    // now, so it pays for a live look rather than reading the cache.
    { LockGuard _own(&g_net_op); status_refresh(); }
    int st = g_status.link;
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

static int scan_collect(bool quiet = false) {
    // A scan is a driver operation, so it takes ownership too. Recursive, so a
    // caller that already holds it (autoconnect, which scans then joins) is
    // not blocked by itself.
    LockGuard _own(&g_net_op);
    if (!radio_up()) return 1;
    g_scan.n = 0;
    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof(opts));
    if (!quiet) out_info("Scanning...");
    if (cyw43_wifi_scan(&cyw43_state, &opts, nullptr, scan_cb) != 0) {
        out_err("Could not start a scan.");
        return 1;
    }
    // A scan takes a couple of seconds; bail out rather than hang if the driver
    // never clears the flag.
    absolute_time_t deadline = make_timeout_time_ms(15000);
    uint32_t scan_started = task_now_ms();
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        if (!quiet) out_spinner("Scanning", task_now_ms() - scan_started);
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) break;
        if (intr_check()) break;
        // task_sleep_ms, not sleep_ms: a raw sleep parks the core without
        // reaching the scheduler, so nothing feeds the watchdog. A scan can run
        // for seconds, and the watchdog then asked the SHELL to stop — which
        // set a flag that made every later scan and ping give up instantly.
        task_sleep_ms(20);
    }
    if (!quiet) out_progress_done();
    if (!g_scan.n) { if (!quiet) out_warn("No networks found."); return 1; }

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

static int wifi_connect(const char *ssid, const char *pw, bool quiet = false,
                        bool persist = true);
static int wifi_autoconnect(bool quiet, bool persist = true);

// The last SSID actually joined. Kept outside the registry so a caller that
// deliberately does not write settings can still report what it did.
static char g_last_joined[33];

void net_apply_addressing(void);
void net_apply_dns(void);

// True on success. The boot join uses this: silent, and it does NOT touch the
// registry — the shell records the result afterwards, on its own task, where
// every other registry write in this OS already happens.
static bool wifi_connect_quiet(const char *ssid, const char *pw) {
    return wifi_connect(ssid, pw, /*quiet*/true, /*persist*/false) == 0;
}

static int wifi_connect(const char *ssid, const char *pw, bool quiet, bool persist) {
    // Whoever holds this is the one task allowed inside cyw43 and lwIP.
    LockGuard _own(&g_net_op);
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
        uint32_t started = task_now_ms();
        rc = -1;
        while (true) {
            int st;
            { NetLock _l; st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA); }
            if (st == CYW43_LINK_UP) { rc = 0; break; }
            if (st < 0) break;                       // failed or auth rejected
            if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) break;
            if (intr_check()) break;                 // Ctrl+C during a long join
            // No way to know how far along a join is, so show that it is still
            // going and for how long — which is the honest version of progress.
            if (!quiet) out_spinner("Connecting", task_now_ms() - started);
            task_sleep_ms(50);   // a join takes seconds; polling faster buys nothing
        }
        if (!quiet) out_progress_done();
    }
    if (rc != 0) {
        int st;
        { NetLock _l; st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA); }
        status_refresh();
        if (!quiet) out_err("Could not connect to '%s' — %s.", ssid, link_text(st));
        return 1;
    }
    // Recorded whether or not it is persisted, so a caller that deliberately
    // does not write the registry can still say what it joined.
    snprintf(g_last_joined, sizeof(g_last_joined), "%s", ssid);
    // Static addressing and a chosen resolver are applied at every connect, so
    // they survive a rejoin rather than lasting until the next DHCP lease.
    net_apply_addressing();
    net_apply_dns();
    status_refresh();
    snprintf(g_status.ssid, sizeof(g_status.ssid), "%s", ssid);
    if (persist) {
        reg_set("WiFi.Active", ssid);
        saved_put(ssid, pw);
    }
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
// The boot join runs INLINE, on the shell's task, and that is a correction
// rather than a preference.
//
// It was briefly a background task, to stop the login prompt waiting on it.
// That cannot work with this scheduler: cyw43_arch_lwip_begin blocks the CORE
// on a mutex, and under cooperative scheduling a task that blocks the core on a
// lock held by another task on the same core has stopped the only thing that
// could release it. The result was a deadlock the watchdog cleaned up, and a
// different fault each time depending on where it landed.
//
// The delay it was trying to avoid is mostly gone anyway. The old join read one
// saved SSID and connected blind, so being anywhere else cost the entire join
// timeout — eight seconds of nothing. Scanning first means a network that is
// not there costs a scan rather than a timeout.
//
// Doing this properly needs ONE task owning every cyw43 and lwIP call, with
// everything else asking it for work. That is a real change and belongs in
// daylight rather than bolted on. ARCHITECTURE.md carries it.
static volatile bool g_join_done;
static volatile bool g_join_ok;

// Reported by the SHELL at its prompt, not by the task. Printing from a
// background task lands in the middle of whatever is being typed.
void net_autoconnect_report(void) {
    if (!g_join_done) return;
    g_join_done = false;
    if (g_join_ok) return;             // joining the network it was told to is not news
    out_warnp("wifi", "No saved network joined at boot.");
    out_multi("  'wifi autoconnect' to retry, or 'wifi auto off' to stop looking.");
}

// The join runs as its own task again, and this time it can.
//
// It was pulled back inline because cyw43_arch_lwip_begin blocks the CORE:
// under cooperative scheduling, a task blocking the core on a lock another
// task holds has stopped the only thing that could release it. Four different
// hard faults came out of that.
//
// What changed is that only ONE task is ever inside cyw43 or lwIP now.
// g_net_op enforces it and YIELDS while it waits, and the questions other
// tasks actually ask — "are we online", "what is the address" — read a cached
// struct instead of reaching into lwIP at all. So there is nothing left to
// contend over.
static int autoconnect_task(void *) {
    bool ok = wifi_autoconnect(/*quiet*/true, /*persist*/true) == 0;
    g_join_ok = ok;
    g_join_done = true;
    return ok ? 0 : 1;
}

void net_autoconnect(void) {
    if (strcmp(reg_get("WiFi.Auto", "false"), "true") != 0) return;

    // If the LAST run died joining, do not try again automatically. Without
    // this a fault during the join is a boot loop, and a device that is
    // unusable is far worse than one that is merely offline.
    const BlackBox *prev = bb_previous();
    if (prev && (strcmp(prev->task, "wifi-join") == 0 ||
                 strstr(prev->cmd, "autoconnect") != nullptr)) {
        out_warnp("wifi", "The last boot crashed while connecting — not retrying automatically.");
        out_multi("  'wifi autoconnect' to try by hand, or 'wifi auto off' to stop.");
        log_add(LOG_K_WARN, "wifi: automatic join skipped after a crash in it");
        return;
    }

    g_join_done = false;
    g_join_ok = false;
    // CORE0, not ANY. pico_cyw43_arch_lwip_threadsafe_background records the
    // core it was initialised on and asserts on it in process_under_lock, the
    // low-priority IRQ handler and deinit — asserts that compile out in a
    // release build, so the wrong core does not fail loudly, it just proceeds.
    // g_net_op serialises TASKS, not cores, so an unpinned join could begin a
    // cyw43 call on one core and finish it on the other.
    //
    // radio_up() is lazy, so whichever task first needs the chip is the one
    // that binds the context. Pinning the joiner is what makes that core 0
    // deterministically, matching the shell where every other network command
    // runs.
    if (task_spawn("wifi-join", "(kernel)", autoconnect_task, nullptr,
                   TASK_STACK_NET, AFFINITY_CORE0) >= 0) {
        out_infop("wifi", "Looking for a saved network in the background...");
        return;
    }
    // No room for a task: do it inline rather than not at all.
    out_infop("wifi", "Looking for a saved network...");
    net_autoconnect_now();
}

void net_autoconnect_now(void) {
    if (strcmp(reg_get("WiFi.Auto", "false"), "true") != 0) return;
    // Scan, then join the strongest SAVED network that is actually in range.
    // Quiet: joining the network it was told to join is not news, and
    // wifi_autoconnect reports it itself when nothing is found.
    wifi_autoconnect(/*quiet*/true, /*persist*/true);
}

// --- first-run helper -------------------------------------------------------

bool net_available(void) { return true; }

// True when the interface is up AND has an address — "joined" is not the same as
// "usable", and a command that needs to send a packet cares about the second.
const char *net_active_ssid(void) { return reg_get("WiFi.Active", "(none)"); }

// A plain read of the cache. No lock, no lwIP, safe from any task at any time
// — including while another task is midway through a download.
//
// It reflects the last completed operation rather than this instant. That is
// the right trade: the alternative was reaching into lwIP from five different
// tasks, which is what deadlocked the device.
bool net_is_connected(void) { return g_status.connected; }

static bool net_is_connected_live(void) {
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
static int wifi_autoconnect(bool quiet, bool persist) {
    // Held across the whole scan-then-join, so nothing else touches the driver
    // in between and finds it mid-operation.
    LockGuard _own(&g_net_op);
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
    if (scan_collect(quiet) != 0) return 1;

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
    return wifi_connect(best, pw[0] ? pw : nullptr, quiet, persist);
}

// --- addressing -------------------------------------------------------------
//
// DHCP is the default and right for almost everyone. Static addressing exists
// because "almost" is not "all": a device that has to be reachable at a known
// address, or a network with no DHCP server, or a DNS server that resolves
// names the default one does not.
//
// Settings live in the registry and are applied at every connect, so they
// survive a reboot and a rejoin rather than lasting until the next DHCP lease.

static bool parse_ip(const char *s, ip4_addr_t *out) {
    return s && s[0] && ip4addr_aton(s, out) != 0;
}

// Apply whatever addressing is configured. Called after a join.
void net_apply_addressing(void) {
    if (strcmp(reg_get("Net.Static", "false"), "true") != 0) return;

    ip4_addr_t ip, mask, gw;
    if (!parse_ip(reg_get("Net.IP", ""), &ip) ||
        !parse_ip(reg_get("Net.Mask", "255.255.255.0"), &mask) ||
        !parse_ip(reg_get("Net.Gateway", ""), &gw)) {
        out_warnp("net", "Static addressing is on but incomplete — using DHCP.");
        return;
    }

    NetLock _l;
    if (!netif_default) return;
    // Stop DHCP first, or its next renewal replaces what was just set.
    dhcp_stop(netif_default);
    netif_set_addr(netif_default, &ip, &mask, &gw);
}

void net_apply_dns(void) {
    const char *p = reg_get("Net.DNS", "");
    const char *sec = reg_get("Net.DNS2", "");
    NetLock _l;
    ip_addr_t a;
    if (p[0] && ip4addr_aton(p, ip_2_ip4(&a))) { IP_SET_TYPE(&a, IPADDR_TYPE_V4); dns_setserver(0, &a); }
    if (sec[0] && ip4addr_aton(sec, ip_2_ip4(&a))) { IP_SET_TYPE(&a, IPADDR_TYPE_V4); dns_setserver(1, &a); }
}

static void net_show(void) {
    bool stat = strcmp(reg_get("Net.Static", "false"), "true") == 0;
    out_info("Network configuration");
    out_multi("  Mode      %s", stat ? "static" : "DHCP");

    char ip[20] = "", mask[20] = "", gw[20] = "", d1[20] = "", d2[20] = "";
    bool up = false;
    {
        NetLock _l;
        up = netif_default && netif_is_up(netif_default);
        if (up) {
            snprintf(ip,   sizeof(ip),   "%s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
            snprintf(mask, sizeof(mask), "%s", ip4addr_ntoa(netif_ip4_netmask(netif_default)));
            snprintf(gw,   sizeof(gw),   "%s", ip4addr_ntoa(netif_ip4_gw(netif_default)));
        }
        const ip_addr_t *s0 = dns_getserver(0);
        const ip_addr_t *s1 = dns_getserver(1);
        if (s0) snprintf(d1, sizeof(d1), "%s", ipaddr_ntoa(s0));
        if (s1) snprintf(d2, sizeof(d2), "%s", ipaddr_ntoa(s1));
    }

    out_multi("  Address   %s", up ? ip : "(not connected)");
    if (up) {
        out_multi("  Netmask   %s", mask);
        out_multi("  Gateway   %s", gw);
    }
    out_multi("  DNS       %s%s%s", d1[0] ? d1 : "(none)",
              d2[0] && strcmp(d2, "0.0.0.0") ? ", " : "", d2[0] && strcmp(d2, "0.0.0.0") ? d2 : "");

    if (stat) {
        out_blank();
        out_multi("  Configured: %s / %s via %s",
                  reg_get("Net.IP", "(unset)"), reg_get("Net.Mask", "255.255.255.0"),
                  reg_get("Net.Gateway", "(unset)"));
    }
}

static int cmd_net(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "status")) { net_show(); return 0; }

    if (!strcmp(argv[1], "dhcp")) {
        reg_set("Net.Static", "false");
        persist_save_registry();
        out_ok("Set to DHCP. Reconnect for it to take effect.");
        return 0;
    }

    if (!strcmp(argv[1], "static")) {
        if (argc < 5) {
            out_multi("Usage: net static <address> <netmask> <gateway>");
            out_multi("  e.g.  net static 10.1.1.50 255.255.255.0 10.1.1.1");
            return 1;
        }
        ip4_addr_t t;
        // Validated before saving, so a typo is caught at the prompt rather
        // than becoming a device that will not come back on the network.
        if (!parse_ip(argv[2], &t)) { out_err("'%s' is not an address.", argv[2]); return 1; }
        if (!parse_ip(argv[3], &t)) { out_err("'%s' is not a netmask.", argv[3]); return 1; }
        if (!parse_ip(argv[4], &t)) { out_err("'%s' is not a gateway.", argv[4]); return 1; }
        reg_set("Net.IP", argv[2]);
        reg_set("Net.Mask", argv[3]);
        reg_set("Net.Gateway", argv[4]);
        reg_set("Net.Static", "true");
        persist_save_registry();
        out_ok("Static addressing saved.");
        out_multi("  Applied now, and at every connect from here on.");
        net_apply_addressing();
        return 0;
    }

    if (!strcmp(argv[1], "dns")) {
        if (argc < 3) {
            out_multi("Usage: net dns <server> [second]");
            out_multi("  e.g.  net dns 1.1.1.1 1.0.0.1     |    net dns clear");
            return 1;
        }
        if (!strcmp(argv[2], "clear")) {
            reg_set("Net.DNS", "");
            reg_set("Net.DNS2", "");
            persist_save_registry();
            out_ok("DNS cleared. Reconnect to take whatever DHCP offers.");
            return 0;
        }
        ip4_addr_t t;
        if (!parse_ip(argv[2], &t)) { out_err("'%s' is not an address.", argv[2]); return 1; }
        reg_set("Net.DNS", argv[2]);
        if (argc >= 4) {
            if (!parse_ip(argv[3], &t)) { out_err("'%s' is not an address.", argv[3]); return 1; }
            reg_set("Net.DNS2", argv[3]);
        }
        persist_save_registry();
        net_apply_dns();
        out_ok("DNS set.");
        return 0;
    }

    out_multi("Usage:");
    out_multi("  net status                       what the connection looks like");
    out_multi("  net dhcp                         take the address from the network");
    out_multi("  net static <ip> <mask> <gw>      set it by hand");
    out_multi("  net dns <server> [second]        which resolver to use");
    out_multi("  net dns clear                    go back to the network's own");
    return 1;
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

void net_op_acquire(void) {}
void net_op_release(void) {}
// No radio, so no core to be wrong about. The transport still calls it.
bool net_core_ok(void) { return true; }
bool radio_locked(void) { return false; }   // nothing to lock
void net_autoconnect(void) {}
void net_autoconnect_now(void) {}
void net_autoconnect_report(void) {}
bool net_available(void) { return false; }
bool net_is_connected(void) { return false; }
const char *net_active_ssid(void) { return "(no radio)"; }
int  net_setup_scan_and_join(void) { return 1; }

static int cmd_wifi(int, char **) {
    out_err("This board has no wireless hardware.");
    return 1;
}

// Present so the command exists and explains itself, rather than being absent
// and looking like a feature that was forgotten.
static int cmd_net(int, char **) {
    out_err("This board has no network interface to configure.");
    return 1;
}

#endif

// radio [on|off|status] — the hard stop for every radio.
//
// Persistent on purpose: a privacy switch that forgets itself over a reboot is
// not one. `airplane` is the same command, as it was in v1.
static int cmd_radio(int argc, char **argv) {
    const char *a = argc > 1 ? argv[1] : "status";

    if (strcmp(a, "off") == 0 || strcmp(a, "lock") == 0) {
        reg_set("System.RadioLock", "true");
        persist_save_dirty();
#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI
        radio_down();          // down now, not just refused next time
#endif
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
#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI
        if (radio_locked()) out_warnp("radio", "Locked off.");
        else                out_okp("radio", "Available.");
#else
        out_infop("radio", "This board has no radio.");
#endif
        return 0;
    }

    out_multi("Usage: radio off      lock every radio down (survives a reboot)");
    out_multi("       radio on       release the lock");
    out_multi("       radio status   show the current state");
    return a[0] ? 1 : 0;
}

void net_register(void) {
    static const Command cr{"radio", "lock every radio off, or release it", cmd_radio,
                            nullptr, LEVEL_ADMIN};
    cmd_register(&cr);
    cmd_alias("airplane", "radio");

    static const Command cn{"net", "addressing, DNS and connection detail", cmd_net, nullptr};
    cmd_register(&cn);
    static const Command c{"wifi", "wireless status and connection", cmd_wifi, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
