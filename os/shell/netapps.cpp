// Network commands that are not WiFi management: ping, nslookup, ntp.
//
// All three sit on lwIP directly and none of them needs TLS, which is why they
// come first — a package fetch needs mbedtls and a much bigger conversation
// about RAM, while these are useful today and cost almost nothing.
//
// Threading model: the build uses pico_cyw43_arch_lwip_threadsafe_background, so
// lwIP callbacks fire from a background context and any lwIP call made from the
// shell must be wrapped in cyw43_arch_lwip_begin/end. Getting that wrong does
// not fail loudly — it corrupts the stack's internal lists under load, which is
// the worst kind of bug to find later. Every raw lwIP call below is wrapped.
//
// Wrapping is necessary and it is not sufficient. The lwIP lock is keyed on the
// CORE, so it neither separates two tasks on one core nor lets a task on the
// other core do anything but stop — netown.h has the mechanism and the reset it
// caused. Every entry below therefore takes NetOwn first, which is what makes
// `ping` safe to call from the shell and fw_net_ping safe to call from a
// package's unpinned task.

#include "kernel.h"
#include "command.h"
#include "out.h"
#include "registry.h"
#include "interrupt.h"
#include "task.h"
#include "netown.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/aon_timer.h"
#include "lwip/dns.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/netif.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"

#include <time.h>

bool net_is_connected(void);        // net.cpp

// Wait for `flag` to go true, or `ms` to elapse. The shell is otherwise blocking
// and the background context needs the core, so this yields rather than spins.
static bool wait_for(volatile bool &flag, uint32_t ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (!flag) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
        if (intr_check()) return false;      // Ctrl+C gets out of every wait
        // task_sleep_ms, not sleep_ms: a raw sleep parks the core without
        // reaching the scheduler, so nothing feeds the watchdog. resolve_host
        // waits up to 8 s, which against an 8 s watchdog is a reboot rather
        // than a timeout — the same bug that made the login prompt restart.
        task_sleep_ms(5);
    }
    return true;
}

// --- DNS --------------------------------------------------------------------

// One implementation, in net.cpp, because the record lwIP writes into has to
// outlive this call — see the note there. This used to be a DnsWait on the
// stack, and giving up early left lwIP holding its address.
bool net_resolve(const char *host, ip_addr_t *out);        // net.cpp

// Resolve a name, or parse it if it is already a dotted quad.
static bool resolve_host(const char *host, ip_addr_t *out) {
    return net_resolve(host, out);
}

static bool require_network(void) {
    if (net_is_connected()) return true;
    out_err("Not connected. Use 'wifi connect <ssid>' first.");
    return false;
}

static int cmd_nslookup(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: nslookup <host>"); return 1; }
    if (!require_network()) return 1;
    ip_addr_t addr;
    if (!resolve_host(argv[1], &addr)) { out_err("Could not resolve '%s'.", argv[1]); return 1; }
    out_multi("  %s  ->  %s", argv[1], ipaddr_ntoa(&addr));
    return 0;
}

// --- ping -------------------------------------------------------------------
//
// ICMP echo over lwIP's raw API. The reply callback has to check the id and
// sequence itself: a raw PCB sees EVERY inbound ICMP packet on the interface,
// including replies to somebody else's ping and unrelated error messages.

#define PING_ID       0x5243        // 'RC'
#define PING_DATA_LEN 32

// Scoped form of the begin/end pair this file uses elsewhere. The function
// below has several exits and a paired call at each one is a lock left held the
// first time somebody adds another.
// A NetOwn has to be in scope above it, every time. The constructor notes it
// when there is not, so the rule is enforced by the build rather than by
// remembering.
struct LwipLock {
    LwipLock()  { if (!net_op_held()) net_unowned_note(); cyw43_arch_lwip_begin(); }
    ~LwipLock() { cyw43_arch_lwip_end(); }
};

// A ping's state, in a small static table rather than on the pinging task's
// stack or in one global.
//
// Not one global: two terminals (the console and an SSH session, or two
// sessions) can ping at once, and each needs its own replies. Each slot gets
// an ICMP id of its own, so even two pings of the same host keep apart.
//
// Not on the stack: the network is no longer held for the whole run (see
// cmd_ping), so a ping can be ended while it waits, and the raw PCB's callback
// would then write into a stack that has been freed. A slot is static memory;
// one whose owner has gone is reclaimed, its PCB removed, by the next ping.
#define PING_SLOTS 4
struct PingSlot {
    volatile bool   got;
    uint16_t        id, seq;
    uint32_t        sent_us, rtt_us;
    ip_addr_t       target;
    struct raw_pcb *pcb;
    int             owner;           // pid; 0 when free
    uint32_t        rgen;            // net_radio_gen() when opened
};

// A ping from before the radio last went down had its socket closed by
// ping_radio_down.
static bool ping_live(const PingSlot *s) {
    return s->pcb && net_radio_is_up() && s->rgen == net_radio_gen();
}
static PingSlot g_pings[PING_SLOTS];

static u8_t ping_recv(void *arg, struct raw_pcb *, struct pbuf *p, const ip_addr_t *addr) {
    PingSlot *s = (PingSlot *)arg;
    // The payload starts after the IP header; lwIP hands the raw PCB the whole
    // datagram, so the IP header has to be stepped over before the ICMP header
    // makes sense.
    if (!s || p->tot_len < PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr)) return 0;
    if (!ip_addr_eq(addr, &s->target)) return 0;

    struct icmp_echo_hdr hdr;
    if (pbuf_copy_partial(p, &hdr, sizeof(hdr), PBUF_IP_HLEN) != sizeof(hdr)) return 0;
    if (ICMPH_TYPE(&hdr) != ICMP_ER) return 0;
    if (lwip_ntohs(hdr.id) != s->id) return 0;        // somebody else's ping
    if (lwip_ntohs(hdr.seqno) != s->seq) return 0;     // a late reply to an earlier one

    s->rtt_us = (uint32_t)(time_us_64() - s->sent_us);
    s->got    = true;
    pbuf_free(p);
    return 1;                        // consumed
}

static bool owner_gone(int pid) {
    const TaskInfo *ti = task_find(pid);
    return !ti || ti->state == TASK_DONE;
}

// Claim a slot and open its socket. The caller holds a NetOwn.
static PingSlot *ping_open(const ip_addr_t *target) {
    static uint8_t next_id;
    PingSlot *s = nullptr;
    for (int i = 0; i < PING_SLOTS; i++) {
        PingSlot *c = &g_pings[i];
        if (c->owner && owner_gone(c->owner)) {         // ended mid-ping
            if (c->pcb && net_radio_is_up()) { LwipLock lk; raw_remove(c->pcb); }
            c->pcb = nullptr;
            c->owner = 0;
        }
        if (!c->owner && !s) s = c;
    }
    if (!s || !net_radio_is_up()) return nullptr;
    s->rgen = net_radio_gen();
    s->got = false;
    s->seq = 0;
    s->target = *target;
    s->id = (uint16_t)((PING_ID & 0xFF00) | ++next_id);
    s->owner = task_self();
    {
        LwipLock lk;
        s->pcb = raw_new(IP_PROTO_ICMP);
        if (s->pcb) { raw_recv(s->pcb, ping_recv, s); raw_bind(s->pcb, IP_ADDR_ANY); }
    }
    if (!s->pcb) { s->owner = 0; return nullptr; }
    return s;
}

// One echo request. The network is held for the allocation and the send only.
// 0 sent, -1 out of packet buffers, -2 the radio could not be reached, or
// lwIP's error.
static int ping_send(PingSlot *s, uint16_t seq) {
    NetOwn own;
    if (!own.ok || !ping_live(s)) return -2;
    struct pbuf *p;
    { LwipLock lk; p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + PING_DATA_LEN, PBUF_RAM); }
    if (!p) return -1;

    auto *hdr = (struct icmp_echo_hdr *)p->payload;
    ICMPH_TYPE_SET(hdr, ICMP_ECHO);
    ICMPH_CODE_SET(hdr, 0);
    hdr->chksum = 0;
    hdr->id     = lwip_htons(s->id);
    hdr->seqno  = lwip_htons(seq);
    // A recognisable payload makes a capture readable, and matches what
    // every other ping puts on the wire.
    char *data = (char *)hdr + sizeof(struct icmp_echo_hdr);
    for (int b = 0; b < PING_DATA_LEN; b++) data[b] = (char)('a' + b % 23);
    hdr->chksum = inet_chksum(hdr, (u16_t)(sizeof(struct icmp_echo_hdr) + PING_DATA_LEN));

    s->got     = false;
    s->seq     = seq;
    s->sent_us = (uint32_t)time_us_64();
    err_t e;
    { LwipLock lk; e = raw_sendto(s->pcb, p, &s->target); pbuf_free(p); }
    return e == ERR_OK ? 0 : (int)e;
}

// The radio is about to go down: close every ping's socket while lwIP may
// still be entered. Called by radio_down holding the network.
void ping_radio_down(void) {
    LwipLock lk;
    for (int i = 0; i < PING_SLOTS; i++)
        if (g_pings[i].pcb) { raw_remove(g_pings[i].pcb); g_pings[i].pcb = nullptr; }
}

static void ping_close(PingSlot *s) {
    NetOwn own;
    if (!own.ok) return;             // left to be reclaimed once this task ends
    if (s->pcb && net_radio_is_up()) { LwipLock lk; raw_remove(s->pcb); }
    s->pcb = nullptr;
    s->owner = 0;
}

// Is this the device itself? Then the echo goes through lwIP's loopback, which
// delivers only when polled. Under a NetOwn.
static bool ping_is_self(const ip_addr_t *a) {
    if (ip_addr_isloopback(a)) return true;
    LwipLock lk;
    return netif_default && ip_addr_eq(a, &netif_default->ip_addr);
}

// The wait for a reply. For the device's own address it also hands lwIP the
// looped-back packets (the request, then the reply), a millisecond at a time.
static bool ping_wait(PingSlot *s, bool self, uint32_t ms) {
    if (!self) return wait_for(s->got, ms);
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (!s->got) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
        if (intr_check()) return false;
        {
            NetOwn own;
            if (own.ok && ping_live(s)) { LwipLock lk; netif_poll_all(); }
        }
        if (!s->got) task_sleep_ms(1);
    }
    return true;
}

static PingSlot *ping_start(const ip_addr_t *target) {
    NetOwn own;
    if (!own.ok) return nullptr;
    return ping_open(target);
}

// One echo request, for a package rather than for the screen.
//
// v1's speedtest measured latency by timing a TCP connect, and said why in a
// comment: MicroPython had no way to send an ICMP packet. This one does — the
// raw PCB above is the real thing — so a package gets the same measurement the
// `ping` command makes rather than an approximation of it.
//
// Returns the round trip in microseconds, or negative: -1 could not send, -2 no
// reply before the timeout.
int net_pkg_ping(const char *host, uint32_t timeout_ms) {
    if (!host || !net_is_connected()) return -1;
    ip_addr_t target;
    if (!resolve_host(host, &target)) return -1;     // takes the network itself
    PingSlot *s = ping_start(&target);
    if (!s) return -1;
    int rc = -1;
    bool self;
    { NetOwn own; self = own.ok && ping_is_self(&target); }
    if (ping_send(s, 1) == 0)
        rc = ping_wait(s, self, timeout_ms ? timeout_ms : 2000) ? (int)s->rtt_us : -2;
    ping_close(s);
    return rc;
}

// The network is NOT held for the run. It was, from the first line to the
// last, and nothing else could use it in the meantime: in an SSH session that
// included the server sending ping's own output, so four lines arrived at once
// at the end, and the gaps between pings were raw sleeps under the lock, which
// kept every other task off that core. Now it is taken to open the socket, for
// each send, and to close it; the waits yield and hold nothing.
static int cmd_ping(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: ping <host> [count]"); return 1; }
    if (!require_network()) return 1;

    uint32_t count = (argc >= 3) ? (uint32_t)strtoul(argv[2], nullptr, 10) : 4;
    if (count == 0 || count > 100) count = 4;

    ip_addr_t target;
    if (!resolve_host(argv[1], &target)) {
        out_err("Could not resolve '%s'.", argv[1]);
        return 1;
    }
    PingSlot *s = ping_start(&target);
    if (!s) { out_err("Could not open a raw socket (four pings may already be running)."); return 1; }
    bool self;
    { NetOwn own; self = own.ok && ping_is_self(&target); }

    char shown[IPADDR_STRLEN_MAX];
    ipaddr_ntoa_r(&target, shown, sizeof(shown));
    out_info("PING %s (%s): %d data bytes", argv[1], shown, PING_DATA_LEN);

    uint32_t sent = 0, recvd = 0, total_us = 0, best = 0xFFFFFFFFu, worst = 0;
    for (uint32_t i = 0; i < count; i++) {
        int e = ping_send(s, (uint16_t)(i + 1));
        if (e == -1) { out_err("Out of packet buffers."); break; }
        if (e == -2) { out_err("Could not reach the wireless chip from this task."); break; }
        if (e != 0) { out_warn("Send failed (%d).", e); continue; }
        sent++;

        if (ping_wait(s, self, 2000)) {
            uint32_t us = s->rtt_us;
            recvd++; total_us += us;
            if (us < best)  best  = us;
            if (us > worst) worst = us;
            out_multi("  %d bytes from %s: icmp_seq=%u time=%u.%03u ms",
                      PING_DATA_LEN, shown, (unsigned)(i + 1),
                      (unsigned)(us / 1000), (unsigned)(us % 1000));
        } else {
            out_multi("  Request timed out.  icmp_seq=%u", (unsigned)(i + 1));
        }
        out_flush();
        if (intr_check()) break;
        // Broken into slices so Ctrl+C lands during the gap between pings, not
        // just during the wait for a reply. task_sleep_ms, so the core is
        // free for everything else meanwhile.
        for (int slice = 0; slice < 7 && i + 1 < count; slice++) {
            if (intr_check()) break;
            task_sleep_ms(100);
        }
        if (intr_check()) break;
    }

    ping_close(s);

    out_blank();
    out_multi("  %u packets transmitted, %u received, %u%% packet loss",
              (unsigned)sent, (unsigned)recvd,
              (unsigned)(sent ? (sent - recvd) * 100 / sent : 0));
    if (recvd) {
        uint32_t avg = total_us / recvd;
        out_multi("  round-trip min/avg/max = %u.%03u/%u.%03u/%u.%03u ms",
                  (unsigned)(best / 1000),  (unsigned)(best % 1000),
                  (unsigned)(avg / 1000),   (unsigned)(avg % 1000),
                  (unsigned)(worst / 1000), (unsigned)(worst % 1000));
    }
    return recvd ? 0 : 1;
}

// --- ntp --------------------------------------------------------------------
//
// SNTP by hand rather than via lwIP's sntp app: this needs one reading, on
// demand, reported to the user — not a daemon that quietly steps the clock.

#define NTP_PORT       123
#define NTP_MSG_LEN    48
// Seconds between 1900-01-01 (the NTP epoch) and 1970-01-01 (the Unix epoch).
#define NTP_UNIX_DELTA 2208988800u

struct NtpState { volatile bool got; uint32_t unix_secs; };
static NtpState g_ntp;

static void ntp_recv(void *, struct udp_pcb *, struct pbuf *p,
                     const ip_addr_t *, u16_t port) {
    if (port == NTP_PORT && p->tot_len == NTP_MSG_LEN) {
        uint8_t mode;
        pbuf_copy_partial(p, &mode, 1, 0);
        uint8_t stratum;
        pbuf_copy_partial(p, &stratum, 1, 1);
        uint8_t ts[4];
        pbuf_copy_partial(p, ts, 4, 40);        // transmit timestamp, seconds
        uint32_t secs1900 = ((uint32_t)ts[0] << 24) | ((uint32_t)ts[1] << 16) |
                            ((uint32_t)ts[2] << 8) | ts[3];
        // Stratum 0 is a "kiss of death" packet, not a time source, and its
        // timestamp is meaningless.
        if (stratum != 0 && (mode & 0x07) == 4 && secs1900 > NTP_UNIX_DELTA) {
            g_ntp.unix_secs = secs1900 - NTP_UNIX_DELTA;
            g_ntp.got = true;
        }
    }
    pbuf_free(p);
}

static int ntp_sync(const char *server, bool set_clock) {
    if (!require_network()) return 1;

    // As for ping: one owner, from the resolve to the socket being removed.
    NetOwn own;
    if (!own.ok) { out_err("Could not reach the wireless chip from this task."); return 1; }

    ip_addr_t addr;
    if (!resolve_host(server, &addr)) { out_err("Could not resolve '%s'.", server); return 1; }

    struct udp_pcb *pcb;
    {
        LwipLock lk;
        pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
        if (pcb) udp_recv(pcb, ntp_recv, nullptr);
    }
    if (!pcb) { out_err("Could not open a UDP socket."); return 1; }

    g_ntp.got = false;
    out_info("Querying %s (%s)...", server, ipaddr_ntoa(&addr));

    bool sent = false;
    for (int attempt = 0; attempt < 3 && !g_ntp.got; attempt++) {
        {
            LwipLock lk;
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
            if (p) {
                uint8_t *req = (uint8_t *)p->payload;
                memset(req, 0, NTP_MSG_LEN);
                req[0] = 0x1b;              // LI = 0, VN = 3, Mode = 3 (client)
                sent = (udp_sendto(pcb, p, &addr, NTP_PORT) == ERR_OK) || sent;
                pbuf_free(p);
            }
        }
        wait_for(g_ntp.got, 2500);
    }

    { LwipLock lk; udp_remove(pcb); }

    if (!sent)      { out_err("Could not send the request."); return 1; }
    if (!g_ntp.got) { out_err("No reply from %s.", server); return 1; }

    time_t e = (time_t)g_ntp.unix_secs;
    struct tm *utc = gmtime(&e);
    if (!utc) { out_err("Bad time from the server."); return 1; }

    if (!set_clock) {
        out_multi("  %04d-%02d-%02d %02d:%02d:%02d UTC",
                  utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                  utc->tm_hour, utc->tm_min, utc->tm_sec);
        return 0;
    }

    struct tm t = *utc;
    t.tm_isdst = 0;
    if (!aon_timer_set_time_calendar(&t)) { out_err("Could not set the clock."); return 1; }
    // The clock is now trustworthy, which is what lets file timestamps be
    // recorded — the same flag `date set` writes.
    reg_set("System.Clock_Set", "true");
    clock_persist();        // remember it, so a cold boot comes up near here

    int off = (int)reg_get_int("System.TZ_Offset", 0);
    out_ok("Clock synced.");
    out_multi("  %04d-%02d-%02d %02d:%02d:%02d UTC%s",
              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
              t.tm_hour, t.tm_min, t.tm_sec, off ? "  (offset applied on display)" : "");
    return 0;
}

static int cmd_ntp(int argc, char **argv) {
    const char *server = reg_get("System.NTP_Server", "pool.ntp.org");
    if (argc >= 2 && !strcmp(argv[1], "server")) {
        if (argc < 3) { out_multi("  NTP server : %s", server); return 0; }
        reg_set("System.NTP_Server", argv[2]);
        out_ok("NTP server set to '%s'.", argv[2]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "show")) return ntp_sync(server, /*set*/false);
    if (argc >= 2 && strcmp(argv[1], "sync") != 0) {
        out_multi("Usage: ntp [sync] | ntp show | ntp server [host]");
        return 1;
    }
    return ntp_sync(server, /*set*/true);
}


// --- lanscan: who else is on this network -------------------------------------
//
// An ARP sweep: ask every address in the subnet "who has this?" and list the
// ones that answer, with their hardware address, then try one TCP port on each
// (22 unless told otherwise) so a computer running SSH stands out. ARP is what
// finds them, not ping: plenty of computers drop ICMP and still answer ARP,
// because nothing on a local network works without it.
//
// In batches, because lwIP's ARP table holds ARP_TABLE_SIZE (32, lwipopts.h)
// entries and a request past that pushes the oldest out: 24 at a time, read back
// before the next 24 are asked. Twice: a phone asleep on Wi-Fi power saving can
// take longer than one wait to answer, so the second pass asks again only the
// addresses that were silent. NetOwn per batch, not for the whole sweep, so a
// download on the other core waits half a second at most, not the whole scan.

#define LAN_BATCH       24
#define LAN_WAIT_MS     500
#define LAN_PASSES      2
#define LAN_MAX_FOUND   64
#define LAN_MAX_HOSTS   1022        // a /22; anything wider is not "this network"
#define LAN_PROBE_MS    400

struct LanHost { uint32_t ip; uint8_t mac[6]; int8_t port_open; };   // -1 not tried

struct LanProbe {
    struct tcp_pcb *pcb;
    volatile bool done;
    volatile bool open;
};

static err_t lan_connected(void *arg, struct tcp_pcb *pcb, err_t) {
    LanProbe *p = (LanProbe *)arg;
    p->open = true;
    p->done = true;
    p->pcb = nullptr;
    tcp_arg(pcb, nullptr);
    tcp_err(pcb, nullptr);
    tcp_abort(pcb);              // a reset: nothing is sent to the service
    return ERR_ABRT;
}

static void lan_err(void *arg, err_t) {
    LanProbe *p = (LanProbe *)arg;
    if (!p) return;
    p->pcb = nullptr;            // lwIP has freed it
    p->done = true;              // refused (RST) or unreachable
}

// 1 open, 0 closed or silent, -1 could not try.
static int lan_probe(uint32_t ip_be, uint16_t port) {
    LanProbe pr{nullptr, false, false};
    {
        NetOwn own;
        if (!own.ok) return -1;
        LwipLock lk;
        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) return -1;
        pr.pcb = pcb;
        tcp_arg(pcb, &pr);
        tcp_err(pcb, lan_err);
        ip_addr_t a;
        ip_addr_set_ip4_u32(&a, ip_be);
        if (tcp_connect(pcb, &a, port, lan_connected) != ERR_OK) {
            tcp_arg(pcb, nullptr);
            tcp_err(pcb, nullptr);
            tcp_abort(pcb);
            return -1;
        }
    }
    absolute_time_t deadline = make_timeout_time_ms(LAN_PROBE_MS);
    while (!pr.done && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (intr_check()) break;
        task_sleep_ms(5);
    }
    // Still waiting: nobody answered. Withdraw the attempt so no callback can
    // reach `pr` once this returns.
    {
        NetOwn own;
        LwipLock lk;
        if (!pr.done && pr.pcb) {
            tcp_arg(pr.pcb, nullptr);
            tcp_err(pr.pcb, nullptr);
            tcp_abort(pr.pcb);
            pr.pcb = nullptr;
        }
    }
    return pr.open ? 1 : 0;
}

static void lan_ip_str(uint32_t ip_be, char *out, size_t cap) {
    ip4_addr_t a;
    ip4_addr_set_u32(&a, ip_be);
    snprintf(out, cap, "%s", ip4addr_ntoa(&a));
}

static int cmd_lanscan(int argc, char **argv) {
    uint16_t port = 22;
    bool probe = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v < 1 || v > 65535) { out_err("Port must be 1 to 65535."); return 1; }
            port = (uint16_t)v;
        } else if (!strcmp(argv[i], "-n")) {
            probe = false;
        } else {
            out_multi("Usage: lanscan [-p port] [-n]");
            out_multi("  Lists the devices on this Wi-Fi network: address, hardware address,");
            out_multi("  and whether port 22 (SSH) answers. -p checks another port, -n none.");
            return 1;
        }
    }
    if (!net_is_connected()) { out_err("Not connected. 'wifi connect <ssid>' first."); return 1; }

    uint32_t self = 0, mask = 0, gw = 0;
    uint8_t self_mac[6] = {0};
    {
        NetOwn own;
        if (!own.ok) { out_err("The network is not up."); return 1; }
        LwipLock lk;
        struct netif *n = netif_default;
        if (!n) { out_err("No network interface."); return 1; }
        self = ip4_addr_get_u32(netif_ip4_addr(n));
        mask = ip4_addr_get_u32(netif_ip4_netmask(n));
        gw   = ip4_addr_get_u32(netif_ip4_gw(n));
        memcpy(self_mac, n->hwaddr, 6);
    }
    uint32_t hmask = lwip_ntohl(mask), hself = lwip_ntohl(self);
    uint32_t net = hself & hmask, hosts = ~hmask;          // host part, all ones = broadcast
    if (hosts < 2 || hosts - 1 > LAN_MAX_HOSTS) {
        out_err("This network has %lu addresses; lanscan sweeps up to %d.",
                (unsigned long)(hosts > 0 ? hosts - 1 : 0), LAN_MAX_HOSTS);
        return 1;
    }

    LanHost *list = (LanHost *)malloc(sizeof(LanHost) * LAN_MAX_FOUND);
    if (!list) { out_err("Not enough memory (heap fragmented)."); return 1; }
    int nfound = 0;
    bool stopped = false;

    char me[16];
    lan_ip_str(self, me, sizeof(me));
    out_info("Looking for devices on %s/%d (%lu addresses)...", me,
             __builtin_popcount(hmask), (unsigned long)(hosts - 1));

    // One bit per address: answered already, so the second pass skips it.
    uint8_t *seen = (uint8_t *)calloc((hosts + 7) / 8, 1);
    if (!seen) { free(list); out_err("Not enough memory (heap fragmented)."); return 1; }
    for (int pass = 0; pass < LAN_PASSES && !stopped; pass++) {
        uint32_t h = 1;
        while (h < hosts && !stopped) {
            // The next batch of addresses still to ask about.
            uint32_t batch[LAN_BATCH];
            int nb = 0;
            while (h < hosts && nb < LAN_BATCH) {
                uint32_t ip = lwip_htonl(net | h);
                if (ip != self && !(seen[h / 8] & (1u << (h % 8)))) batch[nb++] = h;
                h++;
            }
            if (!nb) break;
            {
                NetOwn own;
                if (!own.ok) { stopped = true; break; }
                LwipLock lk;
                for (int k = 0; k < nb; k++) {
                    ip4_addr_t a; ip4_addr_set_u32(&a, lwip_htonl(net | batch[k]));
                    etharp_request(netif_default, &a);
                }
            }
            for (uint32_t waited = 0; waited < LAN_WAIT_MS; waited += 20) {
                if (intr_check()) { stopped = true; break; }
                task_sleep_ms(20);
            }
            NetOwn own;
            if (!own.ok) { stopped = true; break; }
            LwipLock lk;
            for (int k = 0; k < nb && nfound < LAN_MAX_FOUND; k++) {
                ip4_addr_t a; ip4_addr_set_u32(&a, lwip_htonl(net | batch[k]));
                struct eth_addr *eth = nullptr;
                const ip4_addr_t *got = nullptr;
                if (etharp_find_addr(netif_default, &a, &eth, &got) >= 0 && eth) {
                    seen[batch[k] / 8] |= (uint8_t)(1u << (batch[k] % 8));
                    list[nfound].ip = ip4_addr_get_u32(&a);
                    memcpy(list[nfound].mac, eth->addr, 6);
                    list[nfound].port_open = -1;
                    nfound++;
                }
            }
        }
    }
    free(seen);
    // Found in two passes, so out of order: by address.
    for (int i = 1; i < nfound; i++) {
        LanHost t = list[i];
        int j = i - 1;
        while (j >= 0 && lwip_ntohl(list[j].ip) > lwip_ntohl(t.ip)) { list[j + 1] = list[j]; j--; }
        list[j + 1] = t;
    }
    // This device last, so the count above is of the others.
    if (nfound < LAN_MAX_FOUND) {
        list[nfound].ip = self;
        memcpy(list[nfound].mac, self_mac, 6);
        list[nfound].port_open = -1;
        nfound++;
    }

    if (probe && !stopped) {
        for (int i = 0; i < nfound - 1; i++) {          // not this device
            if (intr_check()) { stopped = true; break; }
            list[i].port_open = (int8_t)lan_probe(list[i].ip, port);
        }
    }

    out_blank();
    out_multi("  %s%-16s %-18s%s", C_GRAY, "ADDRESS", "HARDWARE", C_RESET);
    for (int i = 0; i < nfound; i++) {
        char ip[16];
        lan_ip_str(list[i].ip, ip, sizeof(ip));
        char note[48] = "";
        bool is_self = (i == nfound - 1);
        if (is_self)               snprintf(note, sizeof(note), "this device");
        else if (list[i].ip == gw) snprintf(note, sizeof(note), "router");
        char portw[24] = "";
        if (!is_self && list[i].port_open == 1)
            snprintf(portw, sizeof(portw), "%s%s%s", C_GREEN, port == 22 ? "ssh" : "open", C_RESET);
        else if (!is_self && list[i].port_open == 0 && probe)
            snprintf(portw, sizeof(portw), "%s-%s", C_GRAY, C_RESET);
        const uint8_t *m = list[i].mac;
        out_multi("  %-16s %02x:%02x:%02x:%02x:%02x:%02x  %s%s%s",
                  ip, m[0], m[1], m[2], m[3], m[4], m[5],
                  portw, (portw[0] && note[0]) ? "  " : "", note);
    }
    out_blank();
    int open = 0;
    for (int i = 0; i < nfound - 1; i++) if (list[i].port_open == 1) open++;
    if (stopped) out_warn("Stopped; the list is what had answered so far.");
    if (nfound - 1 >= LAN_MAX_FOUND - 1) out_warn("Showing the first %d devices.", LAN_MAX_FOUND - 1);
    if (probe)
        out_info("%d other device%s; %d answer%s on port %u.", nfound - 1, nfound - 1 == 1 ? "" : "s",
                 open, open == 1 ? "s" : "", (unsigned)port);
    else
        out_info("%d other device%s.", nfound - 1, nfound - 1 == 1 ? "" : "s");
    free(list);
    return 0;
}

void netapps_register(void) {
    static const Command cmds[] = {
        {"ping",     "ping <host> [count]",     cmd_ping,     nullptr},
        {"nslookup", "nslookup <host>",         cmd_nslookup, nullptr},
        {"ntp",      "sync the clock over NTP", cmd_ntp,      nullptr},
        {"lanscan",  "devices on this network", cmd_lanscan,  nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
    cmd_alias("ns", "nslookup");
}
