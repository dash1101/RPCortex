#include "httpparse.h"

#include <string.h>
#include <stdlib.h>

// --- small helpers ----------------------------------------------------------

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool hdr_is(const char *line, const char *name) {
    while (*name) {
        if (lower(*line) != lower(*name)) return false;
        line++; name++;
    }
    // The name must be followed by its colon, otherwise "Content-Length-Hint"
    // would match "Content-Length".
    while (*line == ' ') line++;
    return *line == ':';
}

static const char *hdr_value(const char *line) {
    const char *c = strchr(line, ':');
    if (!c) return "";
    c++;
    while (*c == ' ' || *c == '\t') c++;
    return c;
}

static bool copy_bounded(char *dst, size_t cap, const char *src, size_t n) {
    if (n >= cap) return false;      // refuse rather than truncate
    memcpy(dst, src, n);
    dst[n] = 0;
    return true;
}

// --- URL --------------------------------------------------------------------

bool http_parse_url(const char *url, HttpUrl *out) {
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));

    const char *p = url;
    if (!strncmp(p, "http://", 7)) {
        out->tls = false; out->port = 80; p += 7;
    } else if (!strncmp(p, "https://", 8)) {
        out->tls = true;  out->port = 443; p += 8;
    } else {
        return false;    // no scheme guessing: an unknown scheme is a mistake,
                         // and assuming http for it would be the wrong guess in
                         // the direction that loses encryption
    }

    // Host runs to '/', ':' or the end. Credentials in the authority are not
    // supported — nothing here needs them and parsing them wrong is a way to
    // connect somewhere unintended.
    const char *hs = p;
    while (*p && *p != '/' && *p != ':' && *p != '@' && *p != '?' && *p != '#') p++;
    if (*p == '@') return false;
    if (p == hs) return false;
    if (!copy_bounded(out->host, sizeof(out->host), hs, (size_t)(p - hs))) return false;

    if (*p == ':') {
        p++;
        const char *ds = p;
        long v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (p == ds || v <= 0 || v > 65535) return false;
        out->port = (uint16_t)v;
    }

    if (*p == 0) {
        out->path[0] = '/'; out->path[1] = 0;
        return true;
    }
    if (*p != '/') return false;     // '?' or '#' with no path is malformed here
    return copy_bounded(out->path, sizeof(out->path), p, strlen(p));
}

bool http_resolve_redirect(const HttpUrl *from, const char *location, HttpUrl *out) {
    if (!from || !location || !out || !location[0]) return false;

    // Absolute: parse it whole and let the scheme change.
    if (!strncmp(location, "http://", 7) || !strncmp(location, "https://", 8))
        return http_parse_url(location, out);

    // Relative: same host, same scheme, new path. Only an absolute path is
    // handled — a truly relative one ("../x") would need path normalisation,
    // and no server this talks to emits them.
    if (location[0] != '/') return false;
    *out = *from;
    return copy_bounded(out->path, sizeof(out->path), location, strlen(location));
}

// --- response ---------------------------------------------------------------

void http_parser_init(HttpParser *p, HttpSink sink, void *ctx) {
    memset(p, 0, sizeof(*p));
    p->state = HTTP_ST_STATUS;
    p->content_length = -1;
    p->sink = sink;
    p->ctx = ctx;
}

bool http_parser_done(const HttpParser *p)   { return p->state == HTTP_ST_DONE; }
bool http_parser_failed(const HttpParser *p) { return p->state == HTTP_ST_ERROR; }

bool http_is_redirect(const HttpParser *p) {
    switch (p->status) {
        case 301: case 302: case 303: case 307: case 308:
            return p->location[0] != 0;
        default:
            return false;
    }
}

// Accumulate into the line buffer until CRLF (or a bare LF — some servers, and
// every hand-written test fixture, omit the CR). Returns true when a line is
// ready. A line longer than the buffer is not an error: the tail is dropped and
// the line is reported, because the only over-long headers in practice are ones
// nothing here reads.
static bool feed_line(HttpParser *p, uint8_t c) {
    if (c == '\n') {
        if (p->line_len && p->line[p->line_len - 1] == '\r') p->line_len--;
        p->line[p->line_len] = 0;
        p->line_over = false;
        return true;
    }
    if (p->line_len + 1 < sizeof(p->line)) {
        p->line[p->line_len++] = (char)c;
    } else {
        p->line_over = true;
    }
    return false;
}

static void line_reset(HttpParser *p) { p->line_len = 0; }

// Hand body bytes on. The sink refusing is a stop, not a failure to ignore.
static bool emit(HttpParser *p, const uint8_t *d, uint32_t n) {
    if (n == 0) return true;
    if (p->sink) {
        int rc = p->sink(p->ctx, d, n);
        if (rc != 0) {
            p->sink_rc = rc;
            p->state = HTTP_ST_ERROR;
            return false;
        }
    }
    p->body_seen += n;
    return true;
}

static void parse_status_line(HttpParser *p) {
    // "HTTP/1.1 200 OK"
    if (strncmp(p->line, "HTTP/", 5) != 0) { p->state = HTTP_ST_ERROR; return; }
    const char *sp = strchr(p->line, ' ');
    if (!sp) { p->state = HTTP_ST_ERROR; return; }
    while (*sp == ' ') sp++;
    if (sp[0] < '0' || sp[0] > '9') { p->state = HTTP_ST_ERROR; return; }
    p->status = (int)strtol(sp, nullptr, 10);
    if (p->status < 100 || p->status > 599) { p->state = HTTP_ST_ERROR; return; }
    p->state = HTTP_ST_HEADERS;
}

// Decide how the body is framed, once the headers are in. Order matters:
// chunked wins over Content-Length when a server sends both, which is what the
// RFC requires and is also the safer reading of a contradictory response.
static void headers_complete(HttpParser *p) {
    // A 204/304 carries no body whatever the headers claim, and neither does a
    // redirect this client is about to follow.
    if (p->status == 204 || p->status == 304) { p->state = HTTP_ST_DONE; return; }

    if (p->chunked) {
        p->state = HTTP_ST_CHUNK_SIZE;
        line_reset(p);
        return;
    }
    if (p->content_length == 0) { p->state = HTTP_ST_DONE; return; }
    if (p->content_length > 0)  { p->state = HTTP_ST_BODY_LEN; return; }
    p->state = HTTP_ST_BODY_EOF;
}

static void parse_header_line(HttpParser *p) {
    if (hdr_is(p->line, "content-length")) {
        const char *v = hdr_value(p->line);
        char *end = nullptr;
        long long n = strtoll(v, &end, 10);
        if (end != v && n >= 0) p->content_length = n;
    } else if (hdr_is(p->line, "transfer-encoding")) {
        // Only "chunked" matters here. Lowercase a bounded copy and look for it:
        // the header is case-insensitive, and "gzip, chunked" is legal, so a
        // substring test on a folded copy is both correct and short.
        const char *v = hdr_value(p->line);
        char buf[48]; size_t n = 0;
        for (const char *s = v; *s && n + 1 < sizeof(buf); s++) buf[n++] = lower(*s);
        buf[n] = 0;
        if (strstr(buf, "chunked")) p->chunked = true;
    } else if (hdr_is(p->line, "location")) {
        const char *v = hdr_value(p->line);
        copy_bounded(p->location, sizeof(p->location), v, strlen(v));
    }
}

uint32_t http_parser_feed(HttpParser *p, const uint8_t *data, uint32_t len) {
    uint32_t i = 0;

    while (i < len && p->state != HTTP_ST_DONE && p->state != HTTP_ST_ERROR) {
        switch (p->state) {

        case HTTP_ST_STATUS:
            if (feed_line(p, data[i++])) { parse_status_line(p); line_reset(p); }
            break;

        case HTTP_ST_HEADERS:
            if (feed_line(p, data[i++])) {
                if (p->line_len == 0) { headers_complete(p); }
                else                  { parse_header_line(p); }
                line_reset(p);
            }
            break;

        case HTTP_ST_BODY_LEN: {
            int64_t want = p->content_length - p->body_seen;
            uint32_t avail = len - i;
            uint32_t n = (want < (int64_t)avail) ? (uint32_t)want : avail;
            if (!emit(p, data + i, n)) break;
            i += n;
            if (p->body_seen >= p->content_length) p->state = HTTP_ST_DONE;
            break;
        }

        case HTTP_ST_BODY_EOF: {
            uint32_t n = len - i;
            if (!emit(p, data + i, n)) break;
            i += n;
            break;      // completion arrives via http_parser_eof
        }

        case HTTP_ST_CHUNK_SIZE:
            if (feed_line(p, data[i++])) {
                // "1a2b" or "1a2b;ext=value" — hex, optional extensions.
                char *end = nullptr;
                long long n = strtoll(p->line, &end, 16);
                if (end == p->line || n < 0) { p->state = HTTP_ST_ERROR; }
                else if (n == 0)             { p->state = HTTP_ST_TRAILER; }
                else { p->chunk_left = n; p->state = HTTP_ST_CHUNK_DATA; }
                line_reset(p);
            }
            break;

        case HTTP_ST_CHUNK_DATA: {
            uint32_t avail = len - i;
            uint32_t n = (p->chunk_left < (int64_t)avail) ? (uint32_t)p->chunk_left : avail;
            if (!emit(p, data + i, n)) break;
            i += n;
            p->chunk_left -= n;
            if (p->chunk_left == 0) { p->state = HTTP_ST_CHUNK_CRLF; line_reset(p); }
            break;
        }

        case HTTP_ST_CHUNK_CRLF:
            if (feed_line(p, data[i++])) {
                if (p->line_len != 0) p->state = HTTP_ST_ERROR;   // garbage after a chunk
                else                  p->state = HTTP_ST_CHUNK_SIZE;
                line_reset(p);
            }
            break;

        case HTTP_ST_TRAILER:
            // Trailing headers after the final chunk, ended by a blank line.
            if (feed_line(p, data[i++])) {
                if (p->line_len == 0) p->state = HTTP_ST_DONE;
                line_reset(p);
            }
            break;

        default:
            return i;
        }
    }
    return i;
}

void http_parser_eof(HttpParser *p) {
    if (p->state == HTTP_ST_BODY_EOF) {
        p->state = HTTP_ST_DONE;        // this framing ends exactly here
    } else if (p->state != HTTP_ST_DONE && p->state != HTTP_ST_ERROR) {
        // Anything else was still expecting bytes. Truncated is not complete,
        // and reporting it as complete would install half a package.
        p->state = HTTP_ST_ERROR;
    }
}
