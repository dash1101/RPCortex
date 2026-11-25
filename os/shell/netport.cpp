// The network port: the bridge on its own USB serial interface (LINK.md, "The
// network port").
//
// The console bridge (bridge.cpp) reads the computer's answers out of the
// console's input, which only the console's own shell may read, so it serves
// commands typed there and nothing else. This is the same protocol on a second
// CDC interface that carries nothing but bridge frames, so ANY task may use it:
// a background job, a service, a package's worker, a Nova D1 menu. A computer
// running `rpc bridge` holds it open and answers, and the board is online for as
// long as it is plugged in, on every image (on a Wi-Fi image, when Wi-Fi is not
// joined).
//
// Threading. TinyUSB's state belongs to the `usb` task on core 0 (usbdev.cpp),
// and a fetch can run on any task on either core, so the two never touch the
// same thing: netport_pump, from the usb task, moves bytes between the CDC FIFOs
// and two rings here, and the fetch reads and writes only the rings, under a
// critical section. One fetch at a time: the port is claimed from open to close.

#include "bridge.h"
#include "httproute.h"
#include "logring.h"
#include "task.h"
#include "interrupt.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/sync.h"
#include "tusb.h"

#define NET_ITF   1
#define RX_N      4096           // FHEAD, the FHDR lines and one FDATA, with room
#define TX_N      2048

static uint8_t g_rx[RX_N], g_tx[TX_N];
static volatile uint32_t g_rx_head, g_rx_tail, g_tx_head, g_tx_tail;
static critical_section_t g_cs;
static bool g_cs_ready;

static volatile bool g_dtr;        // the computer has the port open
static volatile bool g_offer;      // and has said it will answer (0x1E 0x1F)
static volatile bool g_fetching;   // a fetch owns what arrives
static volatile int  g_owner;      // the task holding the port, 0 when free

static BridgeConn    g_conn;
static HttpTransport g_inner;
static bool          g_ready;

static void cs_enter(void) { if (g_cs_ready) critical_section_enter_blocking(&g_cs); }
static void cs_exit(void)  { if (g_cs_ready) critical_section_exit(&g_cs); }

static uint32_t rx_used(void) { return (g_rx_head - g_rx_tail) % RX_N; }
static uint32_t tx_used(void) { return (g_tx_head - g_tx_tail) % TX_N; }

// --- the usb task's side -------------------------------------------------------------

static const char kAnswer[] = "\x1e" "BRIDGE on chunk=768\n";

static void tx_put_locked(const char *d, uint32_t n) {
    for (uint32_t i = 0; i < n && tx_used() < TX_N - 1; i++) {
        g_tx[g_tx_head] = (uint8_t)d[i];
        g_tx_head = (g_tx_head + 1) % TX_N;
    }
}

// Not fetching: what arrives is either an offer or something stray (the tail of
// a fetch that was given up on). Both are consumed here.
static uint8_t g_door;             // 1 after a 0x1E
static void door_locked(void) {
    while (rx_used()) {
        uint8_t c = g_rx[g_rx_tail];
        g_rx_tail = (g_rx_tail + 1) % RX_N;
        if (g_door && c == 0x1F) {
            if (!g_offer) log_add(LOG_K_INFO, "network port: a computer is offering its connection");
            g_offer = true;
            tx_put_locked(kAnswer, sizeof(kAnswer) - 1);
        }
        g_door = (c == 0x1E);
    }
}

void netport_pump(void) {
    if (!g_cs_ready) { critical_section_init(&g_cs); g_cs_ready = true; }
    if (!tud_ready()) return;
    uint8_t tmp[64];
    // In: from the CDC FIFO into the ring, while there is room.
    for (;;) {
        cs_enter();
        uint32_t room = RX_N - 1 - rx_used();
        cs_exit();
        if (!room || !tud_cdc_n_available(NET_ITF)) break;
        uint32_t n = tud_cdc_n_read(NET_ITF, tmp, room < sizeof(tmp) ? room : sizeof(tmp));
        if (!n) break;
        cs_enter();
        for (uint32_t i = 0; i < n; i++) { g_rx[g_rx_head] = tmp[i]; g_rx_head = (g_rx_head + 1) % RX_N; }
        cs_exit();
    }
    cs_enter();
    if (!g_fetching) door_locked();
    cs_exit();
    // Out: from the ring into the CDC FIFO, while it takes it.
    bool wrote = false;
    for (;;) {
        uint32_t space = tud_cdc_n_write_available(NET_ITF);
        if (!space) break;
        cs_enter();
        uint32_t n = 0;
        while (n < sizeof(tmp) && n < space && tx_used()) {
            tmp[n++] = g_tx[g_tx_tail];
            g_tx_tail = (g_tx_tail + 1) % TX_N;
        }
        cs_exit();
        if (!n) break;
        tud_cdc_n_write(NET_ITF, tmp, n);
        wrote = true;
    }
    if (wrote) tud_cdc_n_write_flush(NET_ITF);
}

// The computer opened or closed the port (DTR). Closing it withdraws the offer:
// whatever opens it next has said nothing yet.
void netport_line_state(bool dtr) {
    g_dtr = dtr;
    if (!dtr) {
        if (g_offer) log_add(LOG_K_INFO, "network port: closed by the computer");
        g_offer = false;
    }
}

// --- the fetch's side ----------------------------------------------------------------

static void io_write(void *, const char *data, uint32_t len) {
    uint32_t start = task_now_ms();
    while (len) {
        cs_enter();
        while (len && tx_used() < TX_N - 1) {
            g_tx[g_tx_head] = (uint8_t)*data++;
            g_tx_head = (g_tx_head + 1) % TX_N;
            len--;
        }
        cs_exit();
        if (!len) break;
        // The usb task drains it; a computer that stopped reading is caught by
        // the protocol's own timeouts, but a frame is never left half-written
        // here for ever.
        if (!g_dtr || task_now_ms() - start > 5000) return;
        task_sleep_ms(1);
    }
}

static int io_getc(void *) {
    int c = -1;
    cs_enter();
    if (rx_used()) { c = g_rx[g_rx_tail]; g_rx_tail = (g_rx_tail + 1) % RX_N; }
    cs_exit();
    return c;
}

static uint32_t io_now(void *) { return task_now_ms(); }
static bool io_stop(void *) { return intr_pending() || task_should_stop(); }
static void io_idle(void *) { task_sleep_ms(1); }
static void io_lost(void *) {
    if (g_offer) log_add(LOG_K_WARN, "network port: the computer stopped answering; offer withdrawn");
    g_offer = false;
}

static void ensure_ready(void) {
    if (g_ready) return;
    BridgeIo io{io_write, io_getc, io_now, io_stop, io_idle, io_lost, nullptr};
    bridge_init(&g_conn, &io);
    bridge_transport(&g_conn, &g_inner);
    g_ready = true;
}

// One fetch at a time: the port is claimed for a hop and given back after.
static bool claim(void) {
    int me = task_self();
    uint32_t start = task_now_ms();
    for (;;) {
        cs_enter();
        bool got = g_owner == 0;
        if (got) { g_owner = me ? me : -1; g_fetching = true; g_rx_tail = g_rx_head; }
        cs_exit();
        if (got) return true;
        if (task_now_ms() - start > 30000 || task_should_stop()) return false;
        task_sleep_ms(5);
    }
}

static void release(void) {
    cs_enter();
    g_fetching = false;
    g_owner = 0;
    cs_exit();
}

static int t_open(void *ctx, const char *host, uint16_t port, bool tls) {
    if (!claim()) return -1;
    int r = g_inner.open(ctx, host, port, tls);
    if (r != 0) release();
    return r;
}

static void t_close(void *ctx) {
    g_inner.close(ctx);
    release();
}

bool netport_offered(void) { return g_dtr && g_offer; }

// The USB host changed DTR on one of the two serial interfaces. TinyUSB calls
// this from the usb task (usbdev.cpp); the SDK does not define it. Interface 0
// is the console, whose bridge offer it ends (bridge.cpp, or nothing on an image
// with Wi-Fi); interface 1 is this port.
void bridge_console_line_state(bool dtr);
extern "C" void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;
    if (itf == NET_ITF) netport_line_state(dtr);
    else bridge_console_line_state(dtr);
}

bool netport_transport_get(HttpTransport *t) {
    if (!netport_offered()) return false;
    ensure_ready();
    *t = g_inner;
    t->open = t_open;
    t->close = t_close;
    return true;
}

const char *netport_last_detail(void) {
    const char *d = bridge_detail(&g_conn);
    return d[0] ? d : "no detail from the computer";
}
