// The HTTP response parser, exercised the way TCP actually behaves.
//
// The headline test is not "does it parse a response" — it is "does it parse a
// response the same way no matter where the bytes are cut". Every response here
// is fed at every possible split point, and at one byte at a time, and all of
// them must produce byte-identical bodies and identical parser state. That is
// the class of bug a real socket produces occasionally, on someone else's
// network, and never while anyone is watching.
#include "httpparse.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int checks = 0, fails = 0;

static void ok(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("    FAIL %s\n", what); fails++; }
}

// A sink that accumulates, so the body can be compared.
struct Buf { char data[4096]; uint32_t len; int fail_after; };

static int sink(void *ctx, const uint8_t *d, uint32_t n) {
    Buf *b = (Buf *)ctx;
    if (b->fail_after >= 0 && (int)(b->len + n) > b->fail_after) return -1;
    if (b->len + n >= sizeof(b->data)) return -1;
    memcpy(b->data + b->len, d, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

// Feed `raw` in chunks of `step` bytes (0 means all at once), then signal EOF.
static void run(const char *raw, uint32_t step, HttpParser *p, Buf *b, bool eof) {
    memset(b, 0, sizeof(*b));
    b->fail_after = -1;
    http_parser_init(p, sink, b);
    uint32_t total = (uint32_t)strlen(raw);
    if (step == 0) step = total ? total : 1;
    uint32_t at = 0;
    while (at < total && !http_parser_done(p) && !http_parser_failed(p)) {
        uint32_t n = total - at < step ? total - at : step;
        uint32_t used = http_parser_feed(p, (const uint8_t *)raw + at, n);
        at += used;
        if (used < n) break;      // parser stopped early
    }
    if (eof) http_parser_eof(p);
}

// The property that matters: the split must not change the outcome.
static void every_split(const char *raw, const char *want_body, int want_status,
                        bool eof, const char *label) {
    HttpParser ref; Buf rb;
    run(raw, 0, &ref, &rb, eof);

    char msg[160];
    snprintf(msg, sizeof(msg), "%s: whole - status %d", label, want_status);
    ok(ref.status == want_status, msg);
    snprintf(msg, sizeof(msg), "%s: whole - body", label);
    ok(strcmp(rb.data, want_body) == 0, msg);
    snprintf(msg, sizeof(msg), "%s: whole - done", label);
    ok(http_parser_done(&ref), msg);

    uint32_t len = (uint32_t)strlen(raw);
    for (uint32_t step = 1; step <= len; step++) {
        HttpParser p; Buf b;
        run(raw, step, &p, &b, eof);
        if (b.len != rb.len || memcmp(b.data, rb.data, b.len) != 0 ||
            p.status != ref.status || http_parser_done(&p) != http_parser_done(&ref)) {
            snprintf(msg, sizeof(msg),
                     "%s: split every %u bytes changed the result (body %u vs %u, "
                     "status %d vs %d, done %d vs %d)",
                     label, step, b.len, rb.len, p.status, ref.status,
                     (int)http_parser_done(&p), (int)http_parser_done(&ref));
            ok(false, msg);
            return;                     // one report is enough
        }
    }
    snprintf(msg, sizeof(msg), "%s: identical across all %u splits", label, len);
    ok(true, msg);
}

int main(void) {
    printf("  httpparse\n");

    // --- URLs ---------------------------------------------------------------
    {
        HttpUrl u;
        ok(http_parse_url("http://example.com/a/b", &u) && !u.tls &&
           !strcmp(u.host, "example.com") && u.port == 80 && !strcmp(u.path, "/a/b"),
           "url: plain http");
        ok(http_parse_url("https://raw.githubusercontent.com/dash1101/RPCortex-repo/main/repo/index.json", &u) &&
           u.tls && u.port == 443 && !strcmp(u.host, "raw.githubusercontent.com"),
           "url: the real index URL");
        ok(http_parse_url("http://192.168.1.5:8080/pkg.pkg", &u) &&
           u.port == 8080 && !strcmp(u.host, "192.168.1.5"), "url: explicit port");
        ok(http_parse_url("http://example.com", &u) && !strcmp(u.path, "/"),
           "url: bare host gets /");
        ok(!http_parse_url("ftp://example.com/x", &u),   "url: rejects ftp");
        ok(!http_parse_url("example.com/x", &u),         "url: rejects no scheme");
        ok(!http_parse_url("http:///path", &u),          "url: rejects empty host");
        ok(!http_parse_url("http://u:p@example.com/", &u), "url: rejects credentials");
        ok(!http_parse_url("http://example.com:0/x", &u),  "url: rejects port 0");
        ok(!http_parse_url("http://example.com:99999/x", &u), "url: rejects port > 65535");

        // A host too long must be refused, not truncated — truncation would
        // connect somewhere real and wrong.
        char big[300]; strcpy(big, "http://");
        memset(big + 7, 'a', 200); strcpy(big + 207, "/x");
        ok(!http_parse_url(big, &u), "url: refuses an over-long host");
    }

    // --- redirects ----------------------------------------------------------
    {
        HttpUrl from, to;
        http_parse_url("https://example.com/a/b", &from);
        ok(http_resolve_redirect(&from, "/c/d", &to) && to.tls &&
           !strcmp(to.host, "example.com") && !strcmp(to.path, "/c/d"),
           "redirect: absolute path keeps host and scheme");
        ok(http_resolve_redirect(&from, "http://other.net/z", &to) && !to.tls &&
           !strcmp(to.host, "other.net") && to.port == 80,
           "redirect: full URL replaces everything");
        ok(!http_resolve_redirect(&from, "../up", &to), "redirect: rejects relative");
        ok(!http_resolve_redirect(&from, "", &to),      "redirect: rejects empty");
    }

    // --- framing ------------------------------------------------------------
    every_split("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world",
                "hello world", 200, false, "content-length");

    every_split("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n",
                "hello world", 200, false, "chunked");

    every_split("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n"
                "b\r\nhello world\r\n0\r\n\r\n",
                "hello world", 200, false, "chunked, capitalised");

    every_split("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
                "5\r\nhello\r\n0\r\n\r\n",
                "hello", 200, false, "chunked, listed last");

    every_split("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                "5;ext=1\r\nhello\r\n0\r\n\r\n",
                "hello", 200, false, "chunk extensions ignored");

    every_split("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                "5\r\nhello\r\n0\r\nX-Trailer: v\r\n\r\n",
                "hello", 200, false, "trailing headers after the last chunk");

    // No length, no chunking: the body ends when the connection does.
    every_split("HTTP/1.1 200 OK\r\nServer: x\r\n\r\nhello world",
                "hello world", 200, true, "close-delimited");

    every_split("HTTP/1.1 204 No Content\r\n\r\n", "", 204, false, "204 has no body");

    every_split("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "", 200, false,
                "zero-length body");

    // Bare LF instead of CRLF — out of spec, but servers and fixtures do it.
    every_split("HTTP/1.1 200 OK\nContent-Length: 2\n\nhi", "hi", 200, false,
                "bare LF line endings");

    // --- redirect responses -------------------------------------------------
    {
        HttpParser p; Buf b;
        run("HTTP/1.1 302 Found\r\nLocation: https://elsewhere.test/x\r\nContent-Length: 0\r\n\r\n",
            0, &p, &b, false);
        ok(p.status == 302, "302: status");
        ok(http_is_redirect(&p), "302: recognised as a redirect");
        ok(!strcmp(p.location, "https://elsewhere.test/x"), "302: location captured");

        run("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0, &p, &b, false);
        ok(!http_is_redirect(&p), "200: not a redirect");
    }

    // --- failure modes ------------------------------------------------------
    {
        HttpParser p; Buf b;

        run("NOT-HTTP\r\n\r\n", 0, &p, &b, false);
        ok(http_parser_failed(&p), "rejects a non-HTTP response");

        run("HTTP/1.1 999 Nonsense\r\n\r\n", 0, &p, &b, false);
        ok(http_parser_failed(&p), "rejects an impossible status code");

        // Truncated: fewer body bytes than promised, then the connection drops.
        // This MUST be an error — reporting it complete installs half a package.
        run("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort", 0, &p, &b, true);
        ok(http_parser_failed(&p), "truncated content-length body fails at EOF");
        ok(!http_parser_done(&p),  "truncated body is never reported done");

        // Truncated mid-chunk, likewise.
        run("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n20\r\nshort", 0, &p, &b, true);
        ok(http_parser_failed(&p), "truncated chunk fails at EOF");

        run("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 0, &p, &b, false);
        ok(http_parser_failed(&p), "rejects a non-hex chunk size");

        // A sink that refuses partway — the "filesystem filled up" case. The
        // transfer must stop rather than run to completion.
        memset(&b, 0, sizeof(b));
        b.fail_after = 4;
        http_parser_init(&p, sink, &b);
        const char *raw = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";
        http_parser_feed(&p, (const uint8_t *)raw, (uint32_t)strlen(raw));
        ok(http_parser_failed(&p), "a refusing sink stops the transfer");
        ok(p.sink_rc == -1, "the sink's reason is kept");
    }

    // --- a body big enough to cross real segment boundaries -----------------
    {
        static char raw[3200];
        char body[2048];
        for (size_t i = 0; i < sizeof(body) - 1; i++) body[i] = (char)('a' + i % 26);
        body[sizeof(body) - 1] = 0;
        snprintf(raw, sizeof(raw), "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n%s",
                 (unsigned)strlen(body), body);

        // Every split is O(n^2) at this size, so step through plausible TCP
        // segment sizes rather than all of them.
        HttpParser ref; Buf rb;
        run(raw, 0, &ref, &rb, false);
        ok(rb.len == strlen(body) && !strcmp(rb.data, body), "2 KB body: whole");

        bool all = true;
        const uint32_t steps[] = {1, 2, 3, 64, 128, 536, 1024, 1460};
        for (uint32_t s : steps) {
            HttpParser p; Buf b;
            run(raw, s, &p, &b, false);
            if (b.len != rb.len || memcmp(b.data, rb.data, b.len) || !http_parser_done(&p))
                all = false;
        }
        ok(all, "2 KB body: identical at every realistic segment size");
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
