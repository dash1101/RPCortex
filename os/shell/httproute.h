// Which way an HTTP request leaves the device.
//
// Two transports can carry one, and an image has at most one of them:
//
//   * the network (httptransport.cpp): lwIP and mbedTLS over Wi-Fi, in an image
//     with Wi-Fi, once it is connected;
//   * the bridge (bridge.cpp): the host on the other end of the console fetches
//     it (LINK.md, protocol 3), in an image without Wi-Fi, once rpc or the browser
//     tool has offered it.
//
// The HTTP client itself (core/httpfetch.cpp) and everything on top of it, wget,
// curl, runurl, pkg's repository half, update's network half and the packages'
// fw_http_* calls, is in every image and asks here. Each feature file answers for
// its own transport; the one an image leaves out has a stand-in that says no.
#ifndef RPC_HTTPROUTE_H
#define RPC_HTTPROUTE_H

#include "httpfetch.h"

enum HttpVia { HTTP_VIA_NONE = 0, HTTP_VIA_NET, HTTP_VIA_BRIDGE };

// The way a request can go right now, with `t` filled in for it. NONE when there
// is none; http_no_route then says why.
HttpVia http_route(HttpTransport *t);

// Say, as an error, why nothing can be fetched: not connected to Wi-Fi, or (no
// Wi-Fi) no host has offered the bridge, or this is not the console's command.
void http_no_route(void);

// Whether `url` can be fetched over `via` with its certificate checked, saying
// why not when it cannot. Over the bridge the host checks it, so always yes.
bool http_https_ready(const char *url, HttpVia via);

// What the last transfer did, for a failure message worth reading.
void http_last_detail(char *out, unsigned cap);

// --- answered by the feature files ---------------------------------------------

// httptransport.cpp, or net_off.cpp: false without a connection.
bool http_net_transport_get(HttpTransport *t);
void http_net_last_detail(char *out, unsigned cap);
void http_net_no_route(void);
bool http_tls_available(void);
const char *http_tls_why(void);

// bridge.cpp in an image without Wi-Fi; httptransport.cpp otherwise, where it
// is always false (that image has a network of its own).
bool bridge_transport_get(HttpTransport *t);
const char *bridge_last_detail(void);
bool bridge_available(void);             // the image can bridge at all (HELLO's bridge=1)
bool bridge_offered(void);
// `on` from link's BRIDGE verb. False, changing nothing, in an image that cannot.
bool bridge_set_offer(bool on);
void bridge_no_route(void);

#endif  // RPC_HTTPROUTE_H
