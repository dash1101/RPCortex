// The network bridge on the console (LINK.md, "The network bridge").
//
// Built only into images without Wi-Fi, next to net_off.cpp: an image with a
// network of its own never asks the host for one (httptransport.cpp says no for
// it). core/bridge.cpp is the protocol; this connects it to the console and
// decides when it may be used.
//
// When: a host has offered it (link's BRIDGE on, or 0x1E 0x1F from a terminal),
// the offer still stands (the port has not been closed since), and the request
// comes from the console's own shell, the one task that reads the console. A
// background job, a service, a package's worker or a remote login reading it
// would be taking the console's bytes, which is the bug interrupt.cpp's rules
// exist to prevent; they get "no network" and a line saying why.
//
// Reading happens through intr_getkey: the stash first, then the wire, in order,
// so whatever intr_check set aside while the fetch polled for Ctrl+C comes back
// in place. NO InputClaim here: inside link mode the claim is link's, and a claim
// is per task, so taking and releasing one here would release link's.
//
// DEVICE-UNCONFIRMED: everything here. bridge_test drives the protocol against a
// fake host and the rpc and browser suites drive the hosts against a mock device;
// the line-state callback, the door on the real console and the speed over USB
// need a board.

#include "bridge.h"
#include "httproute.h"
#include "imagefeatures.h"
#include "interrupt.h"
#include "logring.h"
#include "out.h"
#include "task.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

// One fetch at a time, and only ever from the shell's task, which is the only
// one allowed to read the console: so one connection, kept here, is all there
// can be. Its working memory is allocated per hop (core/bridge.cpp).
static BridgeConn g_conn;
static bool       g_ready;
static HttpTransport g_inner;

static BridgeOffer g_offer;         // core/bridge.cpp decides; this keeps it
static BridgeDoor g_door;
static volatile bool g_reading;     // a fetch is reading the host's lines: the door stands aside

// --- the console, for core/bridge.cpp --------------------------------------------

static void io_write(void *, const char *data, uint32_t len) {
    out_console(data, len);          // never into an EXEC's capture
    stdio_flush();
}

// The host's lines are read a buffer at a time (intr_getkeys), not a byte at a
// time: each byte read alone took a trip through the SDK's stdio and the USB
// stack, and receiving a chunk's line took longer than writing the chunk to
// flash. Measured on a Pico 2 W, a 382 KB download went from 16.9 s to 11.0 s.
// What is read and not used when the fetch closes goes back (intr_unget), so
// keys the host sends after the last frame reach the prompt, in order.
static char g_rx[256];
static int  g_rx_n, g_rx_at;

static int io_getc(void *) {
    if (g_rx_at >= g_rx_n) {
        g_rx_at = 0;
        g_rx_n = intr_getkeys(g_rx, (int)sizeof(g_rx));
        if (g_rx_n <= 0) { g_rx_n = 0; return -1; }
    }
    int c = (unsigned char)g_rx[g_rx_at++];
    // A Ctrl+C read here is the fetch's to act on, and the command's too: raise
    // it, so the poll httpfetch makes next reports a cancel, not a lost line.
    if (c == 0x03) intr_raise();
    return c;
}

static void rx_give_back(void) {
    if (g_rx_at < g_rx_n) intr_unget(g_rx + g_rx_at, g_rx_n - g_rx_at);
    g_rx_n = g_rx_at = 0;
}

static uint32_t io_now(void *) { return task_now_ms(); }
static bool io_stop(void *) { return intr_pending() || task_should_stop(); }
// A millisecond, as link mode waits: a yield loop would hold the core at 100%.
static void io_idle(void *) { task_sleep_ms(1); }

static void io_lost(void *) {
    // The host has gone quiet in the middle of a fetch. Whatever it was, it is
    // not answering, and the next command must not send frames at it.
    if (g_offer.on) log_add(LOG_K_WARN, "bridge: the host stopped answering; offer withdrawn");
    bridge_offer_set(&g_offer, false, false);
}

static void ensure_ready(void) {
    if (g_ready) return;
    BridgeIo io{io_write, io_getc, io_now, io_stop, io_idle, io_lost, nullptr};
    bridge_init(&g_conn, &io);
    bridge_transport(&g_conn, &g_inner);
    g_ready = true;
}

// The door stands aside for exactly the length of a hop: from open to close the
// host's lines are the fetch's, and anything arriving outside one is stray.
static int t_open(void *ctx, const char *host, uint16_t port, bool tls) {
    g_reading = true;
    int r = g_inner.open(ctx, host, port, tls);
    if (r != 0) { rx_give_back(); g_reading = false; }
    return r;
}

static void t_close(void *ctx) {
    g_inner.close(ctx);
    rx_give_back();
    g_reading = false;
}

// --- the offer --------------------------------------------------------------------

// The USB host stopped asserting DTR: the port was closed, by the tool that made
// the offer or by anything else. Whatever opens it next has said nothing yet.
// TinyUSB calls this from the USB task (usbdev.cpp); the SDK does not define it.
extern "C" void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)itf; (void)rts;
    bridge_offer_dtr(&g_offer, dtr);
}

static void offer(bool on) {
    if (on && !g_offer.on) log_add(LOG_K_INFO, "bridge: offered by the host on the console");
    bridge_offer_set(&g_offer, on, stdio_usb_connected());
}

bool bridge_available(void) { return true; }

// The callback is the reliable signal; asking about the port as well catches a
// drop it did not report.
bool bridge_offered(void) { return bridge_offer_valid(&g_offer, stdio_usb_connected()); }

bool bridge_set_offer(bool on) { offer(on); return true; }

// Every console key passes through here first (interrupt.h).
static int door(int c) {
    if (g_reading) return c;
    bool offered = false;
    int r = bridge_door(&g_door, c, &offered);
    if (offered) {
        offer(true);
        // The answer that tells a terminal host the bridge is there. It only
        // ever follows the host's own offer, so a terminal that never made one
        // never sees it.
        char f[40];
        int n = snprintf(f, sizeof(f), "\x1e" "BRIDGE on chunk=%d\n", BRIDGE_CHUNK);
        out_console(f, (uint32_t)n);
        stdio_flush();
    }
    return r;
}

void bridge_start(void) { intr_set_key_filter(door); }

// --- who may use it ----------------------------------------------------------------

static bool console_command(void) {
    int shell = task_shell_pid();
    if (!shell || task_self() != shell) return false;
    const TaskShell *ts = task_shell();
    return ts && !ts->term && ts->intr_dom == 0;
}

bool bridge_transport_get(HttpTransport *t) {
    if (!bridge_may_use(&g_offer, stdio_usb_connected(), console_command())) return false;
    ensure_ready();
    *t = g_inner;
    t->open = t_open;
    t->close = t_close;
    return true;
}

const char *bridge_last_detail(void) {
    const char *d = bridge_detail(&g_conn);
    return d[0] ? d : "no detail from the bridge";
}

void bridge_no_route(void) {
    feature_missing(FEAT_WIFI);
    if (bridge_offered() && !console_command()) {
        out_multi("  The computer's connection (the bridge) serves only commands run at the");
        out_multi("  console, not background jobs, services or remote logins.");
    } else {
        out_multi("  With rpc or the browser tool attached, this goes through the computer's");
        out_multi("  connection: 'rpc term', 'rpc exec', or the browser tool's terminal.");
    }
}
