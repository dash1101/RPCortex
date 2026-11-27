// lwIP configuration for RPCortex v2.
//
// A DHCP client and outbound TCP/UDP, nothing more. lwIP's defaults assume a
// machine with memory to spare; every value here is sized for a device where the
// heap is the scarce resource, which is the same reasoning that shaped v1's
// network layer. NO_SYS=1 because there is no RTOS — the cyw43 background
// context drives the stack.

#ifndef RPC_LWIPOPTS_H
#define RPC_LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

// One arena, allocated statically. A pool that cannot grow is the point: an
// exhausted pool drops a packet, where a growing one would take the heap the
// shell needs with it.
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
// TLS needs materially more than plain TCP: handshake flights and 16 KB
// records both come through here. 4000 was sized for ping and NTP.
#define MEM_SIZE                    16000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
// Received data waits here until the reader takes it. Do NOT shrink this on the
// strength of lwIP's own statistics: on a Pico 2 W (v2.0.677) MEMP_STATS put the
// pool's high-water mark at 14 of 32 under every load tried (an 848 KB HTTPS
// download, two at once, a download beside an SSH session streaming 650 KB, a
// speedtest), yet 20 buffers cut the speedtest from 2.2 to 1.2 Mbit/s and 26
// to 2.0 (four runs each). 32 stays: 9 KB is not worth 9% of the throughput.
// MEM_SIZE above is not oversized either: that SSH test took it to 15.5 of 16 KB.
#define PBUF_POOL_SIZE              32

#define LWIP_ARP                    1
// 32, not lwIP's 10: `lanscan` asks a whole batch of addresses at once and reads
// the answers back from this table, and a request for the eleventh pushed the
// first out before it could be read. About 20 bytes an entry.
#define ARP_TABLE_SIZE              32
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0

// The receive window MUST be larger than one TLS record, and that is not a
// tuning preference — it is a deadlock if it is not.
//
// A TLS record carries up to 16384 bytes of plaintext, plus a 5-byte header and
// up to ~256 of MAC, padding and tag: about 16.6 KB on the wire. mbedtls cannot
// decrypt a PARTIAL record, so if the window is smaller than one record the
// sequence is:
//
//   the server fills the window with part of a record
//   mbedtls has no complete record, so produces no plaintext
//   nothing is consumed, so the window never advances
//   the server cannot send the rest
//
// Both sides then wait forever. It is invisible for small responses, which
// arrive in small records — which is exactly how it presented: a 1 KB manifest
// downloaded fine and a 694 KB image stopped dead at 923 bytes, one byte past
// the end of its 922-byte header block.
//
// 12 * 1460 = 17520, comfortably past a worst-case record. Do not reduce this
// below 16 KB while MBEDTLS_SSL_IN_CONTENT_LEN is 16384; the two are a pair.
#define TCP_WND                     (12 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETCONN_FULLDUPLEX     0
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

// Checksums are computed in hardware on the CYW43 path for the protocols that
// matter, but leaving them on costs little and catches a corrupted frame.
#define LWIP_CHKSUM_ALGORITHM       3

#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

#ifndef NDEBUG
// The driver is noisy and the shell is not.
//
// Turned on once, with ALTCP_MBEDTLS_DEBUG, to find out why a handshake was
// failing. It printed nothing — the answer turned out to be that the handshake
// was not failing at all, only running out of time — so it bought no
// information and is off again rather than left in on the strength of an
// intention.
#define LWIP_DEBUG                  0
#endif

// --- TLS --------------------------------------------------------------------
//
// altcp is lwIP's pluggable transport layer: the same tcp_* shaped API with a
// TLS implementation slotted underneath, so the HTTP transport calls one set of
// functions and the only difference between http:// and https:// is which
// allocator made the connection.
#define LWIP_ALTCP                  1
#define LWIP_ALTCP_TLS              1
#define LWIP_ALTCP_TLS_MBEDTLS      1
// A server certificate that does not verify ends the handshake (lwIP's default
// is OPTIONAL, which lets it through). httptransport.cpp sets the same on its
// config and checks the result again before sending anything.
#define ALTCP_MBEDTLS_AUTHMODE      MBEDTLS_SSL_VERIFY_REQUIRED

// Packets to the device's own address (and 127.0.0.1) are looped back instead
// of going nowhere, so `ping <own address>` answers as it does on a computer.
// Nothing here is a thread, so lwIP delivers looped packets only when
// netif_poll_all() is called; ping does that while it waits on such a target.
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_HAVE_LOOPIF            1
#define LWIP_LOOPBACK_MAX_PBUFS     8

#endif  // RPC_LWIPOPTS_H
