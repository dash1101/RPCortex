// The network bridge: HTTP through the host a board is plugged into (LINK.md,
// "The network bridge", protocol 3).
//
// A board with no network of its own (a Pico, a Pico 2, an image built without
// Wi-Fi) has no TCP/IP stack and no TLS. When rpc or the browser tool is on the
// other end of its console, those tools can fetch a URL for it. This is the
// device's half: an HttpTransport, the same four calls lwIP's transport provides,
// so httpfetch.cpp drives it unchanged and everything built on that (wget, curl,
// pkg install, update, a package's fw_http_*) gets redirects, size caps, sinks
// and progress without knowing which way the bytes came.
//
// It is a socket only in shape. `open` remembers where; `send` collects the
// request httpfetch built; the first `recv` turns that request into FETCH frames,
// waits for the host's answer and hands back a response in HTTP/1.1 form, made up
// here from the status, the headers and the body the host sent. The parser on top
// cannot tell the difference, and that is the point: a truncated body, a 404, a
// full filesystem all fail exactly the way they fail over Wi-Fi.
//
// Pure, behind BridgeIo, so os/host/bridge_test.cpp runs it against a fake host.
// os/shell/bridge.cpp connects it to the console.
#ifndef RPC_BRIDGE_H
#define RPC_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "httpfetch.h"

#define BRIDGE_CHUNK      768      // bytes in one FDATA line (1024 base64 characters)
#define BRIDGE_QUIET_MS   10000    // no line from the host for this long: it is not there
#define BRIDGE_DRAIN_MS   3000     // after FSTOP, how long to wait for the host's FDONE
#define BRIDGE_LINE_MAX   1104     // one line from the host, as link mode reads them
#define BRIDGE_REQ_MAX    3072     // the request httpfetch builds, body included
#define BRIDGE_HDR_MAX    200      // one response header passed on by the host

// What the bridge needs from the platform. None of it may block.
struct BridgeIo {
    // One whole frame: 0x1E first and '\n' last, written in one piece.
    void     (*write)(void *ctx, const char *data, uint32_t len);
    // One byte from the host, or -1 when nothing is waiting.
    int      (*getc)(void *ctx);
    uint32_t (*now_ms)(void *ctx);
    // True when the fetch should stop: Ctrl+C, or the task was asked to end.
    bool     (*stop)(void *ctx);
    // Nothing is waiting: let something else run for a moment.
    void     (*idle)(void *ctx);
    // The host stopped answering, so the offer should go (see LINK.md).
    void     (*lost)(void *ctx);
    void *ctx;
};

struct BridgeConn {
    BridgeIo io;
    uint32_t next_id;               // the id the next fetch takes; never 0
    uint32_t id;                    // the fetch in flight

    // Where httpfetch asked to connect.
    char     host[HTTP_HOST_MAX];
    uint16_t port;
    bool     tls;

    // One heap block for the length of a hop: the request while it is being
    // collected, then the line being read, the chunk being handed out and the
    // response head made up from FHEAD, plus a frame for writing throughout.
    uint8_t *mem;
    uint32_t req_len;
    uint8_t  phase;                 // see bridge.cpp

    uint32_t head_len, head_off;    // the made-up status line and headers
    uint32_t data_len, data_off;    // the FDATA chunk being handed out
    uint64_t got;                   // body bytes received
    int64_t  len;                   // FHEAD's len, -1 when not given
    bool     ack_due;               // a chunk was handed out and not yet acknowledged
    bool     ended;                 // FDONE arrived
    bool     failed;

    // Why the last fetch failed, for a person (http_last_detail).
    char     detail[120];
};

// Ready a connection. `io` is copied; next_id starts at 1.
void bridge_init(BridgeConn *c, const BridgeIo *io);

// The four calls, with `c` as their context.
void bridge_transport(BridgeConn *c, HttpTransport *t);

// What went wrong last, or "" when nothing did.
const char *bridge_detail(const BridgeConn *c);

// --- the console's input door ------------------------------------------------
//
// Every byte typed at the console (the prompt, the login prompt, a program
// reading keys, a link request) passes through this first, so that:
//
//   * 0x1E 0x1F, a terminal host offering the bridge, is taken out and reported;
//   * any other line starting with 0x1E is dropped to its end: an answer to a
//     fetch that arrived after the device gave up on it must never be read as
//     keys, and least of all run as a command at the prompt;
//   * 0x03 always gets through.
struct BridgeDoor {
    uint8_t  st;
    uint16_t n;
};

// One byte in. Returns the byte to pass on, or -1 when the door kept it. Sets
// *offer when the offer has just arrived.
int bridge_door(BridgeDoor *d, int c, bool *offer);

#endif  // RPC_BRIDGE_H
