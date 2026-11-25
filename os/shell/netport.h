// The network port: the bridge on its own USB serial interface (netport.cpp).
#ifndef RPC_NETPORT_H
#define RPC_NETPORT_H

#include "httpfetch.h"

// From the usb task, every turn: moves bytes between the port and its rings.
void netport_pump(void);
// The computer opened (true) or closed the port.
void netport_line_state(bool dtr);
// A computer holds the port and has offered its connection.
bool netport_offered(void);
// The transport, when offered. Any task may use it, one fetch at a time.
bool netport_transport_get(HttpTransport *t);
const char *netport_last_detail(void);

#endif
