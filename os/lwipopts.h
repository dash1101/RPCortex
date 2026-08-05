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
// Headroom for a large download. Received data is held until the reader takes
// it, and a firmware image is nearly 700 KB — 24 was sized for ping and NTP.
#define PBUF_POOL_SIZE              32

#define LWIP_ARP                    1
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
// Off in general — the driver is noisy and the shell is not — but the TLS
// layer is exempt.
//
// A failed handshake reaches the shell as "the request failed", because the
// only thing lwIP hands back is an aborted connection: mbedtls's actual reason
// is logged inside altcp_tls_mbedtls.c and thrown away when debugging is off.
// Four rounds were spent inferring that reason from the outside.
//
// ALTCP_MBEDTLS_DEBUG alone is quiet in normal use: it prints on handshake
// completion and on error, not per packet. That is worth having permanently.
#define LWIP_DEBUG                  1
#define ALTCP_MBEDTLS_DEBUG         LWIP_DBG_ON

// Everything else stays off. LWIP_DEBUG=1 only ENABLES the machinery; each
// subsystem is still gated by its own flag, and the defaults are all OFF.
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

#endif  // RPC_LWIPOPTS_H
