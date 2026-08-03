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
#include <stdlib.h>

#include "pico/stdlib.h"

bool net_is_connected(void);
// Only one task may be inside cyw43 or lwIP at a time — see net.cpp. A
// connection holds that ownership from open to close, so a download and a WiFi
// join can never be part-way through each other.
void net_op_acquire(void);
void net_op_release(void);
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
static struct altcp_tls_config *tls_config(void) {
    if (g_tls_tried) return g_tls_cfg;
    g_tls_tried = true;

    bool is_dir = false; uint32_t fsize = 0;
    if (!storage_stat(CA_PATH, &is_dir, &fsize)) {
        snprintf(g_tls_why, sizeof(g_tls_why), "%s does not exist", CA_PATH);
        return nullptr;
    }
    if (fsize == 0) {
        snprintf(g_tls_why, sizeof(g_tls_why), "%s is empty", CA_PATH);
        return nullptr;
    }
    if (fsize >= CA_MAX) {
        snprintf(g_tls_why, sizeof(g_tls_why), "%s is %lu bytes, over the %d limit",
                 CA_PATH, (unsigned long)fsize, CA_MAX);
        return nullptr;
    }

    uint8_t *pem = (uint8_t *)malloc(CA_MAX);
    if (!pem) {
        snprintf(g_tls_why, sizeof(g_tls_why), "no room to load %lu bytes",
                 (unsigned long)fsize);
        return nullptr;
    }

    uint32_t n = storage_read_file(CA_PATH, pem, CA_MAX - 1);
    if (n == 0) {
        snprintf(g_tls_why, sizeof(g_tls_why), "%s would not read back", CA_PATH);
        free(pem);
        return nullptr;
    }
    pem[n] = 0;                       // mbedtls counts the terminator for PEM

    g_tls_cfg = altcp_tls_create_config_client(pem, n + 1);
    if (!g_tls_cfg)
        snprintf(g_tls_why, sizeof(g_tls_why),
                 "%lu bytes read, but no certificate parsed", (unsigned long)n);
    free(pem);
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

static int lw_open(void *ctx, const char *host, uint16_t port, bool tls) {
    TcpConn *c = (TcpConn *)ctx;
    // Taken here and released in lw_close, which the fetch driver always calls
    // — including on every failure path and between redirect hops.
    net_op_acquire();

    struct altcp_tls_config *cfg = nullptr;
    if (tls) {
        cfg = tls_config();
        if (!cfg) { net_op_release(); return -1; }   // no roots: refuse, never downgrade
    }

    memset(c, 0, sizeof(*c));

    ip_addr_t addr;
    if (!resolve(host, &addr)) { net_op_release(); return -1; }

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

    if (!c->pcb || e != ERR_OK) { net_op_release(); return -1; }
    if (!wait_flag(c->connected, CONNECT_MS, &c->failed)) { net_op_release(); return -1; }
    return 0;
}

static int lw_send(void *ctx, const uint8_t *data, uint32_t len) {
    TcpConn *c = (TcpConn *)ctx;
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

static void lw_close(void *ctx) {
    TcpConn *c = (TcpConn *)ctx;
    cyw43_arch_lwip_begin();
    if (c->rx) { pbuf_free(c->rx); c->rx = nullptr; }
    if (c->pcb) {
        altcp_arg(c->pcb, nullptr);
        altcp_recv(c->pcb, nullptr);
        altcp_err(c->pcb, nullptr);
        altcp_sent(c->pcb, nullptr);
        // altcp_close can fail when memory is tight; abort then, because leaving
        // a pcb with our (stack) arg attached is how a later callback writes
        // into a connection that no longer exists.
        if (altcp_close(c->pcb) != ERR_OK) altcp_abort(c->pcb);
        c->pcb = nullptr;
    }
    cyw43_arch_lwip_end();
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

#else   // no radio on this board

bool http_transport_get(HttpTransport *) { return false; }
bool http_tls_available(void) { return false; }
void http_last_detail(char *out, unsigned cap) { snprintf(out, cap, "no wireless"); }
const char *http_tls_why(void) { return "no wireless on this board"; }
void http_tls_reset(void) {}

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
        out_err("No trusted certificates, so HTTPS cannot be verified.");
        out_multi("  Install them at /os/ca.pem. Unverified HTTPS is not offered:");
        out_multi("  anything on the path could serve a package and it would install.");
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
    static const Command c{"wget", "download a file over HTTP", cmd_wget, nullptr};
    static const Command c2{"curl", "fetch a URL and print it", cmd_curl, nullptr};
    cmd_register(&c);
    cmd_register(&c2);
}
