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

#include "kernel.h"
#include "command.h"
#include "out.h"
#include "registry.h"
#include "interrupt.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/aon_timer.h"
#include "lwip/dns.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

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

struct PingState {
    volatile bool got;
    uint16_t      seq;
    uint32_t      sent_us;
    uint32_t      rtt_us;
    ip_addr_t     target;
};
static PingState g_ping;

static u8_t ping_recv(void *, struct raw_pcb *, struct pbuf *p, const ip_addr_t *addr) {
    // The payload starts after the IP header; lwIP hands the raw PCB the whole
    // datagram, so the IP header has to be stepped over before the ICMP header
    // makes sense.
    if (p->tot_len < PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr)) return 0;
    if (!ip_addr_eq(addr, &g_ping.target)) return 0;

    struct icmp_echo_hdr hdr;
    if (pbuf_copy_partial(p, &hdr, sizeof(hdr), PBUF_IP_HLEN) != sizeof(hdr)) return 0;
    if (ICMPH_TYPE(&hdr) != ICMP_ER) return 0;
    if (lwip_ntohs(hdr.id) != PING_ID) return 0;
    if (lwip_ntohs(hdr.seqno) != g_ping.seq) return 0;

    g_ping.rtt_us = (uint32_t)(time_us_64() - g_ping.sent_us);
    g_ping.got    = true;
    pbuf_free(p);
    return 1;                        // consumed
}

// Scoped form of the begin/end pair this file uses elsewhere. The function
// below has several exits and a paired call at each one is a lock left held the
// first time somebody adds another.
struct LwipLock {
    LwipLock()  { cyw43_arch_lwip_begin(); }
    ~LwipLock() { cyw43_arch_lwip_end(); }
};

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
    if (!resolve_host(host, &g_ping.target)) return -1;

    struct raw_pcb *pcb;
    {
        LwipLock lk;
        pcb = raw_new(IP_PROTO_ICMP);
        if (pcb) { raw_recv(pcb, ping_recv, nullptr); raw_bind(pcb, IP_ADDR_ANY); }
    }
    if (!pcb) return -1;

    // A sequence number that moves, so a late reply to the PREVIOUS call is not
    // counted as the answer to this one — which would report a round trip of
    // almost nothing and make a slow link look fast.
    static uint16_t seq;
    seq++;

    int rc = -1;
    {
        struct pbuf *p;
        { LwipLock lk; p = pbuf_alloc(PBUF_IP,
                                     sizeof(struct icmp_echo_hdr) + PING_DATA_LEN,
                                     PBUF_RAM); }
        if (p) {
            auto *hdr = (struct icmp_echo_hdr *)p->payload;
            ICMPH_TYPE_SET(hdr, ICMP_ECHO);
            ICMPH_CODE_SET(hdr, 0);
            hdr->chksum = 0;
            hdr->id     = lwip_htons(PING_ID);
            hdr->seqno  = lwip_htons(seq);
            char *data = (char *)hdr + sizeof(struct icmp_echo_hdr);
            for (int b = 0; b < PING_DATA_LEN; b++) data[b] = (char)('a' + b % 23);
            hdr->chksum = inet_chksum(hdr, (u16_t)(sizeof(struct icmp_echo_hdr) +
                                                   PING_DATA_LEN));

            g_ping.got     = false;
            g_ping.seq     = seq;
            g_ping.sent_us = (uint32_t)time_us_64();

            err_t e;
            { LwipLock lk; e = raw_sendto(pcb, p, &g_ping.target); pbuf_free(p); }
            if (e == ERR_OK)
                rc = wait_for(g_ping.got, timeout_ms ? timeout_ms : 2000)
                         ? (int)g_ping.rtt_us : -2;
        }
    }

    { LwipLock lk; raw_remove(pcb); }
    return rc;
}

static int cmd_ping(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: ping <host> [count]"); return 1; }
    if (!require_network()) return 1;

    uint32_t count = (argc >= 3) ? (uint32_t)strtoul(argv[2], nullptr, 10) : 4;
    if (count == 0 || count > 100) count = 4;

    if (!resolve_host(argv[1], &g_ping.target)) {
        out_err("Could not resolve '%s'.", argv[1]);
        return 1;
    }

    cyw43_arch_lwip_begin();
    struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
    if (pcb) { raw_recv(pcb, ping_recv, nullptr); raw_bind(pcb, IP_ADDR_ANY); }
    cyw43_arch_lwip_end();
    if (!pcb) { out_err("Could not open a raw socket."); return 1; }

    out_info("PING %s (%s): %d data bytes", argv[1], ipaddr_ntoa(&g_ping.target),
             PING_DATA_LEN);

    uint32_t sent = 0, recvd = 0, total_us = 0, best = 0xFFFFFFFFu, worst = 0;
    for (uint32_t i = 0; i < count; i++) {
        struct pbuf *p;
        cyw43_arch_lwip_begin();
        p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + PING_DATA_LEN, PBUF_RAM);
        cyw43_arch_lwip_end();
        if (!p) { out_err("Out of packet buffers."); break; }

        auto *hdr = (struct icmp_echo_hdr *)p->payload;
        ICMPH_TYPE_SET(hdr, ICMP_ECHO);
        ICMPH_CODE_SET(hdr, 0);
        hdr->chksum = 0;
        hdr->id     = lwip_htons(PING_ID);
        hdr->seqno  = lwip_htons((uint16_t)(i + 1));
        // A recognisable payload makes a capture readable, and matches what
        // every other ping puts on the wire.
        char *data = (char *)hdr + sizeof(struct icmp_echo_hdr);
        for (int b = 0; b < PING_DATA_LEN; b++) data[b] = (char)('a' + b % 23);
        hdr->chksum = inet_chksum(hdr, (u16_t)(sizeof(struct icmp_echo_hdr) + PING_DATA_LEN));

        g_ping.got     = false;
        g_ping.seq     = (uint16_t)(i + 1);
        g_ping.sent_us = (uint32_t)time_us_64();

        cyw43_arch_lwip_begin();
        err_t e = raw_sendto(pcb, p, &g_ping.target);
        pbuf_free(p);
        cyw43_arch_lwip_end();
        if (e != ERR_OK) { out_warn("Send failed (%d).", (int)e); continue; }
        sent++;

        if (wait_for(g_ping.got, 2000)) {
            uint32_t us = g_ping.rtt_us;
            recvd++; total_us += us;
            if (us < best)  best  = us;
            if (us > worst) worst = us;
            out_multi("  %d bytes from %s: icmp_seq=%u time=%u.%03u ms",
                      PING_DATA_LEN, ipaddr_ntoa(&g_ping.target), (unsigned)(i + 1),
                      (unsigned)(us / 1000), (unsigned)(us % 1000));
        } else {
            out_multi("  Request timed out.  icmp_seq=%u", (unsigned)(i + 1));
        }
        if (intr_check()) break;
        // Broken into slices so Ctrl+C lands during the gap between pings, not
        // just during the wait for a reply.
        for (int slice = 0; slice < 7 && i + 1 < count; slice++) {
            if (intr_check()) break;
            sleep_ms(100);
        }
        if (intr_check()) break;
    }

    cyw43_arch_lwip_begin();
    raw_remove(pcb);
    cyw43_arch_lwip_end();

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

    ip_addr_t addr;
    if (!resolve_host(server, &addr)) { out_err("Could not resolve '%s'.", server); return 1; }

    cyw43_arch_lwip_begin();
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb) udp_recv(pcb, ntp_recv, nullptr);
    cyw43_arch_lwip_end();
    if (!pcb) { out_err("Could not open a UDP socket."); return 1; }

    g_ntp.got = false;
    out_info("Querying %s (%s)...", server, ipaddr_ntoa(&addr));

    bool sent = false;
    for (int attempt = 0; attempt < 3 && !g_ntp.got; attempt++) {
        cyw43_arch_lwip_begin();
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, NTP_MSG_LEN, PBUF_RAM);
        if (p) {
            uint8_t *req = (uint8_t *)p->payload;
            memset(req, 0, NTP_MSG_LEN);
            req[0] = 0x1b;              // LI = 0, VN = 3, Mode = 3 (client)
            sent = (udp_sendto(pcb, p, &addr, NTP_PORT) == ERR_OK) || sent;
            pbuf_free(p);
        }
        cyw43_arch_lwip_end();
        wait_for(g_ntp.got, 2500);
    }

    cyw43_arch_lwip_begin();
    udp_remove(pcb);
    cyw43_arch_lwip_end();

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

#else   // no radio on this board

int net_pkg_ping(const char *, uint32_t) { return -1; }

static int cmd_ping(int, char **)     { out_err("This board has no wireless hardware."); return 1; }
static int cmd_nslookup(int, char **) { out_err("This board has no wireless hardware."); return 1; }
static int cmd_ntp(int, char **)      { out_err("This board has no wireless hardware."); return 1; }

#endif

void netapps_register(void) {
    static const Command cmds[] = {
        {"ping",     "ping <host> [count]",     cmd_ping,     nullptr},
        {"nslookup", "nslookup <host>",         cmd_nslookup, nullptr},
        {"ntp",      "sync the clock over NTP", cmd_ntp,      nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
    cmd_alias("ns", "nslookup");
}
