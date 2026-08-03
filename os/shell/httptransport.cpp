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
#include "out.h"
#include "httpfetch.h"
#include "interrupt.h"
#include "task.h"
#include "storage.h"
#include "path.h"

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"

bool net_is_connected(void);
const char *fs_cwd(void);       // the shell's working directory (fs.cpp)

#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#define RX_RING       2048      // one BDP's worth on a local network
#define CONNECT_MS    10000
#define READ_MS       15000

// --- waiting ----------------------------------------------------------------

// Yield, don't sleep. See the header comment.
static bool wait_flag(volatile bool &flag, uint32_t ms, volatile bool *fail = nullptr) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (!flag) {
        if (fail && *fail) return false;
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
        if (intr_check()) return false;          // Ctrl+C gets out of every wait
        task_sleep_ms(2);
    }
    return true;
}

// --- connection state -------------------------------------------------------

struct TcpConn {
    struct tcp_pcb *pcb;
    uint8_t   ring[RX_RING];
    volatile uint16_t head;      // written by the lwIP callback
    volatile uint16_t tail;      // read by the task
    volatile bool connected;
    volatile bool closed;        // peer sent FIN
    volatile bool failed;
    volatile uint32_t unsent;    // bytes still in flight
};

static uint16_t ring_used(const TcpConn *c) {
    return (uint16_t)((c->head - c->tail) & (RX_RING - 1));
}
static uint16_t ring_free(const TcpConn *c) {
    return (uint16_t)(RX_RING - 1 - ring_used(c));
}

// --- lwIP callbacks (background context) ------------------------------------

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    TcpConn *c = (TcpConn *)arg;
    if (err != ERR_OK) { c->failed = true; if (p) pbuf_free(p); return ERR_OK; }
    if (!p) { c->closed = true; return ERR_OK; }        // clean FIN

    // Backpressure done properly: if it does not fit, return ERR_MEM and lwIP
    // keeps the pbuf and re-delivers later. Dropping it instead would lose
    // bytes out of the middle of a download and produce a corrupt file that
    // looks like a successful one.
    if (p->tot_len > ring_free(c)) return ERR_MEM;

    uint16_t off = 0;
    while (off < p->tot_len) {
        uint16_t n = (uint16_t)pbuf_copy_partial(p, c->ring + c->head,
                                                 (uint16_t)(RX_RING - c->head), off);
        if (n == 0) break;
        c->head = (uint16_t)((c->head + n) & (RX_RING - 1));
        off = (uint16_t)(off + n);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t) {
    TcpConn *c = (TcpConn *)arg;
    c->pcb = nullptr;            // lwIP has already freed it; never touch it again
    c->failed = true;
}

static err_t on_connected(void *arg, struct tcp_pcb *, err_t err) {
    TcpConn *c = (TcpConn *)arg;
    if (err != ERR_OK) { c->failed = true; return ERR_OK; }
    c->connected = true;
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *, u16_t len) {
    TcpConn *c = (TcpConn *)arg;
    if (c->unsent >= len) c->unsent -= len; else c->unsent = 0;
    return ERR_OK;
}

// --- DNS --------------------------------------------------------------------

struct DnsWait { volatile bool done; bool ok; ip_addr_t addr; };

static void on_dns(const char *, const ip_addr_t *ip, void *arg) {
    DnsWait *w = (DnsWait *)arg;
    if (ip) { w->addr = *ip; w->ok = true; }
    w->done = true;
}

static bool resolve(const char *host, ip_addr_t *out) {
    if (ip4addr_aton(host, ip_2_ip4(out))) { IP_SET_TYPE(out, IPADDR_TYPE_V4); return true; }
    DnsWait w{false, false, {}};
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, &w.addr, on_dns, &w);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) { *out = w.addr; return true; }
    if (e != ERR_INPROGRESS) return false;
    if (!wait_flag(w.done, 8000) || !w.ok) return false;
    *out = w.addr;
    return true;
}

// --- the HttpTransport implementation ---------------------------------------

static int lw_open(void *ctx, const char *host, uint16_t port, bool tls) {
    TcpConn *c = (TcpConn *)ctx;

    // Stage 3's job. Saying so plainly beats connecting in the clear to a port
    // expecting a handshake and reporting a confusing parse error.
    if (tls) return -1;

    memset(c, 0, sizeof(*c));

    ip_addr_t addr;
    if (!resolve(host, &addr)) return -1;

    cyw43_arch_lwip_begin();
    c->pcb = tcp_new_ip_type(IP_GET_TYPE(&addr));
    if (c->pcb) {
        tcp_arg(c->pcb, c);
        tcp_recv(c->pcb, on_recv);
        tcp_err(c->pcb, on_err);
        tcp_sent(c->pcb, on_sent);
    }
    err_t e = c->pcb ? tcp_connect(c->pcb, &addr, port, on_connected) : ERR_MEM;
    cyw43_arch_lwip_end();

    if (!c->pcb || e != ERR_OK) return -1;
    if (!wait_flag(c->connected, CONNECT_MS, &c->failed)) return -1;
    return 0;
}

static int lw_send(void *ctx, const uint8_t *data, uint32_t len) {
    TcpConn *c = (TcpConn *)ctx;
    uint32_t at = 0;

    while (at < len) {
        if (c->failed || !c->pcb) return -1;

        cyw43_arch_lwip_begin();
        uint32_t room = c->pcb ? tcp_sndbuf(c->pcb) : 0;
        uint32_t n = len - at < room ? len - at : room;
        err_t e = ERR_OK;
        if (n) {
            e = tcp_write(c->pcb, data + at, (u16_t)n, TCP_WRITE_FLAG_COPY);
            if (e == ERR_OK) { c->unsent += n; e = tcp_output(c->pcb); }
        }
        cyw43_arch_lwip_end();

        if (e == ERR_MEM) { task_sleep_ms(2); continue; }   // transient; retry
        if (e != ERR_OK) return -1;
        at += n;
        if (n == 0) {
            if (intr_check()) return -1;
            task_sleep_ms(2);      // no window; let the peer drain
        }
    }
    return 0;
}

static int lw_recv(void *ctx, uint8_t *buf, uint32_t cap) {
    TcpConn *c = (TcpConn *)ctx;

    absolute_time_t deadline = make_timeout_time_ms(READ_MS);
    while (ring_used(c) == 0) {
        if (c->failed) return -1;
        if (c->closed) return 0;                 // clean end of body
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return -1;
        if (intr_check()) return -1;
        task_sleep_ms(2);
    }

    cyw43_arch_lwip_begin();
    uint16_t used = ring_used(c);
    uint32_t n = used < cap ? used : cap;
    // One or two copies depending on whether the data wraps the ring.
    uint32_t first = RX_RING - c->tail;
    if (first > n) first = n;
    memcpy(buf, c->ring + c->tail, first);
    if (n > first) memcpy(buf + first, c->ring, n - first);
    c->tail = (uint16_t)((c->tail + n) & (RX_RING - 1));
    cyw43_arch_lwip_end();

    return (int)n;
}

static void lw_close(void *ctx) {
    TcpConn *c = (TcpConn *)ctx;
    cyw43_arch_lwip_begin();
    if (c->pcb) {
        tcp_arg(c->pcb, nullptr);
        tcp_recv(c->pcb, nullptr);
        tcp_err(c->pcb, nullptr);
        tcp_sent(c->pcb, nullptr);
        // tcp_close can fail when memory is tight; abort then, because leaving
        // a pcb with our (stack) arg attached is how a later callback writes
        // into a connection that no longer exists.
        if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
        c->pcb = nullptr;
    }
    cyw43_arch_lwip_end();
}

// The connection is big (a 2 KB ring), so it does not live on a task stack.
// One at a time is the right limit here: a shell running two downloads at once
// is not a thing worth the second buffer.
static TcpConn g_conn;

bool http_transport_get(HttpTransport *t) {
    if (!net_is_connected()) return false;
    t->open = lw_open; t->send = lw_send; t->recv = lw_recv; t->close = lw_close;
    t->ctx = &g_conn;
    return true;
}

#else   // no radio on this board

bool http_transport_get(HttpTransport *) { return false; }

#endif

// --- wget -------------------------------------------------------------------

struct FileSink { void *h; };

static int file_sink(void *ctx, const uint8_t *data, uint32_t len) {
    FileSink *f = (FileSink *)ctx;
    // Refusing here is what stops a download that has filled the disk, and the
    // driver turns it into "could not write" rather than "bad response".
    return storage_sink_write(f->h, data, len) ? 0 : -1;
}

static void show_progress(void *, uint64_t got, uint64_t total) {
    static uint64_t last;
    // Only redraw every 4 KB. A line per segment costs more time than the
    // transfer it is reporting on.
    if (got - last < 4096 && got != total) return;
    last = got;
    char line[64];
    int n;
    if (total) n = snprintf(line, sizeof(line), "\r  %lu / %lu bytes (%lu%%)   ",
                            (unsigned long)got, (unsigned long)total,
                            (unsigned long)(got * 100 / total));
    else       n = snprintf(line, sizeof(line), "\r  %lu bytes   ", (unsigned long)got);
    // Written raw: this is a line being redrawn in place, so it must not get a
    // tag or a newline, and it must not go down a pipe as data.
    if (n > 0) out_write(line, (uint32_t)n);
}

static int poll_interrupt(void *) { return intr_check() ? 1 : 0; }

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
    out_write("\n", 1);

    if (!good) {
        storage_remove(full);        // a partial file is worse than none
        out_err("%s%s%s", fetch_error_str(r.error),
                r.detail[0] ? " - " : "", r.detail);
        return 1;
    }

    out_ok("Saved %s (%llu bytes)", full, (unsigned long long)r.bytes);
    return 0;
}

void http_register(void) {
    static const Command c{"wget", "download a file over HTTP", cmd_wget, nullptr};
    cmd_register(&c);
}
