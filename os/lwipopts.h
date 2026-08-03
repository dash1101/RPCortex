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
#define PBUF_POOL_SIZE              24

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

#define TCP_WND                     (8 * TCP_MSS)
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
#define LWIP_DEBUG                  0      // the driver is noisy; the shell is not
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
