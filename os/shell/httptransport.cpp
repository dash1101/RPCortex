// The lwIP side of the HTTP client: the transport over Wi-Fi, and the TLS roots.
//
// wget, curl and the fw_http_* calls are in httpclient.cpp, in every image, and
// reach this through httproute.h; an image without Wi-Fi has the bridge instead.
//
// core/httpfetch.cpp decides what happens; this only moves bytes. Keeping the
// split that sharp is what let redirects, size caps, truncation and a full
// filesystem all be tested on a host with no radio in it.
//
// CONFIRMED on a Pico 2 W: `wget` and `curl` over HTTPS through this, and the websearch and
// speedtest packages, which reach the wider web rather than one known server.
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
#include "fsguard.h"
#include "blackbox.h"
#include "netown.h"
#include "task.h"
#include "kernel.h"
#include "logring.h"
#include "stockca.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"

bool net_is_connected(void);
// Only one task may be inside cyw43 or lwIP at a time; see netown.h. A
// connection takes that ownership for each operation on it, not from open to
// close: held for a whole download, it kept every other user of the network
// waiting, an SSH session's own keystrokes and output included. What used to
// keep a WiFi join from landing part-way through a transfer is now the radio
// generation (net_radio_gen): a connection that sees the radio has been down
// since it opened stops, and never touches the PCB it had.
void net_op_acquire(void);
void net_op_release(void);
bool net_core_ok(void);
#include "httproute.h"
#include "pkgrepo.h"

#include "pico/cyw43_arch.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/platform.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

// Under the 8 s watchdog, deliberately.
//
// These waits yield, so they do feed it and a long one is not supposed to
// reboot anything. But a handshake that cannot complete held the line for ten
// seconds against a watchdog that fires at eight, and every reboot in that
// window arrived with no explanation. Giving up first means the failure is
// reported by the code that knows what failed.
// Under the 16 s watchdog, with room for a slow chain.
//
// 6000 was chosen when the watchdog was 8000 and a handshake could not be
// interrupted. It then became the thing that failed: with the ECC flags missing
// the Wikipedia chain took longer than six seconds and this gave up on a
// connection that was still working.
#define CONNECT_MS    12000
#define READ_MS       15000
#define SEND_MS       10000

// The connect and read limits for the request in flight. `curl --timeout` moves
// them; every command puts them back afterwards, so a timeout set for one
// request cannot leak into the next download.
static uint32_t g_connect_ms = CONNECT_MS;
static uint32_t g_read_ms    = READ_MS;

// 0 puts the defaults back.
void http_net_set_timeout_ms(uint32_t ms) {
    g_connect_ms = ms ? ms : CONNECT_MS;
    g_read_ms    = ms ? ms : READ_MS;
}

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
    uint32_t gen;                // net_radio_gen() when it opened

    // Enough to say WHAT went wrong rather than that something did. "connection
    // lost" covers a refused handshake, a peer that hung up, and a read that
    // timed out, and those need three different next steps.
    volatile int   last_err;     // the lwIP error, when one arrived
    volatile bool  handshook;    // the connected callback fired
    bool tls;                    // this connection was HTTPS
    uint32_t free_at_open;       // heap free when it was opened, for a failure message
    volatile uint32_t sent;      // request bytes handed to the stack
    volatile uint32_t got;       // payload bytes received
    // Why the server's certificate was refused (MBEDTLS_X509_BADCERT_*), from
    // the handshake's verify callback; 0 when it verified or was never checked.
    volatile uint32_t verify_flags;
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

static err_t on_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    TcpConn *c = (TcpConn *)arg;
    if (err != ERR_OK) { c->failed = true; return ERR_OK; }
    // The second lock on the door: verification is REQUIRED, so a handshake
    // with a certificate that did not verify should never get here. If it does
    // (a configuration that lost that setting), nothing is sent: the request
    // is written only once `connected` is set, and it is not.
    if (c->tls) {
        uint32_t r = mbedtls_ssl_get_verify_result((mbedtls_ssl_context *)altcp_tls_context(pcb));
        if (r != 0) { c->verify_flags |= r; c->failed = true; return ERR_OK; }
    }
    c->connected = true;
    c->handshook = true;         // for TLS this means the handshake finished
    return ERR_OK;
}

// Called for each certificate in the server's chain as it is checked; what it
// keeps is the reason a refusal gives (an untrusted root, another host's name).
// It changes nothing: the verdict is mbedtls's.
static int on_verify(void *arg, mbedtls_x509_crt *, int, uint32_t *flags) {
    TcpConn *c = (TcpConn *)arg;
    if (*flags) c->verify_flags |= *flags;
    return 0;
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

// --- the shipped roots, written out on a first boot ---------------------------
//
// Not a package, but it ships the same way and for the same reason: it must
// exist on a first boot without anyone having to fetch it, and yet remain
// replaceable afterwards. stock.cpp calls this at first boot and `pkg certs
// install` calls it to repair the file. It lives here, beside the TLS code that
// reads the roots, so an image built without Wi-Fi carries neither.
//
// Written only when absent, so a hand-updated bundle survives a firmware update
// that would otherwise put the shipped one back.
// The built-in roots are no longer copied into /os/ca.pem: they are read from
// the image itself (stockca.h), so a firmware update brings its new roots with
// it, and /os/ca.pem holds only roots someone added. A board that has the old
// copy keeps it harmlessly: roots in it that are built in are skipped.
bool stock_install_cacerts(bool) { return true; }

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
static char g_tls_why[80];
static mbedtls_x509_crt *g_extra;      // roots from /os/ca.pem not already built in
static unsigned g_extra_n;

bool net_radio_is_up(void);

static bool is_stock(const mbedtls_x509_crt *c) {
    for (unsigned i = 0; i < stock_ca_count; i++)
        if (stock_ca[i].len == c->raw.len && !memcmp(stock_ca[i].der, c->raw.p, c->raw.len)) return true;
    return false;
}

// The issuer the handshake is looking for, among the trusted roots: the
// built-in ones, parsed from flash as needed (nocopy: nothing but the parsed
// fields is allocated), and any added in /os/ca.pem. mbedtls frees the chain.
static int ca_lookup(void *, mbedtls_x509_crt const *child, mbedtls_x509_crt **out) {
    mbedtls_x509_crt *chain = nullptr;
    const uint8_t *want = child->issuer_raw.p;
    size_t want_n = child->issuer_raw.len;
    auto take = [&](void) -> bool {
        if (chain) return true;
        chain = (mbedtls_x509_crt *)mbedtls_calloc(1, sizeof(mbedtls_x509_crt));
        if (!chain) return false;
        mbedtls_x509_crt_init(chain);
        return true;
    };
    for (unsigned i = 0; i < stock_ca_count; i++) {
        const StockCa &r = stock_ca[i];
        if (r.subj_len != want_n || memcmp(r.der + r.subj_off, want, want_n)) continue;
        if (!take()) break;
        mbedtls_x509_crt_parse_der_nocopy(chain, r.der, r.len);
    }
    for (const mbedtls_x509_crt *e = g_extra; e && e->raw.p; e = e->next) {
        if (e->subject_raw.len != want_n || memcmp(e->subject_raw.p, want, want_n)) continue;
        if (!take()) break;
        mbedtls_x509_crt_parse_der(chain, e->raw.p, e->raw.len);
    }
    // A chain whose parses all failed is no candidate at all.
    if (chain && !chain->raw.p) { mbedtls_x509_crt_free(chain); mbedtls_free(chain); chain = nullptr; }
    *out = chain;
    return 0;
}

// Roots someone added: /os/ca.pem, PEM, read once. Ones that are built in are
// dropped (every board before this firmware has a copy of the old three).
static void load_extra_roots(void) {
    bool is_dir = false; uint32_t fsize = 0;
    if (!storage_stat(CA_PATH, &is_dir, &fsize) || is_dir || fsize == 0 || fsize >= CA_MAX) return;
    uint8_t *pem = (uint8_t *)malloc(fsize + 1);
    if (!pem) return;
    uint32_t n = storage_read_file(CA_PATH, pem, fsize);
    pem[n] = 0;
    mbedtls_x509_crt all;
    mbedtls_x509_crt_init(&all);
    if (n) mbedtls_x509_crt_parse(&all, pem, n + 1);     // keeps what parses
    free(pem);
    for (mbedtls_x509_crt *c = &all; c && c->raw.p; c = c->next) {
        if (is_stock(c)) continue;
        if (!g_extra) {
            g_extra = (mbedtls_x509_crt *)mbedtls_calloc(1, sizeof(mbedtls_x509_crt));
            if (!g_extra) break;
            mbedtls_x509_crt_init(g_extra);
        }
        if (mbedtls_x509_crt_parse_der(g_extra, c->raw.p, c->raw.len) == 0) g_extra_n++;
    }
    mbedtls_x509_crt_free(&all);
}

static struct altcp_tls_config *tls_config(void) {
    if (g_tls_tried) return g_tls_cfg;
    if (!net_radio_is_up()) {
        snprintf(g_tls_why, sizeof(g_tls_why),
                 "the radio is off, so the certificates have not been loaded yet");
        return nullptr;
    }
    g_tls_tried = true;
    if (stock_ca_count == 0) {
        snprintf(g_tls_why, sizeof(g_tls_why), "this image carries no trusted roots");
        return nullptr;
    }
    // No CA chain in the config: the roots come from ca_lookup instead.
    g_tls_cfg = altcp_tls_create_config_client(nullptr, 0);
    if (!g_tls_cfg) {
        snprintf(g_tls_why, sizeof(g_tls_why), "the TLS configuration could not be made");
        return nullptr;
    }
    // lwIP's struct altcp_tls_config begins with its mbedtls_ssl_config (lwIP
    // 2.2, altcp_tls_mbedtls.c), which is what this reaches. Verification is
    // REQUIRED (lwipopts.h), and on_connected checks the result as well.
    mbedtls_ssl_config *conf = (mbedtls_ssl_config *)g_tls_cfg;
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_cb(conf, ca_lookup, nullptr);
    load_extra_roots();
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

// Is the connection still usable? Only inside a NetOwn scope. A connection
// opened before the radio last went down was closed by http_radio_down.
static bool conn_live(const TcpConn *c) {
    return net_radio_is_up() && c->gen == net_radio_gen();
}

// ONE HTTP CONNECTION AT A TIME, device-wide: g_conn is a single static. It
// used to be kept to one by holding the network lock from open to close. It
// is a claim of its own now, made and checked under the lock but not held
// with it, so a download no longer keeps the rest of the network waiting. A
// second fetch waits for the first (Ctrl+C gets out). A claim left by a task
// that ended mid-transfer is taken over, and its connection taken apart.
static volatile int g_conn_owner;     // pid; 0 when free

static bool conn_owner_gone(int pid) {
    const TaskInfo *ti = task_find(pid);
    return !ti || ti->state == TASK_DONE;
}

// Under a NetOwn.
static bool conn_claim(TcpConn *c) {
    int me = task_self();
    if (g_conn_owner && g_conn_owner != me && !conn_owner_gone(g_conn_owner)) return false;
    if (g_conn_owner && g_conn_owner != me) conn_detach(c);    // ended mid-transfer
    g_conn_owner = me;
    return true;
}

static void conn_release(TcpConn *c) {
    {
        NetOwn own;
        if (own.ok) conn_detach(c);
        else { c->pcb = nullptr; c->rx = nullptr; }   // unreachable radio: let it go
    }
    if (g_conn_owner == task_self()) g_conn_owner = 0;
}

static int lw_open(void *ctx, const char *host, uint16_t port, bool tls) {
    TcpConn *c = (TcpConn *)ctx;
    // A hang inside a fetch used to leave "entered fw_http_get" as the last
    // phase and nothing else, so every stage of a connection had to be ruled out
    // by argument. Each one says so now: the crash report names the last one
    // reached, which is the one that did not finish.
    bb_note_phase("http: waiting for the connection");
    for (;;) {
        {
            NetOwn own;
            // The wireless driver only works on the core that initialised it,
            // and NetOwn could not get this task there.
            if (!own.ok) { out_err("Networking is not available from this core."); return -1; }
            if (conn_claim(c)) break;
        }
        if (intr_check()) return -1;
        task_sleep_ms(50);
    }
    // The connection is this task's from here, and every failure below gives
    // it back through conn_release.

    struct altcp_tls_config *cfg = nullptr;
    {
        NetOwn own;
        if (!own.ok) { conn_release(c); return -1; }
        if (tls) {
            bb_note_phase("http: loading roots");
            cfg = tls_config();
            if (!cfg) { conn_release(c); return -1; }    // no roots: refuse, never downgrade
        }
        // Anything still attached from a previous transfer goes now, BEFORE
        // the memset that would orphan it. Every path that abandons a
        // connection is supposed to have detached it already; this is what
        // makes "supposed to" not matter.
        conn_detach(c);
        memset(c, 0, sizeof(*c));
        c->tls = tls;
        c->free_at_open = heap_free();
        c->gen = net_radio_gen();
    }

    ip_addr_t addr;
    bb_note_phase("http: resolving");
    if (!resolve(host, &addr)) { conn_release(c); return -1; }     // takes the network itself

    err_t e = ERR_MEM;
    {
        NetOwn own;
        if (!own.ok || !conn_live(c)) { conn_release(c); return -1; }
        bb_note_phase(tls ? "http: opening (tls)" : "http: opening");
        cyw43_arch_lwip_begin();
        c->pcb = tls ? altcp_tls_new(cfg, IP_GET_TYPE(&addr))
                     : altcp_tcp_new_ip_type(IP_GET_TYPE(&addr));

        // SNI. Without it a shared host answers with the wrong certificate and
        // the handshake fails for a reason that looks nothing like the cause:
        // raw.githubusercontent.com is exactly such a host.
        if (c->pcb && tls) {
            mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(c->pcb);
            mbedtls_ssl_set_hostname(ssl, host);
            mbedtls_ssl_set_verify(ssl, on_verify, c);      // for the reason, if it fails
        }
        if (c->pcb) {
            altcp_arg(c->pcb, c);
            altcp_recv(c->pcb, on_recv);
            altcp_err(c->pcb, on_err);
            altcp_sent(c->pcb, on_sent);
        }
        e = c->pcb ? altcp_connect(c->pcb, &addr, port, on_connected) : ERR_MEM;
        cyw43_arch_lwip_end();
    }

    // From here on the pcb exists and its callbacks point at `c`, so no exit may
    // simply return: the connection has to be taken apart first. lw_close is not
    // called for an open that failed (the driver only closes what it opened),
    // which is why these say so themselves.
    if (!c->pcb || e != ERR_OK) { conn_release(c); return -1; }
    bb_note_phase(tls ? "http: handshaking" : "http: connecting");
    // The handshake happens in lwIP's callbacks; nothing is held while it does.
    if (!wait_flag(c->connected, g_connect_ms, &c->failed) || c->gen != net_radio_gen()) {
        // Distinct phases, because "still waiting" and "gave up and is tearing
        // down" are eight seconds apart and were indistinguishable: both left
        // "http: handshaking" as the last note, so a reboot during the teardown
        // read exactly like a reboot during the wait.
        bb_note_phase("http: handshake gave up");
        conn_release(c);
        bb_note_phase("http: handshake torn down");
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

        uint32_t n = 0;
        err_t e = ERR_OK;
        {
            NetOwn own;
            if (!own.ok || !conn_live(c)) { c->failed = true; return -1; }
            cyw43_arch_lwip_begin();
            uint32_t room = c->pcb ? altcp_sndbuf(c->pcb) : 0;
            n = len - at < room ? len - at : room;
            if (n) {
                e = altcp_write(c->pcb, data + at, (u16_t)n, TCP_WRITE_FLAG_COPY);
                if (e == ERR_OK) { c->unsent += n; e = altcp_output(c->pcb); }
            }
            cyw43_arch_lwip_end();
        }

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

    // The wait holds nothing: the data arrives through lwIP's callbacks.
    absolute_time_t deadline = make_timeout_time_ms(g_read_ms);
    while (rx_available(c) == 0) {
        if (c->failed) return -1;
        if (c->closed) return 0;                 // clean end of body
        if (c->gen != net_radio_gen()) { c->failed = true; return -1; }   // the radio went down
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            c->last_err = -101;                  // "nothing arrived in time"
            return -1;
        }
        if (intr_check()) return -1;
        task_sleep_ms(2);
    }

    NetOwn own;
    if (!own.ok || !conn_live(c)) { c->failed = true; return -1; }
    cyw43_arch_lwip_begin();
    uint16_t avail = rx_available(c);
    uint16_t n = (uint16_t)(cap < avail ? cap : avail);
    n = pbuf_copy_partial(c->rx, buf, n, 0);

    // Free what has been read, NOW, rather than when the whole chain is done.
    //
    // Waiting until the chain emptied looked simpler and could not work: more
    // data keeps arriving and being appended, so tot_len grows as fast as the
    // reader advances and the end is never reached. Nothing is freed, the pbuf
    // pool runs dry, and the driver has nowhere to put the next packet: the
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
    // dropped, and the sender believes they were delivered, so it never
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
// Caller holds a NetOwn and NOT the lwIP lock. With the radio down there is
// nothing to detach: http_radio_down did it before the radio went.
static void conn_detach(TcpConn *c) {
    if (!c->pcb && !c->rx) return;
    if (!net_radio_is_up()) { c->pcb = nullptr; c->rx = nullptr; return; }
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
    conn_release((TcpConn *)ctx);
}

// How much free heap a TLS handshake wants. A measured threshold with a margin,
// not a calculated one: on a Pico 2 W with everything loaded, 41 KB free failed
// and 47 KB free worked.
#define TLS_HEAP_NEEDED (50u * 1024u)

// The connection is big (a 2 KB ring), so it does not live on a task stack.
// One at a time is the right limit here: a shell running two downloads at once
// is not a thing worth the second buffer.
static TcpConn g_conn;

// The radio is about to go down: take the connection apart while lwIP may
// still be entered. Its owner finds it failed. Called by radio_down holding
// the network.
void http_radio_down(void) {
    TcpConn *c = &g_conn;
    conn_detach(c);
    c->failed = true;
}

// What the last transfer did, for an error message worth reading.
void http_net_last_detail(char *out, unsigned cap) {
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
    if (g_conn.tls && g_conn.verify_flags) {
        uint32_t f = g_conn.verify_flags;
        const char *what =
            (f & MBEDTLS_X509_BADCERT_CN_MISMATCH) ? "its certificate is for another name (someone else's server, or a misconfigured one)" :
            (f & MBEDTLS_X509_BADCERT_NOT_TRUSTED) ? "its certificate does not lead to a root this board trusts" :
            (f & MBEDTLS_X509_BADCERT_REVOKED)     ? "its certificate is revoked" :
                                                     "its certificate did not verify";
        snprintf(out, cap, "refused the server: %s (flags 0x%lx). Nothing was sent. A root can be added in /os/ca.pem (sudo); see 'pkg certs'",
                 what, (unsigned long)f);
        return;
    }
    snprintf(out, cap, "%s; handshake %s, sent %lu, received %lu",
             why, g_conn.handshook ? "ok" : "NOT DONE",
             (unsigned long)g_conn.sent, (unsigned long)g_conn.got);

    // A TLS handshake that dies before anything is exchanged, with no network
    // error behind it, is nearly always the heap: it takes about 45 KB for the
    // session, the certificate chain and the key exchange, and mbedtls reports a
    // failed allocation only as "the handshake did not finish". Measured on a
    // Pico 2 W: 41 KB free failed, 47 KB free worked. Say so, with the number,
    // rather than leave "no error reported" as the whole explanation.
    // A handshake that ended with no network error, no memory shortage and no
    // reason from the certificate check: in practice a server this board
    // would not trust, refused before the check could say more. Nothing was sent.
    if (g_conn.tls && !g_conn.handshook && g_conn.last_err == 0 && g_conn.free_at_open >= TLS_HEAP_NEEDED) {
        size_t n = strlen(out);
        if (n + 1 < cap)
            snprintf(out + n, cap - n, " - the server's certificate may not be one this board trusts ('pkg certs')");
    }
    if (g_conn.tls && !g_conn.handshook && g_conn.free_at_open < TLS_HEAP_NEEDED) {
        size_t n = strlen(out);
        if (n + 1 < cap)
            snprintf(out + n, cap - n,
                     " - low on memory (%u KB free, TLS needs about %u KB): 'unload' a package or 'reboot'",
                     (unsigned)(g_conn.free_at_open / 1024), (unsigned)(TLS_HEAP_NEEDED / 1024));
    }
}

bool http_net_transport_get(HttpTransport *t) {
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

void http_net_no_route(void) {
    out_err("No network. Connect with 'wifi connect <ssid>', or plug the board into a");
    out_multi("  computer running 'rpc bridge' (RPC Studio), which lends it its connection.");
}

// --- the bridge, which this image does not have --------------------------------
//
// An image with Wi-Fi has a network of its own, so it never asks the host for
// one: HELLO says bridge=0, and link's BRIDGE answers ERR nobridge. The bridge
// itself (bridge.cpp) is built only into images without Wi-Fi.
bool bridge_transport_get(HttpTransport *) { return false; }
const char *bridge_last_detail(void) { return ""; }
bool bridge_available(void) { return false; }
bool bridge_offered(void) { return false; }
bool bridge_set_offer(bool) { return false; }
void bridge_no_route(void) { http_net_no_route(); }
void bridge_console_line_state(bool) {}

// --- pkg certs ------------------------------------------------------------------
//
// What the trust store looks like, and a way to put it back. Here, beside the TLS
// code that reads the roots, because an image without Wi-Fi has neither; its
// net_off.cpp answers for this command there.
//
// The bundle ships in flash and is written on a first boot, but a write can
// fail, a file can be truncated, and neither announces itself: the only symptom
// is `pkg update` refusing to verify. This turns that into a readable answer
// and a one-word repair, without a reflash.
int pkg_fetch_certs(int argc, char **argv) {
    (void)argc; (void)argv;
    tls_config();
    bool is_dir = false; uint32_t size = 0;
    bool present = storage_stat(CA_PATH, &is_dir, &size) && !is_dir && size > 0;
    out_info("Trusted roots");
    out_multi("  Built in   %u, from the firmware (updated with it)", stock_ca_count);
    if (present)
        out_multi("  Added      %u in %s%s", g_extra_n, CA_PATH,
                  g_tls_tried ? "" : " (read at the first HTTPS request)");
    else
        out_multi("  Added      none (%s, PEM, sudo to change)", CA_PATH);
    out_multi("  Every HTTPS connection is checked against these, and to the host's name;");
    out_multi("  a server whose certificate does not verify is refused before anything is sent.");
    if (!http_tls_available()) out_warn("HTTPS is not available: %s", http_tls_why());
    return 0;
}
