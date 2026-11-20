// TCP sockets as small integer handles (nettcp.cpp). The package ABI's five
// calls, and the firmware-only ones the SSH server and client use.
//
// Every call takes the network for itself (netown.h) and gives it back before
// returning, so none of them may be called with a lock held that a network
// operation could be waiting for — in practice, never from inside a
// filesystem walk.
#ifndef RPC_NETTCP_H
#define RPC_NETTCP_H

#include <stdint.h>

int      net_pkg_tcp_listen(unsigned port);
int      net_pkg_tcp_accept(int listener, uint32_t timeout_ms);
int      net_pkg_tcp_recv(int handle, void *buf, unsigned cap, uint32_t timeout_ms);
int      net_pkg_tcp_send(int handle, const void *buf, unsigned len);
int      net_pkg_tcp_close(int handle);
unsigned net_pkg_tcp_open(void);

// One look each, no waiting. accept: a handle, 0 for none waiting, -1 on error.
// read: bytes read, 0 for none yet, -1 when the peer has gone. write: bytes
// queued (0 when there is no room yet), -1 on error.
int  net_tcp_accept_now(int listener);
int  net_tcp_read_now(int handle, void *buf, unsigned cap);
int  net_tcp_write_now(int handle, const void *buf, unsigned len);
bool net_tcp_drained(int handle);
void net_tcp_adopt(int handle);
void net_tcp_peer(int handle, char *out, unsigned cap);   // "192.168.1.5"
// Resolve and connect, waiting up to timeout_ms (Ctrl+C gives up). A handle or -1.
int  net_tcp_connect(const char *host, unsigned port, uint32_t timeout_ms);

#endif  // RPC_NETTCP_H
