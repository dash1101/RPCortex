#include "httpfetch.h"

#include <string.h>
#include <stdio.h>

#define DEFAULT_REDIRECTS  5
#define DEFAULT_UA         "RPCortex/2.0"
#define RECV_CHUNK         512      // one TCP segment's worth; see below

const char *fetch_error_str(int err) {
    switch (err) {
        case FETCH_OK:            return "ok";
        case FETCH_ERR_URL:       return "bad URL";
        case FETCH_ERR_CONNECT:   return "could not connect";
        case FETCH_ERR_SEND:      return "send failed";
        case FETCH_ERR_RECV:      return "connection lost";
        case FETCH_ERR_PROTOCOL:  return "bad response";
        case FETCH_ERR_STATUS:    return "server refused";
        case FETCH_ERR_REDIRECT:  return "too many redirects";
        case FETCH_ERR_TOO_BIG:   return "too large";
        case FETCH_ERR_ENCODING:  return "unsupported encoding";
        case FETCH_ERR_SINK:      return "could not write";
        case FETCH_ERR_ABORTED:   return "cancelled";
        default:                  return "failed";
    }
}

int http_build_request(char *buf, uint32_t cap, const HttpUrl *u, const char *ua) {
    if (!ua) ua = DEFAULT_UA;

    // Connection: close is deliberate. Keep-alive would save a handshake on the
    // second request, and would cost a connection state machine, an idle timer
    // and a class of bug where a stale socket returns the previous response.
    // One fetch, one connection.
    //
    // Accept-Encoding: identity is not optional. There is no decompressor here,
    // and a server that helpfully gzips a .pkg would hand back an archive that
    // fails to open with no clue as to why.
    int n = snprintf(buf, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept-Encoding: identity\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     u->path, u->host, ua);
    if (n < 0 || (uint32_t)n >= cap) return -1;
    return n;
}

// What the sink sees, wrapped so the size cap and the progress callback apply
// without every caller having to implement them.
struct SinkGuard {
    HttpSink  inner;
    void     *inner_ctx;
    uint64_t  limit;         // 0 for none
    uint64_t  seen;
    uint64_t  total;         // from Content-Length, 0 when unknown
    void    (*progress)(void *, uint64_t, uint64_t);
    void     *progress_ctx;
    bool      over_limit;
    bool      sink_refused;
    const HttpParser *parser;   // to see the status before the body is passed on
};

static int guarded_sink(void *ctx, const uint8_t *data, uint32_t len) {
    SinkGuard *g = (SinkGuard *)ctx;

    // A redirect's body is not the caller's content, and it must not reach the
    // sink at all — resetting a byte counter afterwards does not unwrite it.
    // Servers do attach a courtesy page to a 302, and prepending that to a .pkg
    // produces an archive that fails to open with nothing to explain why.
    //
    // The status is always known by now: the status line is parsed before the
    // headers, and the headers before any body byte is emitted.
    if (g->parser && g->parser->status >= 300 && g->parser->status < 400)
        return 0;               // accepted and discarded, so the hop completes

    // Refuse BEFORE writing. Catching it afterwards would mean a file already
    // on the filesystem that has to be cleaned up, at the moment the
    // filesystem is the thing under pressure.
    if (g->limit && g->seen + len > g->limit) { g->over_limit = true; return -1; }

    if (g->inner) {
        int rc = g->inner(g->inner_ctx, data, len);
        if (rc != 0) { g->sink_refused = true; return rc; }
    }
    g->seen += len;
    if (g->progress) g->progress(g->progress_ctx, g->seen, g->total);
    return 0;
}

// One request/response exchange. Redirects are handled by the caller looping.
static int one_hop(const HttpTransport *t, const HttpUrl *u, const FetchOpts *o,
                   SinkGuard *guard, HttpParser *p, FetchResult *res) {
    if (t->open(t->ctx, u->host, u->port, u->tls) != 0) {
        snprintf(res->detail, sizeof(res->detail), "%s:%u", u->host, u->port);
        return FETCH_ERR_CONNECT;
    }

    char req[HTTP_PATH_MAX + HTTP_HOST_MAX + 160];
    int n = http_build_request(req, sizeof(req), u, o->user_agent);
    if (n < 0) { t->close(t->ctx); return FETCH_ERR_URL; }

    if (t->send(t->ctx, (const uint8_t *)req, (uint32_t)n) != 0) {
        t->close(t->ctx);
        return FETCH_ERR_SEND;
    }

    http_parser_init(p, guarded_sink, guard);
    guard->parser = p;

    // A stack buffer sized to about one segment. Bigger would not help: the
    // parser consumes what it is given and streams the body straight out, so
    // this only bounds how much arrives per read, and stack is the scarcer
    // resource on a task with a few KB to its name.
    uint8_t buf[RECV_CHUNK];
    int err = FETCH_OK;

    while (!http_parser_done(p) && !http_parser_failed(p)) {
        if (o->poll && o->poll(o->poll_ctx) != 0) { err = FETCH_ERR_ABORTED; break; }

        int got = t->recv(t->ctx, buf, sizeof(buf));
        if (got < 0) { err = FETCH_ERR_RECV; break; }
        if (got == 0) {                       // clean close
            http_parser_eof(p);
            break;
        }

        // Content-Length is known once the headers are in; hand it to the
        // progress callback so a download can show a total rather than a
        // running count with no end in sight.
        if (guard->total == 0 && p->content_length > 0)
            guard->total = (uint64_t)p->content_length;

        uint32_t used = http_parser_feed(p, buf, (uint32_t)got);
        if (used < (uint32_t)got && !http_parser_done(p)) {
            // Stopped early. WHY is decided after the loop: a sink that refused
            // because the filesystem is full stops the parser exactly the same
            // way malformed input does, and "could not write" is a far more
            // useful thing to be told than "bad response".
            break;
        }
    }

    t->close(t->ctx);

    if (err != FETCH_OK) return err;

    // The guard's reasons outrank a bare parser failure: "the disk filled up"
    // is a more useful thing to be told than "the response ended oddly".
    if (guard->over_limit)   return FETCH_ERR_TOO_BIG;
    if (guard->sink_refused) return FETCH_ERR_SINK;
    if (http_parser_failed(p)) {
        snprintf(res->detail, sizeof(res->detail), "truncated after %llu bytes",
                 (unsigned long long)guard->seen);
        return FETCH_ERR_PROTOCOL;
    }
    return FETCH_OK;
}

bool http_fetch(const HttpTransport *t, const char *url,
                HttpSink sink, void *sink_ctx,
                const FetchOpts *opts, FetchResult *res) {
    FetchOpts defaults{};
    if (!opts) opts = &defaults;

    memset(res, 0, sizeof(*res));

    uint32_t max_redirects = opts->max_redirects ? opts->max_redirects : DEFAULT_REDIRECTS;

    HttpUrl u;
    if (!http_parse_url(url, &u)) {
        res->error = FETCH_ERR_URL;
        snprintf(res->detail, sizeof(res->detail), "%.80s", url);
        return false;
    }

    SinkGuard guard{};
    guard.inner = sink;
    guard.inner_ctx = sink_ctx;
    guard.limit = opts->max_bytes;
    guard.progress = opts->progress;
    guard.progress_ctx = opts->progress_ctx;

    for (uint32_t hop = 0; ; hop++) {
        if (hop > max_redirects) {
            res->error = FETCH_ERR_REDIRECT;
            res->redirects = hop - 1;
            return false;
        }

        HttpParser p;
        int err = one_hop(t, &u, opts, &guard, &p, res);
        res->status = p.status;
        res->bytes = guard.seen;
        res->redirects = hop;

        if (err != FETCH_OK) { res->error = err; return false; }

        if (http_is_redirect(&p)) {
            HttpUrl next;
            if (!http_resolve_redirect(&u, p.location, &next)) {
                res->error = FETCH_ERR_REDIRECT;
                snprintf(res->detail, sizeof(res->detail), "%.80s", p.location);
                return false;
            }
            // A redirect body is not the caller's content. Discard whatever it
            // carried so a 302 with a courtesy HTML page does not end up
            // prepended to the file being downloaded.
            guard.seen = 0;
            guard.total = 0;
            u = next;
            continue;
        }

        if (p.status < 200 || p.status > 299) {
            res->error = FETCH_ERR_STATUS;
            snprintf(res->detail, sizeof(res->detail), "HTTP %d", p.status);
            return false;
        }

        res->error = FETCH_OK;
        res->bytes = guard.seen;
        return true;
    }
}
