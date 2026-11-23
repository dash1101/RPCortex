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
extern "C" const unsigned char stock_cacerts_data[];
extern "C" const unsigned int  stock_cacerts_len;

bool stock_install_cacerts(bool force) {
    bool is_dir = false; uint32_t size = 0;
    // Only when absent or empty, so a hand-updated bundle survives a firmware
    // update that would otherwise put the shipped one back.
    if (!force && storage_stat("/os/ca.pem", &is_dir, &size) && size > 0) return true;

    if (!storage_write_file("/os/ca.pem", stock_cacerts_data, stock_cacerts_len)) {
        // Saying nothing here is how a device ends up unable to install
        // packages with no clue why: pkg update reports "no trusted
        // certificates" and nothing ever explains that the write failed.
        out_errp("certs", "Could not write /os/ca.pem — HTTPS will not verify.");
        log_add(LOG_K_ERR, "certs: /os/ca.pem could not be written");
        return false;
    }
    log_addf(LOG_K_OK, "certs: installed %u bytes of trusted roots",
             (unsigned)stock_cacerts_len);
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

bool net_radio_is_up(void);

static struct altcp_tls_config *tls_config(void) {
    if (g_tls_tried) return g_tls_cfg;
    // Not before the radio has been started. The parsed roots live on lwIP's
    // heap (altcp_tls allocates through mem_calloc), and lwIP is initialised
    // with the radio, so with the radio still off mem_malloc walks a free list
    // that was never built and does not come back. `compat` and `pkg certs`
    // both asked from a board whose radio had not been switched on, and the
    // watchdog restarted it 16 s later (seen on a Pico 2 W). Not cached: the
    // answer is different once the radio is up.
    if (!net_radio_is_up()) {
        snprintf(g_tls_why, sizeof(g_tls_why),
                 "the radio is off, so the certificates have not been loaded yet");
        return nullptr;
    }
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
        if (c->pcb && tls)
            mbedtls_ssl_set_hostname((mbedtls_ssl_context *)altcp_tls_context(c->pcb), host);
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
    snprintf(out, cap, "%s; handshake %s, sent %lu, received %lu",
             why, g_conn.handshook ? "ok" : "NOT DONE",
             (unsigned long)g_conn.sent, (unsigned long)g_conn.got);

    // A TLS handshake that dies before anything is exchanged, with no network
    // error behind it, is nearly always the heap: it takes about 45 KB for the
    // session, the certificate chain and the key exchange, and mbedtls reports a
    // failed allocation only as "the handshake did not finish". Measured on a
    // Pico 2 W: 41 KB free failed, 47 KB free worked. Say so, with the number,
    // rather than leave "no error reported" as the whole explanation.
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
    out_err("No network. Connect with 'wifi connect <ssid>' first.");
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
    if (argc >= 3 && !strcmp(argv[2], "install")) {
        if (!stock_install_cacerts(/*force*/true)) return 1;
        http_tls_reset();          // so the repair takes effect immediately
        out_ok("Trusted roots reinstalled.");
    }

    bool is_dir = false; uint32_t size = 0;
    bool present = storage_stat("/os/ca.pem", &is_dir, &size);
    out_info("Trusted roots");
    out_multi("  Built in   always available; the image carries its own copy");
    out_multi("  Override   /os/ca.pem  %s", present ? "present" : "not set");
    if (present) out_multi("  Size       %lu bytes", (unsigned long)size);

    if (http_tls_available()) {
        out_ok("HTTPS can be verified.");
    } else {
        out_warn("HTTPS cannot be verified: %s", http_tls_why());
        out_multi("  'pkg certs install' writes the bundle that ships in firmware.");
    }
    return 0;
}
