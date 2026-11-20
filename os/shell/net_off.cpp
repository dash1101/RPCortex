// No Wi-Fi in this image: the stand-in for net.cpp, netapps.cpp, nettcp.cpp,
// httptransport.cpp, pkgfetch.cpp and update_net.cpp.
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
// from-file` and `update rollback`.

#include "command.h"
#include "out.h"
#include "rpc_app.h"
#include "imagefeatures.h"
#include "pkgrepo.h"
#include "radio.h"
#include "update.h"

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

bool http_tls_available(void) { return false; }
int net_pkg_resolve(const char *, char *out, unsigned cap) { if (out && cap) out[0] = 0; return -1; }
int net_pkg_http_get(const char *, void *, unsigned) { return -1; }
int net_pkg_http_download(const char *, const char *) { return -1; }
int net_pkg_http_measure(const char *, uint32_t *, uint32_t *) { return -1; }

// No TLS, so no trusted roots to write at first boot. Not a failure.
bool stock_install_cacerts(bool) { return true; }

// --- pkg and update: the halves that need a network ----------------------------

static int no_repo(void) {
    feature_missing(FEAT_WIFI);
    out_multi("  A package can still be installed from a file: copy the .pkg over with");
    out_multi("  tools/rpc or the browser tool, then 'pkg install <file>'.");
    return 1;
}

int pkg_fetch_update(void) { return no_repo(); }
int pkg_fetch_install(const char *) { return no_repo(); }
int pkg_fetch_upgrade(void) { return no_repo(); }
int pkg_fetch_certs(int, char **) { return no_repo(); }

static int no_ota(void) {
    feature_missing(FEAT_WIFI);
    out_multi("  'update from-file <image.bin>' applies an image copied onto the device.");
    return 1;
}

int update_check(bool) { return no_ota(); }
int update_online(bool, bool) { return no_ota(); }

// --- the commands --------------------------------------------------------------
//
// Registered with the same names, help lines, levels and aliases as the real
// ones, so `help`, completion and `which` read the same in every image.

static int no_wifi(int, char **) { return feature_missing(FEAT_WIFI); }

void net_register(void) {
    radio_register();          // `radio` and `airplane` (radio.cpp, or radio_off.cpp)

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

void http_register(void) {
    static const Command cr{"runurl", "fetch a script and run it", no_wifi, nullptr,
                            LEVEL_ADMIN};
    cmd_register(&cr);
    static const Command c{"wget", "download a file over HTTP", no_wifi, nullptr};
    static const Command c2{"curl", "fetch a URL and print it (-I headers, -X, -d, -H)", no_wifi, nullptr};
    cmd_register(&c);
    cmd_register(&c2);
}
