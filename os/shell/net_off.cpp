// No Wi-Fi in this image: the stand-in for net.cpp, netapps.cpp, nettcp.cpp and
// the lwIP half of the HTTP client (httptransport.cpp).
//
// Compiled when the image is built without Wi-Fi, which on the plain boards is
// always: they have no radio. Everything else in the OS that asks about the
// network gets a plain "no" (not connected, no address, no signal). The
// commands Wi-Fi brings still exist and say why they cannot work, rather than
// being missing and looking like something forgotten; feature_missing words it
// for this board. The package calls (fw_net_*, fw_http_*, fw_tcp_*) stay in the
// ABI table and fail the way they fail with no connection, so a package that
// uses them still loads.
//
// What still works without a network: `pkg install <file>` (which is how
// tools/rpc and the browser tool put a package on a board), `pkg search` and
// `pkg info` from the catalogue that shipped with the image, and `update
// from-file` and `update rollback`. And with rpc or the browser tool on the
// console, everything the HTTP client does: wget, curl, runurl, `pkg install
// <name>`, `pkg update`, `update check` and `update install` are in every image
// (httpclient.cpp, pkgfetch.cpp, update_net.cpp) and go through the bridge
// (bridge.cpp, LINK.md protocol 3), which this image has instead of a network.

#include "command.h"
#include "out.h"
#include "rpc_app.h"
#include "imagefeatures.h"
#include "pkgrepo.h"
#include "radio.h"
#include "update.h"
#include "httproute.h"

#include <stdint.h>

// --- what the rest of the OS asks (net.cpp) ------------------------------------

// No radio, so no reading — and false rather than a made-up zero, because
// 0 dBm is a valid and extraordinary value, not an absence.
bool net_signal(int *) { return false; }

// Likewise unknown. A board without the part reads VBUS off an ordinary pin,
// and api.cpp does that itself rather than asking here. On a wireless board
// built without Wi-Fi, VBUS is behind the radio chip and goes unread.
int net_power_source(void) { return -1; }
const char *signal_bars(int) { return "____"; }
const char *signal_word(int) { return "no radio"; }
int  net_pkg_scan(FwNetAp *, unsigned) { return -1; }
int  net_pkg_scan2(FwNetAp2 *, unsigned) { return -1; }
int  net_pkg_ssid(char *out, unsigned cap) { if (out && cap) out[0] = 0; return 0; }
int  net_pkg_ip(char *out, unsigned cap)   { if (out && cap) out[0] = 0; return 0; }
void net_autoconnect(void) {}
void net_autoconnect_now(void) {}
void net_autoconnect_report(void) {}
bool net_available(void) { return false; }
bool net_is_connected(void) { return false; }
const char *net_active_ssid(void) { return "(no radio)"; }
int  net_setup_scan_and_join(void) { return 1; }


// --- the package calls (netapps.cpp, nettcp.cpp, httptransport.cpp) ------------

int net_pkg_ping(const char *, uint32_t) { return -1; }

int net_pkg_tcp_listen(unsigned) { return -1; }
int net_pkg_tcp_accept(int, uint32_t) { return -1; }
int net_pkg_tcp_recv(int, void *, unsigned, uint32_t) { return -1; }
int net_pkg_tcp_send(int, const void *, unsigned) { return -1; }
int net_pkg_tcp_close(int) { return -1; }
extern "C" void net_tcp_task_ended(int) {}
unsigned net_pkg_tcp_open(void) { return 0; }

// No DNS without a network; the bridge carries HTTP, not lookups.
int net_pkg_resolve(const char *, char *out, unsigned cap) { if (out && cap) out[0] = 0; return -1; }

// --- the HTTP client's network half (httptransport.cpp) -------------------------
//
// Never a connection of its own, so http_route (httpclient.cpp) always moves on
// to the bridge, and never TLS: over the bridge the host checks certificates.
bool http_net_transport_get(HttpTransport *) { return false; }
void http_net_last_detail(char *out, unsigned cap) { if (out && cap) out[0] = 0; }
void http_net_set_timeout_ms(uint32_t) {}
void http_net_no_route(void) { feature_missing(FEAT_WIFI); }
bool http_tls_available(void) { return false; }
const char *http_tls_why(void) { return "this image has no TLS of its own"; }

// No TLS, so no trusted roots to write at first boot. Not a failure.
bool stock_install_cacerts(bool) { return true; }

// `pkg certs`: there is no trust store here to show or repair.
int pkg_fetch_certs(int, char **) {
    out_info("Trusted roots");
    out_multi("  This image has no TLS of its own, so it keeps no certificates.");
    out_multi("  Through the bridge (rpc or the browser tool on the console), the");
    out_multi("  computer fetches HTTPS and checks it against its own trust store.");
    return 0;
}

// --- the commands --------------------------------------------------------------
//
// Registered with the same names, help lines, levels and aliases as the real
// ones, so `help`, completion and `which` read the same in every image.

static int no_wifi(int, char **) { return feature_missing(FEAT_WIFI); }

void bridge_start(void);       // bridge.cpp: the console's input door

void net_register(void) {
    radio_register();          // `radio` and `airplane` (radio.cpp, or radio_off.cpp)
    bridge_start();

    static const Command cn{"net", "addressing, DNS and connection detail", no_wifi, nullptr};
    cmd_register(&cn);
    static const Command c{"wifi", "wireless status and connection", no_wifi, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}

void netapps_register(void) {
    static const Command cmds[] = {
        {"ping",     "ping <host> [count]",     no_wifi, nullptr},
        {"nslookup", "nslookup <host>",         no_wifi, nullptr},
        {"ntp",      "sync the clock over NTP", no_wifi, nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
    cmd_alias("ns", "nslookup");
}
