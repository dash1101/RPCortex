// The HTTP client's commands and package calls: wget, curl, runurl and the
// fw_http_* calls, in every image.
//
// They used to live in httptransport.cpp and went with Wi-Fi. They do not need
// it: they need a way for a request to leave the device, and an image without
// Wi-Fi has one when a host on the console offers its own (the bridge, LINK.md
// protocol 3). So they are here, and they ask httproute.h which way a request
// goes; httptransport.cpp keeps only lwIP and TLS.
//
// CONFIRMED on a Pico 2 W over Wi-Fi: `wget` and `curl` over HTTPS, and the
// websearch and speedtest packages. Over the bridge: DEVICE-UNCONFIRMED; the host
// suites (bridge_test, RPCortex Studio, tools/web) drive it against a fake host.

#include "command.h"
#include "session.h"
#include "out.h"
#include "httpfetch.h"
#include "httproute.h"
#include "interrupt.h"
#include "task.h"
#include "storage.h"
#include "path.h"
#include "fsguard.h"
#include "rpc_app.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"

const char *fs_cwd(void);
void http_net_set_timeout_ms(uint32_t ms);      // curl --timeout; 0 restores the default
static int cmd_wget(int argc, char **argv);

// --- the route -------------------------------------------------------------------

// The transport the last request went out on, so a failure message describes
// that one. Only ever a hint for a message: two tasks racing on it would at
// worst word one failure the other way.
static volatile uint8_t g_last_via;

HttpVia http_route(HttpTransport *t) {
    HttpVia via = HTTP_VIA_NONE;
    if (http_net_transport_get(t))      via = HTTP_VIA_NET;
    else if (bridge_transport_get(t))   via = HTTP_VIA_BRIDGE;
    if (via != HTTP_VIA_NONE) g_last_via = (uint8_t)via;
    return via;
}

void http_no_route(void) {
    if (bridge_available()) bridge_no_route();
    else                    http_net_no_route();
}

bool http_https_ready(const char *url, HttpVia via) {
    // Over the bridge the host makes the connection and checks the certificate
    // against its own trust store; there is nothing to load here.
    if (via != HTTP_VIA_NET || strncmp(url, "https://", 8) != 0) return true;
    if (http_tls_available()) return true;
    // Refused rather than made unverified: anything on the path could serve a
    // package and it would install.
    out_err("HTTPS cannot be verified: %s", http_tls_why());
    out_multi("  Try 'pkg certs install'; the bundle ships in the firmware. Unverified");
    out_multi("  HTTPS is not offered: anything on the path could serve a package.");
    return false;
}

void http_last_detail(char *out, unsigned cap) {
    if (g_last_via == HTTP_VIA_BRIDGE) snprintf(out, cap, "%s", bridge_last_detail());
    else                               http_net_last_detail(out, cap);
}

void http_report_failure(const FetchResult *r) {
    bool transport = r->error == FETCH_ERR_RECV || r->error == FETCH_ERR_CONNECT ||
                     r->error == FETCH_ERR_SEND;
    if (transport && g_last_via == HTTP_VIA_BRIDGE) {
        out_err("%s", bridge_last_detail());
        return;
    }
    out_err("%s%s%s", fetch_error_str(r->error), r->detail[0] ? " - " : "", r->detail);
    if (transport) {
        char d[192];
        http_last_detail(d, sizeof(d));
        out_multi("  %s", d);
    }
}

// --- wget -------------------------------------------------------------------

struct FileSink { void *h; };

static int file_sink(void *ctx, const uint8_t *data, uint32_t len) {
    FileSink *f = (FileSink *)ctx;
    // Refusing here is what stops a download that has filled the disk, and the
    // driver turns it into "could not write" rather than "bad response".
    return storage_sink_write(f->h, data, len) ? 0 : -1;
}

static uint64_t g_progress_last;    // reset per download, not per boot

static void show_progress(void *, uint64_t got, uint64_t total) {
    // Every 4 KB: often enough to look continuous, rare enough that drawing it
    // does not slow the transfer it is reporting on.
    static uint64_t last;
    if (got < last) last = 0;
    if (got - last < 4096 && got != total) return;
    last = got;
    out_progress("Downloading", got, total);
}

static int poll_interrupt(void *) { return intr_check() ? 1 : 0; }

// Into a caller's buffer. Bounded by that buffer rather than by a limit of its
// own: a package asking for a page into 4 KB gets at most 4 KB, and the fetch
// stops rather than the buffer overflowing.
struct MemSink { unsigned char *buf; unsigned cap; unsigned len; };

static int mem_sink(void *ctx, const uint8_t *data, uint32_t len) {
    MemSink *m = (MemSink *)ctx;
    uint32_t room = m->cap > m->len ? m->cap - m->len : 0;
    uint32_t take = len < room ? len : room;
    if (take) { memcpy(m->buf + m->len, data, take); m->len += take; }
    return (take < len) ? -1 : 0;      // full: stop, and say the transfer ended
}

int net_pkg_http_get(const char *url, void *buf, unsigned cap) {
    if (!url || !buf || cap == 0) return -1;
    HttpTransport t;
    if (!http_route(&t)) return -1;

    MemSink m{ (unsigned char *)buf, cap, 0 };
    FetchOpts o{};
    o.poll = poll_interrupt;
    // No max_bytes: mem_sink stops the transfer itself when the buffer is
    // full, and that is the answer the caller asked for. Passing the buffer's
    // size as the limit made the fetch guard refuse the chunk that would have
    // filled it (and, since the early refusal, any body announced as larger),
    // so a reply bigger than the buffer came back as -1 instead of its start.

    FetchResult r;
    bool good = http_fetch(&t, url, mem_sink, &m, &o, &r);
    // A full buffer stops the transfer, which is not a failure — the caller
    // asked for that much and got it.
    if (!good && m.len < cap) return -1;
    return (int)m.len;
}

// --- fw_http_request: one request, as a browser makes them --------------------------
//
// The body to a file, capped by the CALLER's sink rather than by max_bytes: the
// fetch guard refuses a body announced as bigger than its limit, and a browser
// wants the start of a big page, not a refusal. max_bytes stays the filesystem's
// room, so a page that would fill the disk is still stopped.

struct ReqSink { void *h; uint32_t cap, len; bool full, failed; };

static int req_sink(void *ctx, const uint8_t *d, uint32_t n) {
    ReqSink *s = (ReqSink *)ctx;
    uint32_t room = s->cap ? (s->cap > s->len ? s->cap - s->len : 0) : n;
    uint32_t take = n < room ? n : room;
    if (take && !storage_sink_write(s->h, d, take)) { s->failed = true; return -1; }
    s->len += take;
    if (take < n) { s->full = true; return -1; }
    return 0;
}

struct ReqHead { FwHttpRequest *r; };

static bool header_is(const char *line, const char *name, const char **value) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++) {
        char a = line[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    if (line[n] != ':') return false;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *value = v;
    return true;
}

static void req_header(void *ctx, const char *line) {
    FwHttpRequest *r = ((ReqHead *)ctx)->r;
    const char *v;
    if (header_is(line, "content-type", &v))
        snprintf(r->content_type_out, sizeof(r->content_type_out), "%s", v);
    else if (header_is(line, "location", &v))
        snprintf(r->location, sizeof(r->location), "%s", v);
    else if (header_is(line, "x-rpc-final-url", &v))
        // The bridge's host followed the redirects; this is where it ended up.
        snprintf(r->final_url, sizeof(r->final_url), "%s", v);
}

int net_pkg_http_request(FwHttpRequest *r) {
    r->status = 0; r->bytes = 0; r->truncated = 0;
    r->final_url[0] = r->location[0] = r->content_type_out[0] = r->detail[0] = 0;

    HttpTransport t;
    HttpVia via = http_route(&t);
    if (via == HTTP_VIA_NONE) {
        snprintf(r->detail, sizeof(r->detail), "%s",
                 bridge_available()
                     ? "No network: connect this board to RPC Studio (rpc term, or rpc bridge)."
                     : "No network: 'wifi connect <ssid>' first.");
        return -1;
    }
    char headers[HTTP_EXTRA_MAX] = "";
    if (r->body && r->body_len) {
        char ct[96];
        snprintf(ct, sizeof(ct), "Content-Type: %s",
                 r->content_type ? r->content_type : "application/x-www-form-urlencoded");
        if (!http_add_header(headers, sizeof(headers), ct)) {
            snprintf(r->detail, sizeof(r->detail), "That content type cannot be sent.");
            return -1;
        }
    }

    ReqSink s{nullptr, r->max_bytes, 0, false, false};
    s.h = storage_open_sink(r->path);
    if (!s.h) { snprintf(r->detail, sizeof(r->detail), "Could not write %s.", r->path); return -1; }

    ReqHead hc{r};
    char final_url[256] = "";
    FetchOpts o{};
    o.poll = poll_interrupt;
    uint32_t room = storage_free_bytes();
    uint32_t disk = room > 8192 ? room - 8192 : 1;
    // The caller's cap stops the transfer itself (req_sink), so the fetch is
    // given no limit when that cap fits on the disk: the guard would refuse a
    // page ANNOUNCED as larger than its limit before its first byte, and the
    // start of a big page is exactly what was asked for.
    o.max_bytes = (r->max_bytes && r->max_bytes < disk) ? 0 : disk;
    o.method = r->method;
    o.headers = headers[0] ? headers : nullptr;
    o.body = (const uint8_t *)r->body;
    o.body_len = r->body ? r->body_len : 0;
    o.user_agent = r->user_agent;
    o.on_header = req_header;
    o.on_header_ctx = &hc;
    o.final_url = final_url;
    o.final_url_cap = sizeof(final_url);

    FetchResult res;
    bool good = http_fetch(&t, r->url, req_sink, &s, &o, &res);
    bool closed = storage_close_sink(s.h);
    r->status = res.status;
    r->bytes = s.len;
    r->truncated = s.full ? 1 : 0;
    // The bridge's own header wins: through it, the device only ever saw the URL
    // it asked for.
    if (!r->final_url[0]) snprintf(r->final_url, sizeof(r->final_url), "%s", final_url[0] ? final_url : r->url);

    // An answer, whatever its status, is a page; so is the start of one that was
    // cut at the cap. No answer at all is the failure.
    bool answered = good || s.full || (res.error == FETCH_ERR_STATUS && res.status > 0);
    if (!answered || s.failed || !closed) {
        storage_remove(r->path);
        if (s.failed || !closed) snprintf(r->detail, sizeof(r->detail), "The filesystem refused the page (full?).");
        else if (res.detail[0]) snprintf(r->detail, sizeof(r->detail), "%s: %s", fetch_error_str(res.error), res.detail);
        else snprintf(r->detail, sizeof(r->detail), "%s", fetch_error_str(res.error));
        return -1;
    }
    return (int)s.len;
}

// Fetch and THROW AWAY, counting and timing.
//
// A throughput test wants a megabyte off the wire and has nowhere to put it:
// the buffer form is bounded by the buffer, and the file form would need the
// download to fit in flash — 596 KB free against a 1 MB test — and would be
// measuring the filesystem as much as the link. v1 solved it by streaming and
// discarding chunk by chunk, and this is the same idea with the loop already
// written.
//
// The clock starts at the first byte, not at the call: DNS and the TLS
// handshake are latency, and counting them as transfer time makes a fast link
// look slow on a small file.
struct CountSink { uint64_t bytes; uint32_t first_us; };

static int count_sink(void *ctx, const uint8_t *, uint32_t len) {
    CountSink *c = (CountSink *)ctx;
    if (!c->bytes) c->first_us = (uint32_t)(time_us_64() & 0xFFFFFFFFu);
    c->bytes += len;
    return 0;
}

int net_pkg_http_measure(const char *url, uint32_t *bytes, uint32_t *ms) {
    if (!url) return -1;
    HttpTransport t;
    if (!http_route(&t)) return -1;

    CountSink c{0, 0};
    FetchOpts o{};
    o.poll = poll_interrupt;

    FetchResult r;
    bool good = http_fetch(&t, url, count_sink, &c, &o, &r);
    if (!good && c.bytes == 0) return -1;

    uint32_t end_us = (uint32_t)(time_us_64() & 0xFFFFFFFFu);
    uint32_t took   = c.bytes ? (end_us - c.first_us) : 0;
    if (bytes) *bytes = (uint32_t)(c.bytes > 0xFFFFFFFFull ? 0xFFFFFFFFull : c.bytes);
    // Never zero when anything arrived, so a caller dividing by it cannot fault
    // on a transfer that finished inside one microsecond tick.
    if (ms)    *ms    = took / 1000u ? took / 1000u : 1u;
    return (int)(c.bytes > 0x7FFFFFFFull ? 0x7FFFFFFF : (uint32_t)c.bytes);
}

int net_pkg_http_download(const char *url, const char *path) {
    if (!url || !path) return -1;
    HttpTransport t;
    if (!http_route(&t)) return -1;

    FileSink fs{nullptr};
    fs.h = storage_open_sink(path);
    if (!fs.h) return -1;

    FetchOpts o{};
    o.poll = poll_interrupt;
    uint32_t room = storage_free_bytes();
    o.max_bytes = room > 8192 ? room - 8192 : 1;

    FetchResult r;
    bool good = http_fetch(&t, url, file_sink, &fs, &o, &r);
    if (!storage_close_sink(fs.h)) good = false;
    if (!good) { storage_remove(path); return -1; }
    return (int)r.bytes;
}

static int cmd_runurl(int argc, char **argv) {
    if (argc < 2) {
        out_multi("Usage: runurl <url> [--keep] [-y]");
        out_multi("  Downloads a .rps script and runs it.");
        out_multi("  --keep  leave the file behind afterwards");
        out_multi("  -y      do not ask first");
        return 1;
    }

    const char *url = argv[1];
    bool keep = false, yes = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--keep") == 0)                                  keep = true;
        else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0) yes = true;
    }

    if (!yes) {
        out_warn("This downloads a script and runs it on this device.");
        out_multi("  %s", url);
        if (!session_confirm("Run it")) { out_info("Cancelled."); return 1; }
    }

    const char *tmp = "/tmp/runurl.rps";
    char *wargv[3] = { (char *)"wget", (char *)url, (char *)tmp };
    if (cmd_wget(3, wargv) != 0) return 1;

    const Command *sc = cmd_resolve("script");
    if (!sc || !sc->fn) {
        out_err("The script interpreter is not available.");
        storage_remove(tmp);
        return 1;
    }
    char *sargv[3] = { (char *)"script", (char *)tmp, nullptr };
    int rc = sc->fn(2, sargv);

    if (!keep) storage_remove(tmp);
    else       out_info("Kept as %s", tmp);
    return rc;
}


// runurl <url> [--keep] [-y]
//
// Fetching and executing a remote script is arbitrary code execution, so the
// confirmation is the feature rather than friction — -y is there for scripts
// that have already decided, not as the shortcut to reach for by default.
//
// The download reuses wget wholesale rather than duplicating the fetch, sink
// and progress handling, and the run hands off to `script` the same way `run`
// does for a .rps. Two copies of either would drift.

static int cmd_wget(int argc, char **argv) {
    if (argc < 2) {
        out_multi("Usage: wget <url> [file]");
        out_multi("  Downloads to the current directory unless a name is given.");
        return 1;
    }

    HttpTransport t;
    HttpVia via = http_route(&t);
    if (!via) { http_no_route(); return 1; }

    // Say WHY an https URL cannot be fetched. Without the trusted roots the
    // connection is refused rather than made unverified, and "could not
    // connect" would send someone looking at their WiFi for a problem that is
    // a missing file.
    if (!http_https_ready(argv[1], via)) return 1;

    // Name it after the last path segment when the caller did not say.
    char name[64];
    if (argc >= 3) {
        snprintf(name, sizeof(name), "%s", argv[2]);
    } else {
        const char *slash = strrchr(argv[1], '/');
        const char *base = (slash && slash[1]) ? slash + 1 : "download";
        snprintf(name, sizeof(name), "%s", base);
    }

    char full[128];      // the size every other command here uses
    path_resolve(fs_cwd(), name, full, sizeof(full));
    if (!fs_allow_change(full)) return 1;

    FileSink fs{nullptr};
    fs.h = storage_open_sink(full);
    if (!fs.h) { out_err("Could not create '%s'.", full); return 1; }

    out_info("Fetching %s", argv[1]);
    g_progress_last = 0;    // or a second wget shows nothing until it passes the first

    FetchOpts o{};
    o.poll = poll_interrupt;
    o.progress = show_progress;
    // Never more than the filesystem could hold. Better to refuse at the first
    // byte over than to fill the disk and take the OS down with it.
    uint32_t room = storage_free_bytes();
    o.max_bytes = room > 8192 ? room - 8192 : 1;

    FetchResult r;
    bool good = http_fetch(&t, argv[1], file_sink, &fs, &o, &r);
    // The close is where littlefs commits, so a failure there is a failure of
    // the download however well the transfer itself went.
    if (!storage_close_sink(fs.h) && good) {
        good = false;
        r.error = FETCH_ERR_SINK;
    }
    out_progress_done();

    if (!good) {
        storage_remove(full);        // a partial file is worse than none
        // Out of room is the likeliest reason a download stops, and the plain
        // "too large" / "could not write" left people guessing at the network.
        if (r.error == FETCH_ERR_TOO_BIG) {
            out_err("Not enough room for it: %s.", r.detail[0] ? r.detail : "too large");
            out_multi("  Nothing was written. 'df' shows the space, 'du /' what is using it.");
            return 1;
        }
        if (r.error == FETCH_ERR_SINK) {
            out_err("Could not write %s after %llu KB: the filesystem is full or failing (%lu KB free).",
                    full, (unsigned long long)(r.bytes / 1024),
                    (unsigned long)(storage_free_bytes() / 1024));
            out_multi("  The partial file was removed. 'df' shows the space, 'du /' what is using it.");
            return 1;
        }
        // With the reason: over the bridge every failure is a transport
        // failure, and "connection lost" alone says nothing about which.
        http_report_failure(&r);
        return 1;
    }

    out_ok("Saved %s (%llu bytes)", full, (unsigned long long)r.bytes);
    return 0;
}

// --- curl -------------------------------------------------------------------
//
// wget saves; curl prints. That is the whole difference, and it matters because
// printing means the output goes down a PIPE — `curl <url> | grep something`
// works, which is most of what anyone wants a curl for on a device like this.

struct PrintSink { uint64_t n; };

static int print_sink(void *ctx, const uint8_t *data, uint32_t len) {
    PrintSink *p = (PrintSink *)ctx;
    // Straight to the data channel, so a pipe or a redirect sees it and the
    // status tags do not end up in the middle of a JSON document.
    out_write((const char *)data, len);
    p->n += len;
    return 0;
}

// --- curl's request flags ------------------------------------------------------

struct HeaderPrint { bool on; };
static void print_header_line(void *ctx, const char *line) {
    if (!((HeaderPrint *)ctx)->on) return;
    // The data channel, like the body: `curl -I url | grep etag` has to work.
    out_write(line, (uint32_t)strlen(line));
    out_write("\n", 1);
}

static int cmd_curl(int argc, char **argv) {
    const char *url = nullptr;
    const char *save = nullptr;
    const char *data = nullptr;
    char method[12] = "";
    bool quiet = false, verbose = false, head = false;
    uint32_t timeout_s = 0;
    char hdrs[HTTP_EXTRA_MAX + 1];       // on the stack: 321 bytes, not 321 bytes of RAM forever
    hdrs[0] = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) { save = argv[++i]; continue; }
        if (!strcmp(a, "-s") || !strcmp(a, "--silent")) { quiet = true; continue; }
        if (!strcmp(a, "-v")) { verbose = true; continue; }
        if (!strcmp(a, "-I") || !strcmp(a, "--head")) { head = true; continue; }
        if (!strcmp(a, "-d") && i + 1 < argc) { data = argv[++i]; continue; }
        if (!strcmp(a, "-X") && i + 1 < argc) {
            const char *m = argv[++i];
            size_t n = strlen(m);
            if (n == 0 || n >= sizeof(method)) { out_err("Bad method: %s", m); return 1; }
            for (size_t k = 0; k < n; k++) {
                char c = m[k];
                if (c >= 'a' && c <= 'z') c = (char)(c - 32);
                if (c < 'A' || c > 'Z') { out_err("Bad method: %s", m); return 1; }
                method[k] = c;
            }
            method[n] = 0;
            continue;
        }
        if (!strcmp(a, "-H") && i + 1 < argc) {
            if (!http_add_header(hdrs, sizeof(hdrs), argv[++i])) {
                out_err("Bad or oversized header: %s", argv[i]);
                out_multi("  Use -H 'Name: value'. Headers may total %d bytes.", HTTP_EXTRA_MAX);
                return 1;
            }
            continue;
        }
        if (!strcmp(a, "--timeout") && i + 1 < argc) {
            long v = strtol(argv[++i], nullptr, 10);
            if (v < 1 || v > 120) { out_err("--timeout takes 1 to 120 seconds."); return 1; }
            timeout_s = (uint32_t)v;
            continue;
        }
        if (a[0] == '-' && a[1]) { out_err("Unknown curl flag: %s", a); return 1; }
        url = a;
    }
    if (!url) {
        out_multi("Usage: curl <url> [-v] [-s] [-I] [-X method] [-d data] [-H 'Name: value'] [-o file] [--timeout n]");
        out_multi("  Prints the body, so it can be piped:  curl <url> | grep name");
        out_multi("  -o writes to a file instead;  -s hides the summary;  -I shows only the headers.");
        out_multi("  -d sends a body (and makes it a POST unless -X says otherwise); -H adds a header.");
        return 1;
    }

    // -I is a HEAD whatever -X says, as in v1; a body implies POST when no method
    // was given.
    if (head) snprintf(method, sizeof(method), "HEAD");
    else if (!method[0] && data) snprintf(method, sizeof(method), "POST");

    HttpTransport t;
    HttpVia via = http_route(&t);
    if (!via) { http_no_route(); return 1; }
    if (!http_https_ready(url, via)) return 1;

    HeaderPrint hp{head};
    FetchOpts o{};
    o.poll = poll_interrupt;
    o.method = method[0] ? method : nullptr;
    o.headers = hdrs[0] ? hdrs : nullptr;
    o.body = (const uint8_t *)data;
    o.body_len = data ? (uint32_t)strlen(data) : 0;
    o.on_header = print_header_line;
    o.on_header_ctx = &hp;
    uint32_t room = storage_free_bytes();
    o.max_bytes = save ? (room > 8192 ? room - 8192 : 1) : 0;

    if (timeout_s) http_net_set_timeout_ms(timeout_s * 1000u);
    if (verbose && !quiet) out_info("%s %s", method[0] ? method : "GET", url);

    FetchResult r;
    bool good;
    if (save) {
        char full[128];
        path_resolve(fs_cwd(), save, full, sizeof(full));
        if (!fs_allow_change(full)) { http_net_set_timeout_ms(0); return 1; }
        FileSink fs{nullptr};
        fs.h = storage_open_sink(full);
        if (!fs.h) { http_net_set_timeout_ms(0); out_err("Could not create '%s'.", full); return 1; }
        good = http_fetch(&t, url, file_sink, &fs, &o, &r);
        if (!storage_close_sink(fs.h) && good) { good = false; r.error = FETCH_ERR_SINK; }
        if (!good) storage_remove(full);
        else if (!quiet) out_ok("Saved %s (%lu bytes)", full, (unsigned long)r.bytes);
    } else {
        PrintSink ps{0};
        good = http_fetch(&t, url, print_sink, &ps, &o, &r);
        // A body that did not end in a newline would run into the prompt.
        if (good && ps.n && !quiet) out_write("\n", 1);
    }
    http_net_set_timeout_ms(0);

    if (verbose && !quiet && r.status) out_info("HTTP %d", r.status);

    // A HEAD that came back with an error status still did its job, which was to
    // show the headers; v1 returned the status and printed no error either.
    if (!good && head && r.error == FETCH_ERR_STATUS) return 0;

    if (!good) { http_report_failure(&r); return 1; }
    return 0;
}

void http_register(void) {
    static const Command cr{"runurl", "fetch a script and run it", cmd_runurl, nullptr,
                            LEVEL_ADMIN};
    cmd_register(&cr);
    static const Command c{"wget", "download a file over HTTP", cmd_wget, nullptr};
    static const Command c2{"curl", "fetch a URL and print it (-I headers, -X, -d, -H)", cmd_curl, nullptr};
    cmd_register(&c);
    cmd_register(&c2);
}
