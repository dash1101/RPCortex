// The lwIP side of the HTTP client, and `wget` to exercise it.
//
// core/httpfetch.cpp decides what happens; this only moves bytes. Keeping the
// split that sharp is what let redirects, size caps, truncation and a full
// filesystem all be tested on a host with no radio in it. What is unproven
// until a board runs it is this file, and it is deliberately the small one.
//
// DEVICE-UNCONFIRMED: written against the lwIP raw API and the SDK's
// threadsafe_background locking rules, and compiled for all four boards, but no
// hardware has run it yet.
//
// Two things this has to get right, both of which have bitten elsewhere in this
// tree already:
//
//   * every lwIP call from task context is wrapped in cyw43_arch_lwip_begin/end.
//     Callbacks arrive from a background context that can preempt at any
//     instruction, and the ring below is shared with them.
//   * waits yield to the scheduler rather than sleeping. A raw sleep_ms parks
//     the core without reaching the scheduler, so nothing feeds the watchdog —
//     and an 8 s connect timeout against an 8 s watchdog is a reboot, not a
//     timeout.

#include "command.h"
#include "session.h"
#include "out.h"
#include "httpfetch.h"
#include "interrupt.h"
#include "task.h"
#include "storage.h"
#include "path.h"
#include "blackbox.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"

bool net_is_connected(void);
// Only one task may be inside cyw43 or lwIP at a time — see net.cpp. A
// connection holds that ownership from open to close, so a download and a WiFi
// join can never be part-way through each other.
void net_op_acquire(void);
void net_op_release(void);
bool net_core_ok(void);
static int cmd_wget(int argc, char **argv);
const char *fs_cwd(void);
void http_last_detail(char *out, unsigned cap);
bool http_tls_available(void);       // the shell's working directory (fs.cpp)

#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI

#include "pico/cyw43_arch.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#define CONNECT_MS    10000
#define READ_MS       15000
#define SEND_MS       10000

// --- waiting ----------------------------------------------------------------

// Yield, don't sleep. See the header comment.
static bool wait_flag(volatile bool &flag, uint32_t ms, volatile bool *fail = nullptr) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (!flag) {
        if (fail && *fail) return false;
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
        if (intr_check()) return false;          // Ctrl+C gets out of every wait
        // 10 ms, not 2. This waits on operations measured in SECONDS, and a
        // background task waking five hundred times a second costs the shell a
        // context switch each time — which reads as the shell being choppy for
        // no visible reason.
        task_sleep_ms(10);
    }
    return true;
}

// --- connection state -------------------------------------------------------

struct TcpConn {
    struct altcp_pcb *pcb;

    // The received data, held as lwIP's own pbufs rather than copied into a
    // ring.
    //
    // The ring version refused data it could not fit by returning ERR_MEM,
    // which is correct for raw TCP: lwIP keeps the pbuf and re-delivers it.
    // It is NOT correct under TLS. The plaintext pbuf is produced by the TLS
    // layer, and refusing it does not come back — the transfer simply stops.
    // That is exactly what happened: 918 bytes arrived, the next record did not
    // fit alongside them, and the connection stalled until the read timed out.
    //
    // Holding the chain removes the failure mode rather than making the buffer
    // bigger and hoping. It also removes a copy, and lets flow control be done
    // properly: bytes are acknowledged as the reader CONSUMES them, so the
    // window reflects what has actually been dealt with.
    struct pbuf *rx;             // consumed bytes are freed off the front
    volatile bool connected;
    volatile bool closed;        // peer sent FIN
    volatile bool failed;
    volatile uint32_t unsent;    // bytes still in flight

    // Enough to say WHAT went wrong rather than that something did. "connection
    // lost" covers a refused handshake, a peer that hung up, and a read that
    // timed out, and those need three different next steps.
    volatile int   last_err;     // the lwIP error, when one arrived
    volatile bool  handshook;    // the connected callback fired
    volatile uint32_t sent;      // request bytes handed to the stack
    volatile uint32_t got;       // payload bytes received
};

static uint16_t rx_available(const TcpConn *c) {
    return c->rx ? c->rx->tot_len : 0;
}

// --- lwIP callbacks (background context) ------------------------------------

static err_t on_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    TcpConn *c = (TcpConn *)arg;
    (void)pcb;
    if (err != ERR_OK) { c->failed = true; c->last_err = (int)err; if (p) pbuf_free(p); return ERR_OK; }
    if (!p) { c->closed = true; return ERR_OK; }        // clean FIN

    // Take it, always. Never refuse: under TLS a refused pbuf is not
    // re-delivered, and the transfer stops for good.
    //
    // Acknowledged as the reader CONSUMES it, not here. See lw_recv.
    // Read the length BEFORE chaining: after pbuf_cat this pbuf belongs to the
    // chain, and reasoning about its fields separately stops being safe.
    uint16_t n = p->tot_len;
    if (c->rx) pbuf_cat(c->rx, p);
    else       c->rx = p;
    c->got += n;
    return ERR_OK;
}

static void on_err(void *arg, err_t e) {
    TcpConn *c = (TcpConn *)arg;
    c->pcb = nullptr;            // lwIP has already freed it; never touch it again
    c->last_err = (int)e;
    c->failed = true;
}

static err_t on_connected(void *arg, struct altcp_pcb *, err_t err) {
    TcpConn *c = (TcpConn *)arg;
    if (err != ERR_OK) { c->failed = true; return ERR_OK; }
    c->connected = true;
    c->handshook = true;         // for TLS this means the handshake finished
    return ERR_OK;
}

static err_t on_sent(void *arg, struct altcp_pcb *, u16_t len) {
    TcpConn *c = (TcpConn *)arg;
    if (c->unsent >= len) c->unsent -= len; else c->unsent = 0;
    return ERR_OK;
}

// --- DNS --------------------------------------------------------------------

// One implementation, in net.cpp, because the record lwIP writes into has to
// outlive this call — see the note there. This used to be a DnsWait on the
// stack, and a fetch that timed out or was interrupted left lwIP holding its
// address, to be written into long after the frame had gone.
bool net_resolve(const char *host, ip_addr_t *out);        // net.cpp

static bool resolve(const char *host, ip_addr_t *out) {
    return net_resolve(host, out);
}

// --- the HttpTransport implementation ---------------------------------------

// --- trusted roots ----------------------------------------------------------
//
// The roots live in a FILE rather than baked into the image, because
// certificate authorities rotate and a root compiled into firmware means
// reflashing every device the day one expires. /os/ca.pem can be replaced with
// a download.
//
// Loaded once and cached: parsing PEM costs both time and heap, and every
// package install would otherwise pay for it again.
#define CA_PATH  "/os/ca.pem"
#define CA_MAX   8192

static struct altcp_tls_config *g_tls_cfg;
static bool g_tls_tried;
// WHY it failed, so the answer is a step rather than a shrug. "It does not
// work" and "the file is 0 bytes" need very different next actions.
static char g_tls_why[80];

// Returns null when there are no trusted roots. The caller REFUSES the
// connection in that case; it does not fall back to an unverified one.
//
// Turning verification off would make https:// appear to work while providing
// none of what it promises — anything on the path could serve a package and the
// device would install it. A package manager is precisely the wrong place to be
// relaxed about who is on the other end, so this fails closed and says why.
// The roots are BUILT INTO the image, and /os/ca.pem is an override rather than
// the source of truth.
//
// It used to be the other way round, and that made a working HTTPS stack depend
// on a file surviving on littlefs. Delete it, corrupt it, run out of space
// during the first-boot write, and the device silently loses the ability to
// verify anything — which is to say it loses the ability to install a package or
// take an update, the two things that would let you fix it. factoryreset had to
// grow a special case just to avoid erasing it.
//
// So the file is tried first, because someone adding a private root should be
// able to, and the compiled-in bundle catches every case where it is missing or
// unusable. The failure mode goes from "HTTPS is broken and the fix needs a
// reflash" to "your custom roots did not load, the built-in ones did".
extern "C" const unsigned char stock_cacerts_data[];
extern "C" const unsigned int  stock_cacerts_len;

static struct altcp_tls_config *tls_config(void) {
    if (g_tls_tried) return g_tls_cfg;
    g_tls_tried = true;

    uint8_t *pem = nullptr;
    uint32_t n   = 0;

    bool is_dir = false; uint32_t fsize = 0;
    if (storage_stat(CA_PATH, &is_dir, &fsize) && !is_dir && fsize > 0 && fsize < CA_MAX) {
        pem = (uint8_t *)malloc(CA_MAX);
        if (pem) {
            n = storage_read_file(CA_PATH, pem, CA_MAX - 1);
            if (n == 0) { free(pem); pem = nullptr; }
            else        { pem[n] = 0; }     // mbedtls counts the terminator for PEM
        }
    }

    if (!pem) {
        // The built-in bundle. Already NUL-terminated by the generator, and in
        // flash, so it is used in place rather than copied to the heap — which
        // also means this path works when there is no room for a copy.
        g_tls_cfg = altcp_tls_create_config_client(stock_cacerts_data, stock_cacerts_len);
        if (!g_tls_cfg)
            snprintf(g_tls_why, sizeof(g_tls_why),
                     "the built-in certificates would not parse");
        return g_tls_cfg;
    }

    g_tls_cfg = altcp_tls_create_config_client(pem, n + 1);
    free(pem);
    if (g_tls_cfg) return g_tls_cfg;

    // The file was there and unusable. Say so — a private root that silently
    // did nothing is worth knowing about — but carry on with the built-ins
    // rather than leaving the device unable to verify anything.
    out_warnp("certs", "%s did not parse; using the built-in certificates.", CA_PATH);
    g_tls_cfg = altcp_tls_create_config_client(stock_cacerts_data, stock_cacerts_len);
    if (!g_tls_cfg)
        snprintf(g_tls_why, sizeof(g_tls_why), "no certificates could be parsed at all");
    return g_tls_cfg;
}

bool http_tls_available(void) { return tls_config() != nullptr; }

const char *http_tls_why(void) {
    tls_config();
    return g_tls_why[0] ? g_tls_why : "no trusted certificates";
}

// Forget the cached answer, so a repaired bundle takes effect without a reboot.
void http_tls_reset(void) {
    g_tls_tried = false;
    g_tls_cfg = nullptr;
    g_tls_why[0] = 0;
}

// Defined with lw_close, which is the other half of the same idea.
static void conn_detach(TcpConn *c);

static int lw_open(void *ctx, const char *host, uint16_t port, bool tls) {
    TcpConn *c = (TcpConn *)ctx;
    // Taken here and released in lw_close, which the fetch driver always calls
    // — including on every failure path and between redirect hops.
    // A hang inside a fetch used to leave "entered fw_http_get" as the last
    // phase and nothing else, so every stage of a connection had to be ruled out
    // by argument. Each one says so now: the crash report names the last one
    // reached, which is the one that did not finish.
    bb_note_phase("http: waiting for the network lock");
    net_op_acquire();

    // The wireless driver only works on the core that initialised it. Nothing
    // reaches this from the wrong core today; saying so plainly beats finding
    // out through corruption if something ever does.
    if (!net_core_ok()) {
        out_err("Networking is not available from this core.");
        net_op_release();
        return -1;
    }

    struct altcp_tls_config *cfg = nullptr;
    if (tls) {
        bb_note_phase("http: loading roots");
        cfg = tls_config();
        if (!cfg) { net_op_release(); return -1; }   // no roots: refuse, never downgrade
    }

    // Anything still attached from a previous transfer goes now, BEFORE the
    // memset that would orphan it. Every path that abandons a connection is
    // supposed to have detached it already; this is what makes "supposed to"
    // not matter.
    conn_detach(c);
    memset(c, 0, sizeof(*c));

    ip_addr_t addr;
    bb_note_phase("http: resolving");
    if (!resolve(host, &addr)) { net_op_release(); return -1; }

    bb_note_phase(tls ? "http: opening (tls)" : "http: opening");
    cyw43_arch_lwip_begin();
    c->pcb = tls ? altcp_tls_new(cfg, IP_GET_TYPE(&addr))
                 : altcp_tcp_new_ip_type(IP_GET_TYPE(&addr));

    // SNI. Without it a shared host answers with the wrong certificate and the
    // handshake fails for a reason that looks nothing like the cause —
    // raw.githubusercontent.com is exactly such a host.
    if (c->pcb && tls)
        mbedtls_ssl_set_hostname((mbedtls_ssl_context *)altcp_tls_context(c->pcb), host);
    if (c->pcb) {
        altcp_arg(c->pcb, c);
        altcp_recv(c->pcb, on_recv);
        altcp_err(c->pcb, on_err);
        altcp_sent(c->pcb, on_sent);
    }
    err_t e = c->pcb ? altcp_connect(c->pcb, &addr, port, on_connected) : ERR_MEM;
    cyw43_arch_lwip_end();

    // From here on the pcb exists and its callbacks point at `c`, so no exit may
    // simply return: the connection has to be taken apart first. lw_close is not
    // called for an open that failed — the driver only closes what it opened —
    // which is why these say so themselves.
    if (!c->pcb || e != ERR_OK)  { conn_detach(c); net_op_release(); return -1; }
    bb_note_phase(tls ? "http: handshaking" : "http: connecting");
    if (!wait_flag(c->connected, CONNECT_MS, &c->failed)) {
        conn_detach(c);
        net_op_release();
        return -1;
    }
    return 0;
}

static int lw_send(void *ctx, const uint8_t *data, uint32_t len) {
    TcpConn *c = (TcpConn *)ctx;
    bb_note_phase("http: sending");
    uint32_t at = 0;
    absolute_time_t send_deadline = make_timeout_time_ms(SEND_MS);

    while (at < len) {
        if (c->failed || !c->pcb) return -1;

        cyw43_arch_lwip_begin();
        uint32_t room = c->pcb ? altcp_sndbuf(c->pcb) : 0;
        uint32_t n = len - at < room ? len - at : room;
        err_t e = ERR_OK;
        if (n) {
            e = altcp_write(c->pcb, data + at, (u16_t)n, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) { c->unsent += n; e = altcp_output(c->pcb); }
        }
        cyw43_arch_lwip_end();

        if (e == ERR_MEM) { task_sleep_ms(2); continue; }   // transient; retry
        if (e != ERR_OK) { c->last_err = (int)e; return -1; }
        at += n;
        c->sent += n;
        if (n == 0) {
            // No send window. This needs a deadline of its own: without one a
            // stack that never opens the window spins here until the watchdog
            // notices, with nothing to show for it.
            if (intr_check()) return -1;
            if (absolute_time_diff_us(get_absolute_time(), send_deadline) < 0) {
                c->last_err = -100;      // "the send window never opened"
                return -1;
            }
            task_sleep_ms(2);
        }
    }
    return 0;
}

static int lw_recv(void *ctx, uint8_t *buf, uint32_t cap) {
    TcpConn *c = (TcpConn *)ctx;
    bb_note_phase("http: receiving");

    absolute_time_t deadline = make_timeout_time_ms(READ_MS);
    while (rx_available(c) == 0) {
        if (c->failed) return -1;
        if (c->closed) return 0;                 // clean end of body
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            c->last_err = -101;                  // "nothing arrived in time"
            return -1;
        }
        if (intr_check()) return -1;
        task_sleep_ms(2);
    }

    cyw43_arch_lwip_begin();
    uint16_t avail = rx_available(c);
    uint16_t n = (uint16_t)(cap < avail ? cap : avail);
    n = pbuf_copy_partial(c->rx, buf, n, 0);

    // Free what has been read, NOW, rather than when the whole chain is done.
    //
    // Waiting until the chain emptied looked simpler and could not work: more
    // data keeps arriving and being appended, so tot_len grows as fast as the
    // reader advances and the end is never reached. Nothing is freed, the pbuf
    // pool runs dry, and the driver has nowhere to put the next packet — the
    // transfer stops a few hundred bytes in.
    //
    // That is why a 1 KB manifest downloaded fine and a 694 KB image stalled at
    // 921 bytes: a small transfer finishes before the pool matters.
    //
    // pbuf_free_header drops n bytes from the front and frees each pbuf as it
    // is fully consumed, returning the new head. So an offset is no longer
    // needed at all.
    c->rx = pbuf_free_header(c->rx, n);

    // Acknowledge only what has actually been READ. This is TCP's flow control
    // and it has to mean something.
    //
    // Acking on ARRIVAL instead loses it entirely: the sender keeps sending
    // while this device is busy writing flash, the buffers fill, packets get
    // dropped — and the sender believes they were delivered, so it never
    // retransmits. A download then died partway with no error at around 145 KB,
    // which is simply where writing to flash fell far enough behind.
    //
    // This only became workable once TCP_WND was made larger than one TLS
    // record. Before that it deadlocked, which is exactly what made acking on
    // arrival look like the fix. Two bugs concealing each other.
    if (c->pcb && n) altcp_recved(c->pcb, n);

    cyw43_arch_lwip_end();

    return (int)n;
}

// Detach and drop the pcb. Everything that abandons a connection goes through
// here, which is the whole point: the callbacks below reference `c`, and `c` is
// one static reused by every transfer.
//
// Leaving a pcb attached is not a leak that shows up as memory. lw_open memsets
// the connection at the start of the NEXT transfer, so a callback belonging to
// the abandoned one then writes into the live one: on_err clears its pcb,
// on_connected sets its connected flag before it is, on_recv appends somebody
// else's bytes to its buffer. A failed fetch quietly breaks the fetch after it.
//
// Caller must NOT hold the lwIP lock.
static void conn_detach(TcpConn *c) {
    cyw43_arch_lwip_begin();
    if (c->rx) { pbuf_free(c->rx); c->rx = nullptr; }
    if (c->pcb) {
        altcp_arg(c->pcb, nullptr);
        altcp_recv(c->pcb, nullptr);
        altcp_err(c->pcb, nullptr);
        altcp_sent(c->pcb, nullptr);
        // altcp_close can fail when memory is tight; abort then, because leaving
        // a pcb with our arg attached is how a later callback writes into a
        // connection that no longer exists.
        if (altcp_close(c->pcb) != ERR_OK) altcp_abort(c->pcb);
        c->pcb = nullptr;
    }
    cyw43_arch_lwip_end();
}

static void lw_close(void *ctx) {
    bb_note_phase("http: closing");
    conn_detach((TcpConn *)ctx);
    net_op_release();
}

// The connection is big (a 2 KB ring), so it does not live on a task stack.
// One at a time is the right limit here: a shell running two downloads at once
// is not a thing worth the second buffer.
static TcpConn g_conn;

// What the last transfer did, for an error message worth reading.
void http_last_detail(char *out, unsigned cap) {
    const char *why = "";
    switch (g_conn.last_err) {
        case 0:    why = "no error reported"; break;
        case -100: why = "the send window never opened"; break;
        case -101: why = "nothing arrived within the read timeout"; break;
        case -13:  why = "the peer reset the connection"; break;   // ERR_RST
        case -14:  why = "the connection closed"; break;           // ERR_CLSD
        case -11:  why = "the peer aborted"; break;                // ERR_ABRT
        case -1:   why = "out of memory in the network stack"; break;
        default:   why = "lwIP error"; break;
    }
    snprintf(out, cap, "%s; handshake %s, sent %lu, received %lu",
             why, g_conn.handshook ? "ok" : "NOT DONE",
             (unsigned long)g_conn.sent, (unsigned long)g_conn.got);
}

bool http_transport_get(HttpTransport *t) {
    if (!net_is_connected()) return false;
    t->open = lw_open; t->send = lw_send; t->recv = lw_recv; t->close = lw_close;
    t->ctx = &g_conn;
    return true;
}

// --- what packages see -------------------------------------------------------

int net_pkg_resolve(const char *host, char *out, unsigned cap) {
    if (!host || !out || cap == 0) return -1;
    net_op_acquire();
    if (!net_core_ok()) { net_op_release(); return -1; }
    ip_addr_t addr;
    bool ok = resolve(host, &addr);
    net_op_release();
    if (!ok) return -1;
    int n = snprintf(out, cap, "%s", ipaddr_ntoa(&addr));
    return n < 0 ? -1 : n;
}

#else   // no radio on this board

bool http_transport_get(HttpTransport *) { return false; }
bool http_tls_available(void) { return false; }
void http_last_detail(char *out, unsigned cap) { snprintf(out, cap, "no wireless"); }
const char *http_tls_why(void) { return "no wireless on this board"; }
void http_tls_reset(void) {}
// No radio, so nothing to resolve. The HTTP wrappers below are shared and
// already fail through http_transport_get.
int net_pkg_resolve(const char *, char *out, unsigned cap) { if (out && cap) out[0] = 0; return -1; }

#endif

// --- wget -------------------------------------------------------------------

struct FileSink { void *h; };

static int file_sink(void *ctx, const uint8_t *data, uint32_t len) {
    FileSink *f = (FileSink *)ctx;
    // Refusing here is what stops a download that has filled the disk, and the
    // driver turns it into "could not write" rather than "bad response".
    return storage_sink_write(f->h, data, len) ? 0 : -1;
}

static uint64_t g_progress_last;    // reset per download, not per boot

static void show_progress(void *, uint64_t got, uint64_t total) {
    // Every 4 KB: often enough to look continuous, rare enough that drawing it
    // does not slow the transfer it is reporting on.
    static uint64_t last;
    if (got < last) last = 0;
    if (got - last < 4096 && got != total) return;
    last = got;
    out_progress("Downloading", got, total);
}

static int poll_interrupt(void *) { return intr_check() ? 1 : 0; }

// Into a caller's buffer. Bounded by that buffer rather than by a limit of its
// own: a package asking for a page into 4 KB gets at most 4 KB, and the fetch
// stops rather than the buffer overflowing.
struct MemSink { unsigned char *buf; unsigned cap; unsigned len; };

static int mem_sink(void *ctx, const uint8_t *data, uint32_t len) {
    MemSink *m = (MemSink *)ctx;
    uint32_t room = m->cap > m->len ? m->cap - m->len : 0;
    uint32_t take = len < room ? len : room;
    if (take) { memcpy(m->buf + m->len, data, take); m->len += take; }
    return (take < len) ? -1 : 0;      // full: stop, and say the transfer ended
}

int net_pkg_http_get(const char *url, void *buf, unsigned cap) {
    if (!url || !buf || cap == 0) return -1;
    HttpTransport t;
    if (!http_transport_get(&t)) return -1;

    MemSink m{ (unsigned char *)buf, cap, 0 };
    FetchOpts o{};
    o.poll = poll_interrupt;
    o.max_bytes = cap;

    FetchResult r;
    bool good = http_fetch(&t, url, mem_sink, &m, &o, &r);
    // A full buffer stops the transfer, which is not a failure — the caller
    // asked for that much and got it.
    if (!good && m.len < cap) return -1;
    return (int)m.len;
}

// Fetch and THROW AWAY, counting and timing.
//
// A throughput test wants a megabyte off the wire and has nowhere to put it:
// the buffer form is bounded by the buffer, and the file form would need the
// download to fit in flash — 596 KB free against a 1 MB test — and would be
// measuring the filesystem as much as the link. v1 solved it by streaming and
// discarding chunk by chunk, and this is the same idea with the loop already
// written.
//
// The clock starts at the first byte, not at the call: DNS and the TLS
// handshake are latency, and counting them as transfer time makes a fast link
// look slow on a small file.
struct CountSink { uint64_t bytes; uint32_t first_us; };

static int count_sink(void *ctx, const uint8_t *, uint32_t len) {
    CountSink *c = (CountSink *)ctx;
    if (!c->bytes) c->first_us = (uint32_t)(time_us_64() & 0xFFFFFFFFu);
    c->bytes += len;
    return 0;
}

int net_pkg_http_measure(const char *url, uint32_t *bytes, uint32_t *ms) {
    if (!url) return -1;
    HttpTransport t;
    if (!http_transport_get(&t)) return -1;

    CountSink c{0, 0};
    FetchOpts o{};
    o.poll = poll_interrupt;

    FetchResult r;
    bool good = http_fetch(&t, url, count_sink, &c, &o, &r);
    if (!good && c.bytes == 0) return -1;

    uint32_t end_us = (uint32_t)(time_us_64() & 0xFFFFFFFFu);
    uint32_t took   = c.bytes ? (end_us - c.first_us) : 0;
    if (bytes) *bytes = (uint32_t)(c.bytes > 0xFFFFFFFFull ? 0xFFFFFFFFull : c.bytes);
    // Never zero when anything arrived, so a caller dividing by it cannot fault
    // on a transfer that finished inside one microsecond tick.
    if (ms)    *ms    = took / 1000u ? took / 1000u : 1u;
    return (int)(c.bytes > 0x7FFFFFFFull ? 0x7FFFFFFF : (uint32_t)c.bytes);
}

int net_pkg_http_download(const char *url, const char *path) {
    if (!url || !path) return -1;
    HttpTransport t;
    if (!http_transport_get(&t)) return -1;

    FileSink fs{nullptr};
    fs.h = storage_open_sink(path);
    if (!fs.h) return -1;

    FetchOpts o{};
    o.poll = poll_interrupt;
    uint32_t room = storage_free_bytes();
    o.max_bytes = room > 8192 ? room - 8192 : 1;

    FetchResult r;
    bool good = http_fetch(&t, url, file_sink, &fs, &o, &r);
    if (!storage_close_sink(fs.h)) good = false;
    if (!good) { storage_remove(path); return -1; }
    return (int)r.bytes;
}

static int cmd_runurl(int argc, char **argv) {
    if (argc < 2) {
        out_multi("Usage: runurl <url> [--keep] [-y]");
        out_multi("  Downloads a .rps script and runs it.");
        out_multi("  --keep  leave the file behind afterwards");
        out_multi("  -y      do not ask first");
        return 1;
    }

    const char *url = argv[1];
    bool keep = false, yes = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--keep") == 0)                                  keep = true;
        else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) yes = true;
    }

    if (!yes) {
        out_warn("This downloads a script and runs it on this device.");
        out_multi("  %s", url);
        if (!session_confirm("Run it")) { out_info("Cancelled."); return 1; }
    }

    const char *tmp = "/tmp/runurl.rps";
    char *wargv[3] = { (char *)"wget", (char *)url, (char *)tmp };
    if (cmd_wget(3, wargv) != 0) return 1;

    const Command *sc = cmd_resolve("script");
    if (!sc || !sc->fn) {
        out_err("The script interpreter is not available.");
        storage_remove(tmp);
        return 1;
    }
    char *sargv[3] = { (char *)"script", (char *)tmp, nullptr };
    int rc = sc->fn(2, sargv);

    if (!keep) storage_remove(tmp);
    else       out_info("Kept as %s", tmp);
    return rc;
}


// runurl <url> [--keep] [-y]
//
// Fetching and executing a remote script is arbitrary code execution, so the
// confirmation is the feature rather than friction — -y is there for scripts
// that have already decided, not as the shortcut to reach for by default.
//
// The download reuses wget wholesale rather than duplicating the fetch, sink
// and progress handling, and the run hands off to `script` the same way `run`
// does for a .rps. Two copies of either would drift.

static int cmd_wget(int argc, char **argv) {
    if (argc < 2) {
        out_multi("Usage: wget <url> [file]");
        out_multi("  Downloads to the current directory unless a name is given.");
        return 1;
    }

    HttpTransport t;
    if (!http_transport_get(&t)) {
        out_err("No network. Connect with 'wifi connect <ssid>' first.");
        return 1;
    }

    // Say WHY an https URL cannot be fetched. Without the trusted roots the
    // connection is refused rather than made unverified, and "could not
    // connect" would send someone looking at their WiFi for a problem that is
    // a missing file.
    if (!strncmp(argv[1], "https://", 8) && !http_tls_available()) {
        // The image carries its own roots, so reaching here means something is
        // wrong with the build rather than with this device's files.
        out_err("No trusted certificates could be parsed, so HTTPS cannot be verified.");
        out_multi("  The built-in roots should always load; 'diag' has the detail.");
        out_multi("  Unverified HTTPS is not offered: anything on the path could");
        out_multi("  serve a package and it would install.");
        return 1;
    }

    // Name it after the last path segment when the caller did not say.
    char name[64];
    if (argc >= 3) {
        snprintf(name, sizeof(name), "%s", argv[2]);
    } else {
        const char *slash = strrchr(argv[1], '/');
        const char *base = (slash && slash[1]) ? slash + 1 : "download";
        snprintf(name, sizeof(name), "%s", base);
    }

    char full[128];      // the size every other command here uses
    path_resolve(fs_cwd(), name, full, sizeof(full));

    FileSink fs{nullptr};
    fs.h = storage_open_sink(full);
    if (!fs.h) { out_err("Could not create '%s'.", full); return 1; }

    out_info("Fetching %s", argv[1]);
    g_progress_last = 0;    // or a second wget shows nothing until it passes the first

    FetchOpts o{};
    o.poll = poll_interrupt;
    o.progress = show_progress;
    // Never more than the filesystem could hold. Better to refuse at the first
    // byte over than to fill the disk and take the OS down with it.
    uint32_t room = storage_free_bytes();
    o.max_bytes = room > 8192 ? room - 8192 : 1;

    FetchResult r;
    bool good = http_fetch(&t, argv[1], file_sink, &fs, &o, &r);
    // The close is where littlefs commits, so a failure there is a failure of
    // the download however well the transfer itself went.
    if (!storage_close_sink(fs.h) && good) {
        good = false;
        r.error = FETCH_ERR_SINK;
    }
    out_progress_done();

    if (!good) {
        storage_remove(full);        // a partial file is worse than none
        out_err("%s%s%s", fetch_error_str(r.error),
                r.detail[0] ? " - " : "", r.detail);
        return 1;
    }

    out_ok("Saved %s (%llu bytes)", full, (unsigned long long)r.bytes);
    return 0;
}

// --- curl -------------------------------------------------------------------
//
// wget saves; curl prints. That is the whole difference, and it matters because
// printing means the output goes down a PIPE — `curl <url> | grep something`
// works, which is most of what anyone wants a curl for on a device like this.

struct PrintSink { uint64_t n; };

static int print_sink(void *ctx, const uint8_t *data, uint32_t len) {
    PrintSink *p = (PrintSink *)ctx;
    // Straight to the data channel, so a pipe or a redirect sees it and the
    // status tags do not end up in the middle of a JSON document.
    out_write((const char *)data, len);
    p->n += len;
    return 0;
}

static int cmd_curl(int argc, char **argv) {
    const char *url = nullptr;
    const char *save = nullptr;
    bool quiet = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { save = argv[++i]; continue; }
        if (!strcmp(argv[i], "-s")) { quiet = true; continue; }
        if (argv[i][0] != '-') { url = argv[i]; continue; }
    }
    if (!url) {
        out_multi("Usage: curl <url> [-o file] [-s]");
        out_multi("  Prints the body, so it can be piped:  curl <url> | grep name");
        out_multi("  -o writes to a file instead;  -s hides the summary.");
        return 1;
    }

    HttpTransport t;
    if (!http_transport_get(&t)) {
        out_err("No network. Connect with 'wifi connect <ssid>' first.");
        return 1;
    }
    if (!strncmp(url, "https://", 8) && !http_tls_available()) {
        out_err("HTTPS cannot be verified: %s", http_tls_why());
        out_multi("  Try 'pkg certs install'.");
        return 1;
    }

    FetchOpts o{};
    o.poll = poll_interrupt;
    uint32_t room = storage_free_bytes();
    o.max_bytes = save ? (room > 8192 ? room - 8192 : 1) : 0;

    FetchResult r;
    bool good;
    if (save) {
        char full[128];
        path_resolve(fs_cwd(), save, full, sizeof(full));
        FileSink fs{nullptr};
        fs.h = storage_open_sink(full);
        if (!fs.h) { out_err("Could not create '%s'.", full); return 1; }
        good = http_fetch(&t, url, file_sink, &fs, &o, &r);
        if (!storage_close_sink(fs.h) && good) { good = false; r.error = FETCH_ERR_SINK; }
        if (!good) storage_remove(full);
        else if (!quiet) out_ok("Saved %s (%lu bytes)", full, (unsigned long)r.bytes);
    } else {
        PrintSink ps{0};
        good = http_fetch(&t, url, print_sink, &ps, &o, &r);
        // A body that did not end in a newline would run into the prompt.
        if (good && ps.n && !quiet) out_write("\n", 1);
    }

    if (!good) {
        out_err("%s%s%s", fetch_error_str(r.error), r.detail[0] ? " - " : "", r.detail);
        if (r.error == FETCH_ERR_RECV || r.error == FETCH_ERR_CONNECT || r.error == FETCH_ERR_SEND) {
            char d[96]; http_last_detail(d, sizeof(d));
            out_multi("  %s", d);
        }
        return 1;
    }
    return 0;
}

void http_register(void) {
    static const Command cr{"runurl", "fetch a script and run it", cmd_runurl, nullptr,
                            LEVEL_ADMIN};
    cmd_register(&cr);
    static const Command c{"wget", "download a file over HTTP", cmd_wget, nullptr};
    static const Command c2{"curl", "fetch a URL and print it", cmd_curl, nullptr};
    cmd_register(&c);
    cmd_register(&c2);
}
