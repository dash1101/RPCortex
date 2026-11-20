// TCP for packages — listen, accept, read, write, close.
//
// The five calls a server is built from, and the reason httpd is a package
// rather than another thing compiled into the firmware. A LAN scanner and the
// Nova D1's networking want the same five, which is why this is a socket
// interface and not an HTTP one.
//
// CONFIRMED on a Pico 2 W: the httpd package serves a directory listing and a
// site root through these five calls, to a real browser.
//
// --- handles, not pointers --------------------------------------------------
//
// A package never sees a tcp_pcb. It gets a small integer that is looked up in
// the table below and range-checked on every call, so the worst a package can
// do with a bad one is get -1 back. Handing out a pointer would make the whole
// pointer-checking layer pointless: the package would be passing the firmware
// an address to dereference, which is exactly what ptrcheck exists to stop.
//
// A handle also carries a GENERATION. Slots are reused, and without it a
// package that closed a socket and kept using the old number would be talking
// to whoever got the slot next — silently, and only under load.
//
// --- what is deliberately not here ------------------------------------------
//
// No outbound connect: fw_http_get already covers fetching, with TLS and
// certificate verification, and a raw client socket would be a second way to do
// it with none of that. No TLS server, because a server certificate has to come
// from somewhere and nothing on the device can issue one.
//
// --- ownership, and the wedge that came of not having it ---------------------
//
// This file used to say the one-owner network lock was deliberately not taken:
// it is held for the length of an operation, and for a server that is its whole
// lifetime, so taking it would stop every other network command until the
// server shut down. The reasoning was right and the conclusion was wrong,
// because it left the lwIP lock as the only thing standing between two tasks —
// and that lock cannot do the job. It is keyed on the CORE.
//
// The consequences are in netown.h, in full. In short, a package task reaching
// fw_tcp_* runs with no affinity, which on this chip means core 1 almost
// always, and from there cyw43_arch_lwip_begin() is a __wfe loop with no yield
// and no timeout the moment core 0 is inside the driver. That is the shape that
// took a third of all cold boots as #98.
//
// So ownership IS taken, per CALL rather than per socket. That keeps the
// objection above answered — a listening server holds nothing between calls, so
// `wifi scan` and `pkg install` still work while httpd is up — while making
// every entry into lwIP happen under the lock and on the radio's core. The
// scope is the same one the lwIP lock already had; only the lock above it is
// new.
//
// What it costs: a call arriving on core 1 is migrated to core 0 and migrated
// back on release, which is a handful of yields. Measured on a Pico 2 W, an
// accept loop ticking every 250 ms and a receive loop ticking every 200 ms do
// not notice it.
#include "command.h"
#include "out.h"
#include "task.h"
#include "interrupt.h"
#include "netown.h"

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"

bool net_is_connected(void);

#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

// Scoped lwIP lock. Every function here has more than one exit.
//
// Never on its own: a NetOwn has to be in scope above it. The constructor says
// so if one is not, rather than trusting a comment to be read.
struct LwipLock {
    LwipLock()  { if (!net_op_held()) net_unowned_note(); cyw43_arch_lwip_begin(); }
    ~LwipLock() { cyw43_arch_lwip_end(); }
};

// Six is two listeners and four connections, or one server with five clients.
// More would cost lwIP's PCB pool rather than this table, and that pool is the
// real limit.
#define TCP_SLOTS    6
// How many connections may wait to be accepted. A server that is busy serving
// should not be holding a queue: two is enough to cover the gap between finishing
// one request and asking for the next.
#define TCP_BACKLOG  2

struct TcpSlot {
    bool     used;
    bool     listening;
    uint16_t gen;                  // bumped on free, so a stale handle misses
    int      owner;                // task slot, so a dead package's sockets close
    struct tcp_pcb *pcb;

    // Listener: connections the accept callback has taken but nobody has asked
    // for yet. Each is already a full connection slot with its callbacks set
    // (see on_accept), so nothing that arrives before the accept is lost.
    struct TcpSlot *pending[TCP_BACKLOG];
    uint8_t  npending;

    // Connection: the received data, held as lwIP's own pbufs rather than copied
    // into a ring.
    //
    // The same choice httptransport.cpp arrived at, for the same reason plus
    // one: a ring costs static RAM per slot whether or not anything is
    // connected, and bytes can be acknowledged as the reader CONSUMES them so
    // the window reflects what has actually been dealt with rather than what
    // has been buffered.
    struct pbuf *rx;
    volatile bool closed;          // peer sent FIN
    volatile bool failed;
    volatile bool connected;       // an outbound connection was accepted
};

static TcpSlot g_slots[TCP_SLOTS];
static void slot_release(TcpSlot *s);

// --- handles -----------------------------------------------------------------
//
// index in the low bits, generation above, and never zero or negative so a
// handle can never be confused with an error return.
#define H_MAKE(i, g)  (int)((((uint32_t)(g) & 0x7FFFu) << 8) | ((uint32_t)(i) & 0xFFu) | 0x800000u)
#define H_INDEX(h)    (int)((uint32_t)(h) & 0xFFu)
#define H_GEN(h)      (uint16_t)((((uint32_t)(h)) >> 8) & 0x7FFFu)

static TcpSlot *slot_of(int handle, bool want_listener) {
    if (handle <= 0) return nullptr;
    if (((uint32_t)handle & 0x800000u) == 0) return nullptr;
    int i = H_INDEX(handle);
    if (i < 0 || i >= TCP_SLOTS) return nullptr;
    TcpSlot *s = &g_slots[i];
    if (!s->used || s->gen != H_GEN(handle)) return nullptr;
    if (s->listening != want_listener) return nullptr;
    return s;
}

// Caller holds the lwIP lock.
static TcpSlot *slot_claim(void) {
    for (int i = 0; i < TCP_SLOTS; i++) {
        if (g_slots[i].used) continue;
        TcpSlot *s = &g_slots[i];
        uint16_t g = (uint16_t)((s->gen + 1) & 0x7FFF);
        memset(s, 0, sizeof(*s));
        s->used  = true;
        s->gen   = g;
        s->owner = task_slot_index();
        return s;
    }
    return nullptr;
}

static int slot_handle(const TcpSlot *s) {
    return H_MAKE((int)(s - g_slots), s->gen);
}

// Caller holds the lwIP lock. Tears the slot down completely.
static void slot_release(TcpSlot *s) {
    if (!s->used) return;
    if (s->listening) {
        // Anything queued but never accepted is aborted, not leaked: the peer
        // is owed an answer either way, and a reset is the honest one.
        for (int i = 0; i < s->npending; i++)
            if (s->pending[i]) slot_release(s->pending[i]);
        s->npending = 0;
        if (s->pcb) tcp_close(s->pcb);
    } else {
        if (s->rx) { pbuf_free(s->rx); s->rx = nullptr; }
        if (s->pcb) {
            // The callbacks reference this slot, so they go first. Without
            // that, a packet already in flight arrives after the slot has been
            // reused and is filed against whoever owns it now.
            tcp_arg(s->pcb, nullptr);
            tcp_recv(s->pcb, nullptr);
            tcp_err(s->pcb, nullptr);
            tcp_sent(s->pcb, nullptr);
            // close can fail when memory is tight; abort then, because leaving
            // it half-open holds a PCB nobody will ever collect.
            if (tcp_close(s->pcb) != ERR_OK) tcp_abort(s->pcb);
        }
    }
    s->pcb  = nullptr;
    s->used = false;
}

// --- lwIP callbacks (background context) -------------------------------------

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    TcpSlot *s = (TcpSlot *)arg;
    (void)pcb;
    if (!s || !s->used) { if (p) pbuf_free(p); return ERR_OK; }
    if (err != ERR_OK) { s->failed = true; if (p) pbuf_free(p); return ERR_OK; }
    if (!p) { s->closed = true; return ERR_OK; }     // clean FIN

    // Taken, always. Acknowledged in fw_tcp_recv as the reader consumes it.
    if (s->rx) pbuf_cat(s->rx, p);
    else       s->rx = p;
    return ERR_OK;
}

static void on_err(void *arg, err_t e) {
    TcpSlot *s = (TcpSlot *)arg;
    (void)e;
    if (!s || !s->used) return;
    s->pcb    = nullptr;       // lwIP has already freed it; never touch it again
    s->failed = true;
}

// A connection gets its slot and its callbacks HERE, in lwIP's accept, not
// when a task gets round to accepting it. Until a pcb has a recv callback,
// lwIP's default (tcp_recv_null) throws away whatever arrives; an SSH client
// sends its version line the moment it connects, so the server lost it, read
// the client's next packet as a version line and hung up ("version line too
// long"). And a queued bare pcb that was reset was freed by lwIP while the
// queue still pointed at it. A slot buffers the early data and records the
// reset; the task that accepts it takes it over.
static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    TcpSlot *lsn = (TcpSlot *)arg;
    if (!lsn || !lsn->used || err != ERR_OK || !newpcb) return ERR_VAL;
    if (lsn->npending >= TCP_BACKLOG) return ERR_MEM;   // lwIP refuses it for us
    TcpSlot *conn = slot_claim();
    if (!conn) return ERR_MEM;                          // no slot: refused the same way
    conn->listening = false;
    conn->owner = lsn->owner;      // the listener's until a task accepts it
    conn->pcb = newpcb;
    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, on_recv);
    tcp_err(newpcb, on_err);
    lsn->pending[lsn->npending++] = conn;
    return ERR_OK;
}

// Caller holds the lwIP lock. The oldest waiting connection, now the
// caller's, or null when none is waiting.
static TcpSlot *pending_take(TcpSlot *lsn) {
    if (!lsn->npending) return nullptr;
    TcpSlot *conn = lsn->pending[0];
    for (int i = 1; i < lsn->npending; i++) lsn->pending[i - 1] = lsn->pending[i];
    lsn->npending--;
    conn->owner = task_slot_index();
    return conn;
}

// --- the ABI -----------------------------------------------------------------

int net_pkg_tcp_listen(unsigned port) {
    if (!net_is_connected() || port == 0 || port > 65535) return -1;

    NetOwn own;
    if (!own.ok) return -1;
    LwipLock lk;
    TcpSlot *s = slot_claim();
    if (!s) return -1;
    s->listening = true;

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) { s->used = false; return -1; }
    if (tcp_bind(pcb, IP_ANY_TYPE, (u16_t)port) != ERR_OK) {
        tcp_abort(pcb);
        s->used = false;
        return -1;                                  // the port is already taken
    }
    // tcp_listen REPLACES the pcb and frees the old one. Using the original
    // afterwards is a use-after-free that works right up until it does not.
    struct tcp_pcb *lp = tcp_listen(pcb);
    if (!lp) { tcp_abort(pcb); s->used = false; return -1; }

    s->pcb = lp;
    tcp_arg(lp, s);
    tcp_accept(lp, on_accept);
    return slot_handle(s);
}

int net_pkg_tcp_accept(int listener, uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    for (;;) {
        {
            // Inside the loop, so the wait between polls happens with the
            // network free. Holding it across the sleep would let a server with
            // a 250 ms accept tick own the radio essentially all the time.
            NetOwn own;
            if (!own.ok) return -1;
            LwipLock lk;
            TcpSlot *lsn = slot_of(listener, true);
            if (!lsn) return -1;
            TcpSlot *conn = pending_take(lsn);
            if (conn) return slot_handle(conn);
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return -2;  // timed out
        if (intr_check()) return -2;
        // Yield, never sleep: a raw sleep parks the core without reaching the
        // scheduler, so nothing feeds the watchdog.
        task_sleep_ms(5);
    }
}

int net_pkg_tcp_recv(int handle, void *buf, unsigned cap, uint32_t timeout_ms) {
    if (!buf || cap == 0) return -1;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    for (;;) {
        {
            NetOwn own;
            if (!own.ok) return -1;
            LwipLock lk;
            TcpSlot *s = slot_of(handle, false);
            if (!s) return -1;
            if (s->failed) return -1;
            if (s->rx) {
                uint16_t have = s->rx->tot_len;
                uint16_t take = (uint16_t)(cap < have ? cap : have);
                pbuf_copy_partial(s->rx, buf, take, 0);
                s->rx = pbuf_free_header(s->rx, take);
                // Acknowledged as CONSUMED, not as received: the window then
                // describes what has actually been dealt with, so a slow reader
                // slows the sender instead of losing data.
                if (s->pcb) tcp_recved(s->pcb, take);
                return (int)take;
            }
            // Nothing buffered and the peer has finished: an orderly end of
            // stream, which is 0 rather than an error.
            if (s->closed) return 0;
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return -2;
        if (intr_check()) return -2;
        task_sleep_ms(5);
    }
}

int net_pkg_tcp_send(int handle, const void *buf, unsigned len) {
    if (!buf || len == 0) return -1;
    unsigned sent = 0;
    absolute_time_t deadline = make_timeout_time_ms(10000);

    while (sent < len) {
        unsigned chunk = 0;
        {
            NetOwn own;
            if (!own.ok) return sent ? (int)sent : -1;
            LwipLock lk;
            TcpSlot *s = slot_of(handle, false);
            if (!s || s->failed || !s->pcb) return sent ? (int)sent : -1;

            uint16_t room = tcp_sndbuf(s->pcb);
            if (room) {
                chunk = len - sent;
                if (chunk > room) chunk = room;
                // COPY, because the buffer belongs to the package and this call
                // returns before the bytes are on the wire. Referencing it would
                // let a package free or overwrite data lwIP is still sending.
                err_t e = tcp_write(s->pcb, (const uint8_t *)buf + sent, (u16_t)chunk,
                                    TCP_WRITE_FLAG_COPY);
                if (e == ERR_OK) {
                    tcp_output(s->pcb);
                    sent += chunk;
                } else if (e != ERR_MEM) {
                    return sent ? (int)sent : -1;
                } else {
                    chunk = 0;                      // out of buffers: wait and retry
                }
            }
        }
        if (sent >= len) break;
        if (!chunk) {
            if (absolute_time_diff_us(get_absolute_time(), deadline) < 0)
                return sent ? (int)sent : -2;
            if (intr_check()) return sent ? (int)sent : -2;
            task_sleep_ms(5);
        }
    }
    return (int)sent;
}

int net_pkg_tcp_close(int handle) {
    NetOwn own;
    if (!own.ok) return -1;
    LwipLock lk;
    TcpSlot *s = slot_of(handle, false);
    if (!s) s = slot_of(handle, true);
    if (!s) return -1;
    slot_release(s);
    return 0;
}

// A package's task has gone. Anything it left open is closed here, or the
// listener stays bound and the port cannot be used again until a reboot.
//
// Called from task_slot_recycled, which runs on the SPAWN and REAP paths — so
// most of the time it is some unrelated task creating another one, and taking
// the network for that would migrate a caller who never asked to go anywhere.
// Hence the cheap scan first: nothing to do is the normal case, and it costs
// six loads and no lock.
extern "C" void net_tcp_task_ended(int slot) {
    bool any = false;
    for (int i = 0; i < TCP_SLOTS; i++)
        if (g_slots[i].used && g_slots[i].owner == slot) { any = true; break; }
    if (!any) return;

    // Could not get onto the radio's core. The slots stay allocated and the
    // port stays bound until a reboot, which is a leak — and strictly better
    // than tearing lwIP's structures down from the wrong core, which is not a
    // leak but a fault somewhere unrelated, minutes later.
    NetOwn own;
    if (!own.ok) return;
    LwipLock lk;
    for (int i = 0; i < TCP_SLOTS; i++)
        if (g_slots[i].used && g_slots[i].owner == slot) slot_release(&g_slots[i]);
}

// For `netstat`-ish reporting: how many slots are in use.
unsigned net_pkg_tcp_open(void) {
    unsigned n = 0;
    for (int i = 0; i < TCP_SLOTS; i++) if (g_slots[i].used) n++;
    return n;
}

// Compiled only where SSH is: on a Pico W every kilobyte of the slot counts.
#if defined(RPC_HAS_SSH) && RPC_HAS_SSH

// --- for the SSH server and client: one look, never a wait -------------------
//
// The package calls above each wait in a loop, and each turn of the loop
// takes and gives back the network. The SSH server serves a listener and two
// connections from one task, so it wants to ask each socket once and move on;
// and the client has a keyboard to watch as well as a socket. These do exactly
// one check under the same NetOwn rule and return at once.
//
// Firmware only — not in the package ABI. They also do not call intr_check:
// a Ctrl+C belongs to whatever the SSH session is running, never to the
// socket underneath it.

int net_tcp_accept_now(int listener) {
    NetOwn own;
    if (!own.ok) return -1;
    LwipLock lk;
    TcpSlot *lsn = slot_of(listener, true);
    if (!lsn) return -1;
    TcpSlot *conn = pending_take(lsn);
    return conn ? slot_handle(conn) : 0;
}

int net_tcp_read_now(int handle, void *buf, unsigned cap) {
    if (!buf || !cap) return 0;
    NetOwn own;
    if (!own.ok) return -1;
    LwipLock lk;
    TcpSlot *s = slot_of(handle, false);
    if (!s || s->failed) return -1;
    if (s->rx) {
        uint16_t have = s->rx->tot_len;
        uint16_t take = (uint16_t)(cap < have ? cap : have);
        pbuf_copy_partial(s->rx, buf, take, 0);
        s->rx = pbuf_free_header(s->rx, take);
        if (s->pcb) tcp_recved(s->pcb, take);      // acknowledged as consumed
        return (int)take;
    }
    return s->closed ? -1 : 0;
}

int net_tcp_write_now(int handle, const void *buf, unsigned len) {
    if (!buf || !len) return 0;
    NetOwn own;
    if (!own.ok) return -1;
    LwipLock lk;
    TcpSlot *s = slot_of(handle, false);
    if (!s || s->failed || !s->pcb) return -1;
    uint16_t room = tcp_sndbuf(s->pcb);
    unsigned chunk = len < room ? len : room;
    if (!chunk) return 0;
    err_t e = tcp_write(s->pcb, buf, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
    if (e == ERR_MEM) return 0;                    // out of buffers: try again later
    if (e != ERR_OK) return -1;
    tcp_output(s->pcb);
    return (int)chunk;
}

// Has everything written been acknowledged by the peer? Before a close, so a
// DISCONNECT or the last of a command's output is not cut off by the reset an
// abort sends.
bool net_tcp_drained(int handle) {
    NetOwn own;
    if (!own.ok) return true;
    LwipLock lk;
    TcpSlot *s = slot_of(handle, false);
    if (!s || !s->pcb || s->failed) return true;
    return tcp_sndqueuelen(s->pcb) == 0;
}

// The address at the other end, for a log line.
void net_tcp_peer(int handle, char *out, unsigned cap) {
    if (!out || !cap) return;
    out[0] = 0;
    NetOwn own;
    if (!own.ok) return;
    LwipLock lk;
    TcpSlot *s = slot_of(handle, false);
    if (s && s->pcb) snprintf(out, cap, "%s", ipaddr_ntoa(&s->pcb->remote_ip));
}

// Make the calling task the owner of a socket, so the socket is closed if THIS
// task ends rather than the one that accepted it.
void net_tcp_adopt(int handle) {
    NetOwn own;
    if (!own.ok) return;
    LwipLock lk;
    TcpSlot *s = slot_of(handle, false);
    if (s) s->owner = task_slot_index();
}

// An outbound connection. lwIP's raw API, the way the slots above use it, with
// the name resolved by the one resolver net.cpp owns.
static err_t on_connected_slot(void *arg, struct tcp_pcb *, err_t err) {
    TcpSlot *s = (TcpSlot *)arg;
    if (!s || !s->used) return ERR_OK;
    if (err != ERR_OK) s->failed = true;
    else s->connected = true;
    return ERR_OK;
}

bool net_resolve(const char *host, ip_addr_t *out);   // net.cpp

int net_tcp_connect(const char *host, unsigned port, uint32_t timeout_ms) {
    if (!host || !port || port > 65535 || !net_is_connected()) return -1;
    ip_addr_t addr;
    {
        NetOwn own;
        if (!own.ok) return -1;
        if (!net_resolve(host, &addr)) return -1;
    }
    int handle;
    {
        NetOwn own;
        if (!own.ok) return -1;
        LwipLock lk;
        TcpSlot *s = slot_claim();
        if (!s) return -1;
        struct tcp_pcb *pcb = tcp_new_ip_type(IP_GET_TYPE(&addr));
        if (!pcb) { s->used = false; return -1; }
        s->pcb = pcb;
        tcp_arg(pcb, s);
        tcp_recv(pcb, on_recv);
        tcp_err(pcb, on_err);
        if (tcp_connect(pcb, &addr, (u16_t)port, on_connected_slot) != ERR_OK) {
            slot_release(s);
            return -1;
        }
        handle = slot_handle(s);
    }
    // The wait with the network free, as accept does.
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    for (;;) {
        {
            NetOwn own;
            if (!own.ok) return -1;
            LwipLock lk;
            TcpSlot *s = slot_of(handle, false);
            if (!s || s->failed) { if (s) slot_release(s); return -1; }
            if (s->connected) return handle;
        }
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0 || intr_check()) {
            net_pkg_tcp_close(handle);
            return -1;
        }
        task_sleep_ms(10);
    }
}
#endif  // RPC_HAS_SSH

#else   // no radio on this board

int net_pkg_tcp_listen(unsigned) { return -1; }
int net_pkg_tcp_accept(int, uint32_t) { return -1; }
int net_pkg_tcp_recv(int, void *, unsigned, uint32_t) { return -1; }
int net_pkg_tcp_send(int, const void *, unsigned) { return -1; }
int net_pkg_tcp_close(int) { return -1; }
int  net_tcp_accept_now(int) { return -1; }
int  net_tcp_read_now(int, void *, unsigned) { return -1; }
int  net_tcp_write_now(int, const void *, unsigned) { return -1; }
bool net_tcp_drained(int) { return true; }
void net_tcp_adopt(int) {}
void net_tcp_peer(int, char *out, unsigned cap) { if (out && cap) out[0] = 0; }
int  net_tcp_connect(const char *, unsigned, uint32_t) { return -1; }
extern "C" void net_tcp_task_ended(int) {}
unsigned net_pkg_tcp_open(void) { return 0; }

#endif
