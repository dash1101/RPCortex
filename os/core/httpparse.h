// HTTP/1.1 response parsing and URL splitting, with no sockets anywhere in it.
//
// The transport hands bytes in whatever sizes TCP feels like delivering, so
// everything here is incremental: feed what arrived, get body bytes out through
// a sink, keep the parser between calls. Nothing accumulates a whole response,
// because a .pkg is not guaranteed to be the 5 KB the small ones are and RAM is
// the budget that actually binds.
//
// Keeping it pure is what makes it testable at all. The interesting bugs in an
// HTTP client are the boundary ones — a header split mid-name, a chunk size
// arriving one digit at a time — and those are miserable to provoke through a
// real socket and trivial to provoke here.
#ifndef RPC_HTTPPARSE_H
#define RPC_HTTPPARSE_H

#include <stdint.h>
#include <stdbool.h>

#define HTTP_HOST_MAX  64
#define HTTP_PATH_MAX  192
#define HTTP_LOC_MAX   200
#define HTTP_LINE_MAX  256

struct HttpUrl {
    bool     tls;                    // https rather than http
    char     host[HTTP_HOST_MAX];
    uint16_t port;                   // filled from the scheme when absent
    char     path[HTTP_PATH_MAX];    // always begins with '/'
};

// Split a URL. Returns false on anything not http/https, on an empty host, or
// on a host or path too long to hold — a truncated host would otherwise be a
// silent connection to the wrong place.
bool http_parse_url(const char *url, HttpUrl *out);

// Where body bytes go. Returns 0 to continue, non-zero to abort the transfer —
// which is how "the filesystem filled up" stops a download rather than letting
// it run to completion and fail at the end.
typedef int (*HttpSink)(void *ctx, const uint8_t *data, uint32_t len);

enum HttpState {
    HTTP_ST_STATUS = 0,   // reading the status line
    HTTP_ST_HEADERS,      // reading header lines
    HTTP_ST_BODY_LEN,     // body delimited by Content-Length
    HTTP_ST_BODY_EOF,     // body delimited by the connection closing
    HTTP_ST_CHUNK_SIZE,   // chunked: the size line
    HTTP_ST_CHUNK_DATA,   // chunked: the data
    HTTP_ST_CHUNK_CRLF,   // chunked: the CRLF that follows each chunk
    HTTP_ST_TRAILER,      // chunked: trailing headers after the final chunk
    HTTP_ST_DONE,
    HTTP_ST_ERROR,
};

struct HttpParser {
    uint8_t  state;
    int      status;                 // 200, 404, 301 …
    int64_t  content_length;         // -1 when not given
    int64_t  body_seen;              // bytes handed to the sink
    bool     chunked;
    char     location[HTTP_LOC_MAX]; // Location:, for a redirect
    char     line[HTTP_LINE_MAX];    // the header line being assembled
    uint32_t line_len;
    bool     line_over;              // this line outran the buffer; skip its tail
    int64_t  chunk_left;             // bytes remaining in the current chunk
    HttpSink sink;
    void    *ctx;
    int      sink_rc;                // what the sink said when it refused
};

void http_parser_init(HttpParser *p, HttpSink sink, void *ctx);

// Feed one arrival. Returns the number of bytes consumed, which is everything
// unless the parser finished or failed partway through. Safe to call with any
// split at any offset — that is the property the tests exercise hardest.
uint32_t http_parser_feed(HttpParser *p, const uint8_t *data, uint32_t len);

// Tell the parser the connection closed. A response with neither
// Content-Length nor chunked encoding is only complete at that point, so
// without this such a body would never be reported as finished.
void http_parser_eof(HttpParser *p);

bool http_parser_done(const HttpParser *p);
bool http_parser_failed(const HttpParser *p);

// 301/302/303/307/308 with a Location to follow.
bool http_is_redirect(const HttpParser *p);

// Resolve a Location against the URL it came from, so a server answering with
// an absolute path ("/elsewhere") rather than a full URL still works.
bool http_resolve_redirect(const HttpUrl *from, const char *location, HttpUrl *out);

#endif  // RPC_HTTPPARSE_H
