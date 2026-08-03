// Driving one HTTP request to completion, with the socket held at arm's length.
//
// Everything that decides what happens — building the request, following
// redirects, refusing a body too big for the filesystem, noticing that the
// server said 404 — lives here and knows nothing about lwIP, TLS or sockets. A
// platform supplies four function pointers and this drives them.
//
// The point is not tidiness. It is that the interesting failures (a redirect
// loop, a truncated download, a server that answers with gzip when nothing here
// can decompress it) become testable against a fake transport that produces them
// on demand, instead of requiring a real server that misbehaves on cue. The
// stage-2 socket layer stays thin enough to read in one sitting, which is all
// that is left unproven on the host.
//
// The same seam is what the ESP32-S3 port will implement. It does not have to
// happen now; it does have to not get harder.
#ifndef RPC_HTTPFETCH_H
#define RPC_HTTPFETCH_H

#include <stdint.h>
#include <stdbool.h>
#include "httpparse.h"

// What a platform must provide. Byte streams only — no framing, no parsing.
struct HttpTransport {
    // Open a connection. Returns 0 on success, negative on failure. `tls` is
    // passed through rather than decided here: whether a transport can honour
    // it is the transport's business to report.
    int  (*open)(void *ctx, const char *host, uint16_t port, bool tls);

    // Send everything or fail. Partial sends are the transport's problem to
    // resolve, because a caller cannot retry them without knowing the protocol.
    int  (*send)(void *ctx, const uint8_t *data, uint32_t len);

    // Bytes read, 0 for a clean close, negative for an error. May block; may
    // yield to the scheduler internally, which is how a download stays
    // interruptible without this file knowing what a task is.
    int  (*recv)(void *ctx, uint8_t *buf, uint32_t cap);

    void (*close)(void *ctx);

    void *ctx;
};

enum FetchError {
    FETCH_OK = 0,
    FETCH_ERR_URL,          // could not be parsed, or scheme unsupported
    FETCH_ERR_CONNECT,
    FETCH_ERR_SEND,
    FETCH_ERR_RECV,
    FETCH_ERR_PROTOCOL,     // the response was not valid HTTP, or was truncated
    FETCH_ERR_STATUS,       // a valid response, but not a 2xx
    FETCH_ERR_REDIRECT,     // too many hops, or a redirect with nowhere to go
    FETCH_ERR_TOO_BIG,      // more body than the caller allowed
    FETCH_ERR_ENCODING,     // content-encoding this cannot undo
    FETCH_ERR_SINK,         // the sink refused: out of space, most likely
    FETCH_ERR_ABORTED,      // Ctrl+C, or a caller that changed its mind
};

struct FetchOpts {
    uint32_t max_redirects;     // 0 selects the default of 5
    uint64_t max_bytes;         // 0 means no limit
    const char *user_agent;     // null selects the default

    // Called between reads. Non-zero aborts the transfer. This is how Ctrl+C
    // reaches a download without core/ needing to know what a keyboard is.
    int (*poll)(void *ctx);
    void *poll_ctx;

    // Called as bytes arrive, for a progress line. Total is 0 when the server
    // did not say how big the body would be.
    void (*progress)(void *ctx, uint64_t got, uint64_t total);
    void *progress_ctx;
};

struct FetchResult {
    int      error;             // a FetchError
    int      status;            // the HTTP status actually received
    uint64_t bytes;             // handed to the sink
    uint32_t redirects;
    char     detail[96];        // something a human can act on
};

// Fetch `url`, streaming the body to `sink`. Never accumulates a whole body:
// what the sink does with the bytes is the caller's business, and for a package
// download that means writing them straight to a file.
//
// `opts` may be null for the defaults.
bool http_fetch(const HttpTransport *t, const char *url,
                HttpSink sink, void *sink_ctx,
                const FetchOpts *opts, FetchResult *res);

const char *fetch_error_str(int err);

// Build the request bytes for one URL. Exposed for testing, because the exact
// header block is a thing worth asserting on rather than eyeballing.
int http_build_request(char *buf, uint32_t cap, const HttpUrl *u, const char *user_agent);

#endif  // RPC_HTTPFETCH_H
