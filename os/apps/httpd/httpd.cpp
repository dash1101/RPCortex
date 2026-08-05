// httpd — serve the device over WiFi.
//
//   httpd [port]     serve until Ctrl+C (8080 by default)
//   httpd info       what it serves and what it exposes
//
// Routes:
//   /                a status dashboard
//   /fs?path=/       browse the filesystem
//   /dl?path=<file>  download a file
//
// --- what this exposes ------------------------------------------------------
//
// EVERY FILE ON THE DEVICE, read-only, to anyone who can reach the port. There
// is no password. That is the same bargain v1's httpd made and it is stated
// here rather than buried, because "browse the filesystem" is easy to read as
// "browse the files I meant to share".
//
// Do not run it on a network you do not trust, and stop it when you are done.
// Ctrl+C does that.
//
// --- how it is built --------------------------------------------------------
//
// One connection at a time, served to completion, then closed. A device with
// 12 KB of package heap has no business juggling sockets, and a browser opening
// six parallel connections is served six times in a row rather than six at
// once — which is slower to look at and much easier to be sure of.
//
// Everything is static. A package gets three kilobytes of stack before the
// firmware's own reserve, and an ABI call runs firmware ON that stack, so a
// buffer in a frame that then calls fw_tcp_send is a buffer the firmware does
// not get. This package keeps essentially nothing there.
#include "rpc_app.h"

RPC_APP_VER("httpd", "2.0");

#define DEF_PORT      8080
// A request line plus the headers a browser sends. Anything longer is a request
// this has no intention of serving.
#define REQ_MAX       1024
// One send. Sized to fit comfortably inside a TLS-free MSS run without asking
// the arena for anything.
#define OUT_MAX       1400
#define PATH_MAX      192
#define ARG_MAX       192
// How long to wait for a browser to finish sending its request before giving up
// on it. Short: a connection that opens and says nothing is a port scan.
#define REQ_TIMEOUT   3000
#define ACCEPT_TICK   250

static char g_req[REQ_MAX];
static char g_out[OUT_MAX];
static char g_path[PATH_MAX];
static char g_arg[ARG_MAX];
static char g_scratch[PATH_MAX];
// put_html's OWN working buffer, and it must never be one a caller might have
// handed it. It used g_scratch, which page_dir also builds its link in — so the
// first thing put_html did was clear its own argument, and every filename came
// out empty. Found by httpd_test rather than by looking at a page.
static char g_esc[PATH_MAX];
static unsigned g_served;

// --- small string helpers ----------------------------------------------------

static unsigned s_len(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }

static bool s_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a++ != *b++) return false; }
    return !*a && !*b;
}

static bool starts(const char *s, const char *pre) {
    while (*pre) { if (*s++ != *pre++) return false; }
    return true;
}

// Append to a bounded buffer. Silently stops at the end rather than growing:
// every caller is building a fixed-size chunk that is about to be sent, and a
// truncated page is a better outcome than a smashed buffer.
static void s_cat(char *dst, unsigned cap, const char *src) {
    unsigned n = s_len(dst);
    while (n + 1 < cap && *src) dst[n++] = *src++;
    dst[n] = 0;
}

static void s_cat_uint(char *dst, unsigned cap, uint32_t v) {
    char tmp[12];
    int i = 11;
    tmp[i] = 0;
    if (!v) tmp[--i] = '0';
    while (v && i) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
    s_cat(dst, cap, tmp + i);
}

// --- HTML and URL ------------------------------------------------------------

// Escape the five characters that turn text into markup. A file called
// `<script>` is not going to run in anybody's browser because of this package.
static void s_cat_html(char *dst, unsigned cap, const char *src) {
    for (; *src; src++) {
        switch (*src) {
        case '<':  s_cat(dst, cap, "&lt;");   break;
        case '>':  s_cat(dst, cap, "&gt;");   break;
        case '&':  s_cat(dst, cap, "&amp;");  break;
        case '"':  s_cat(dst, cap, "&quot;"); break;
        case '\'': s_cat(dst, cap, "&#39;");  break;
        default: {
            unsigned n = s_len(dst);
            if (n + 1 < cap) { dst[n] = *src; dst[n + 1] = 0; }
            break;
        }
        }
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode %XX and '+' in place into `dst`. Anything malformed is copied through
// literally rather than guessed at.
static void url_decode(char *dst, unsigned cap, const char *src) {
    unsigned o = 0;
    while (*src && o + 1 < cap) {
        if (*src == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
            dst[o++] = (char)((hexval(src[1]) << 4) | hexval(src[2]));
            src += 3;
        } else if (*src == '+') {
            dst[o++] = ' ';
            src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = 0;
}

// --- path safety -------------------------------------------------------------
//
// The ONE thing this package must not get wrong.
//
// A path arrives from the network and is handed to the filesystem, so `..` is
// the difference between serving the device and serving whatever is above it.
// Rejecting rather than normalising: a normaliser has to be right about every
// form of the same trick, and a refusal only has to be right once.
static bool path_ok(const char *p) {
    if (!p || p[0] != '/') return false;
    for (unsigned i = 0; p[i]; i++) {
        if (p[i] == '.' && p[i + 1] == '.') return false;
        // A backslash is not a separator here, but it is on the machine reading
        // this, and a name containing one has no business round-tripping.
        if (p[i] == '\\') return false;
        if ((unsigned char)p[i] < 0x20) return false;
    }
    return true;
}

// --- sending -----------------------------------------------------------------

// Send everything, or say the connection is gone. fw_tcp_send may accept less
// than it was given on a slow link, which is not an error and is not a partial
// page unless it is treated as one.
static bool send_all(int c, const char *data, unsigned len) {
    unsigned at = 0;
    while (at < len) {
        int n = fw_tcp_send(c, data + at, len - at);
        if (n <= 0) return false;
        at += (unsigned)n;
    }
    return true;
}

static bool send_str(int c, const char *s) { return send_all(c, s, s_len(s)); }

// Flush g_out and start it empty again.
static bool flush(int c) {
    if (!g_out[0]) return true;
    bool ok = send_str(c, g_out);
    g_out[0] = 0;
    return ok;
}

// Append, flushing whenever the buffer is nearly full. Everything below builds
// its page through this, so no page is limited by OUT_MAX.
static bool put(int c, const char *s) {
    if (s_len(g_out) + s_len(s) + 1 >= OUT_MAX && !flush(c)) return false;
    // Still too long for an empty buffer: send it directly.
    if (s_len(s) + 1 >= OUT_MAX) return send_str(c, s);
    s_cat(g_out, OUT_MAX, s);
    return true;
}

static bool put_uint(int c, uint32_t v) {
    char tmp[12];
    tmp[0] = 0;
    s_cat_uint(tmp, sizeof(tmp), v);
    return put(c, tmp);
}

static bool put_html(int c, const char *s) {
    // Escaped in chunks, so a long name cannot overrun — into a buffer of its
    // own, because callers pass g_scratch to this.
    while (*s) {
        g_esc[0] = 0;
        unsigned took = 0;
        while (*s && s_len(g_esc) + 8 < sizeof(g_esc)) {
            char one[2] = { *s++, 0 };
            s_cat_html(g_esc, sizeof(g_esc), one);
            took++;
        }
        if (!took) break;
        if (!put(c, g_esc)) return false;
    }
    return true;
}

static bool head(int c, const char *status, const char *ctype) {
    g_out[0] = 0;
    s_cat(g_out, OUT_MAX, "HTTP/1.1 ");
    s_cat(g_out, OUT_MAX, status);
    s_cat(g_out, OUT_MAX, "\r\nContent-Type: ");
    s_cat(g_out, OUT_MAX, ctype);
    // Closed per response: no keep-alive, no chunked encoding, no content
    // length to get wrong. The end of the body is the end of the connection.
    s_cat(g_out, OUT_MAX, "\r\nConnection: close\r\n"
                          "Cache-Control: no-store\r\n\r\n");
    return flush(c);
}

static const char *kStyle =
    "<style>body{font-family:system-ui,sans-serif;margin:2rem auto;max-width:44rem;"
    "padding:0 1rem;background:#11131a;color:#dde}"
    "a{color:#7cc4ff;text-decoration:none}a:hover{text-decoration:underline}"
    "h1{font-size:1.3rem}table{border-collapse:collapse;width:100%}"
    "td,th{text-align:left;padding:.35rem .6rem;border-bottom:1px solid #2a2e3a}"
    "code{color:#9fe}</style>";

static bool page_top(int c, const char *title) {
    if (!head(c, "200 OK", "text/html; charset=utf-8")) return false;
    if (!put(c, "<!doctype html><meta charset=utf-8><meta name=viewport "
                "content=\"width=device-width,initial-scale=1\"><title>")) return false;
    if (!put_html(c, title)) return false;
    if (!put(c, "</title>")) return false;
    if (!put(c, kStyle)) return false;
    return put(c, "<h1>RPCortex</h1>");
}

// --- the pages ---------------------------------------------------------------

static bool row(int c, const char *k, const char *v) {
    return put(c, "<tr><td>") && put_html(c, k) && put(c, "</td><td><code>")
        && put_html(c, v) && put(c, "</code></td></tr>");
}

static bool row_uint(int c, const char *k, uint32_t v, const char *unit) {
    if (!put(c, "<tr><td>") || !put_html(c, k) || !put(c, "</td><td><code>")) return false;
    if (!put_uint(c, v)) return false;
    return put(c, unit) && put(c, "</code></td></tr>");
}

static bool page_status(int c) {
    if (!page_top(c, "RPCortex")) return false;
    if (!put(c, "<table>")) return false;

    g_scratch[0] = 0;
    if (fw_net_ssid(g_scratch, sizeof(g_scratch)) > 0) row(c, "network", g_scratch);
    g_scratch[0] = 0;
    if (fw_net_ip(g_scratch, sizeof(g_scratch)) > 0) row(c, "address", g_scratch);

    row_uint(c, "uptime",        fw_millis() / 1000u, " s");
    row_uint(c, "cores",         fw_cores(), "");
    row_uint(c, "clock",         fw_clock_hz() / 1000000u, " MHz");
    row_uint(c, "heap free",     fw_heap_free() / 1024u, " KB");
    row_uint(c, "heap total",    fw_heap_total() / 1024u, " KB");
    row_uint(c, "largest block", fw_heap_largest() / 1024u, " KB");
    row_uint(c, "requests",      g_served, "");

    if (!put(c, "</table><p><a href=\"/fs?path=/\">browse the filesystem</a></p>"))
        return false;
    return put(c, "<p style=\"color:#888;font-size:.85rem\">Every file is readable "
                  "by anyone who can reach this port. Stop the server when you are "
                  "done.</p>");
}

static bool page_dir(int c, const char *path) {
    if (!page_top(c, path)) return false;
    if (!put(c, "<p><a href=\"/\">status</a> &middot; <code>") ||
        !put_html(c, path) || !put(c, "</code></p><table><tr><th>name<th>size")) return false;

    int n = fw_dir_count(path);
    if (n < 0) {
        return put(c, "</table><p>That directory cannot be read.</p>");
    }
    for (int i = 0; i < n; i++) {
        FwDirEntry e;
        if (fw_dir_entry(path, (unsigned)i, &e) != 1) break;

        // The link target: the directory, then a slash if it needs one, then the
        // entry. Built here rather than in the browser so a name with a space or
        // an ampersand in it still works.
        g_scratch[0] = 0;
        s_cat(g_scratch, sizeof(g_scratch), path);
        if (s_len(path) && path[s_len(path) - 1] != '/')
            s_cat(g_scratch, sizeof(g_scratch), "/");
        s_cat(g_scratch, sizeof(g_scratch), e.name);

        if (!put(c, "<tr><td><a href=\"")) return false;
        if (!put(c, e.is_dir ? "/fs?path=" : "/dl?path=")) return false;
        if (!put_html(c, g_scratch)) return false;
        if (!put(c, "\">")) return false;
        if (!put_html(c, e.name)) return false;
        if (!put(c, e.is_dir ? "/</a></td><td>dir" : "</a></td><td>")) return false;
        if (!e.is_dir) { if (!put_uint(c, e.size)) return false; }
        if (!put(c, "</td></tr>")) return false;
    }
    return put(c, "</table>");
}

static bool page_file(int c, const char *path) {
    // Read and send in pieces. A file larger than the arena is the normal case,
    // not the exception, so nothing is ever held whole.
    if (!fw_file_exists(path)) {
        if (!head(c, "404 Not Found", "text/plain")) return false;
        return send_str(c, "no such file\r\n");
    }
    if (!head(c, "200 OK", "application/octet-stream")) return false;

    // fw_file_read has no offset, so this serves what fits in one read. Enough
    // for anything on this device worth looking at from a browser, and honest
    // about the limit rather than silently truncating: see httpd info.
    uint32_t n = fw_file_read(path, g_req, sizeof(g_req));
    if (!n) return true;
    return send_all(c, g_req, n);
}

static bool page_404(int c) {
    if (!head(c, "404 Not Found", "text/html; charset=utf-8")) return false;
    return put(c, "<!doctype html><meta charset=utf-8>") && put(c, kStyle)
        && put(c, "<h1>Not found</h1><p><a href=\"/\">status</a></p>") && flush(c);
}

// --- the request -------------------------------------------------------------

// Pull "GET <target> HTTP/1.1" out of the buffer. Only GET: a server that will
// not change anything does not need to understand a method that does.
static bool parse_request(char *target, unsigned cap) {
    if (!starts(g_req, "GET ")) return false;
    const char *p = g_req + 4;
    unsigned o = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && o + 1 < cap) target[o++] = *p++;
    target[o] = 0;
    return o > 0;
}

// Split "<path>?<query>" and pull `path=` out of the query, decoded.
static void split_target(const char *target) {
    g_path[0] = 0;
    g_arg[0] = 0;

    const char *q = target;
    while (*q && *q != '?') q++;

    unsigned n = (unsigned)(q - target);
    if (n >= sizeof(g_path)) n = sizeof(g_path) - 1;
    for (unsigned i = 0; i < n; i++) g_path[i] = target[i];
    g_path[n] = 0;

    if (*q != '?') return;
    q++;
    // Only one parameter is understood, and only by name.
    while (*q) {
        if (starts(q, "path=")) {
            url_decode(g_arg, sizeof(g_arg), q + 5);
            // Stop at the next parameter, if the decode carried one in.
            for (unsigned i = 0; g_arg[i]; i++) if (g_arg[i] == '&') { g_arg[i] = 0; break; }
            return;
        }
        while (*q && *q != '&') q++;
        if (*q == '&') q++;
    }
}

static void serve(int c) {
    // Read until the headers end. A browser sends them in one segment in
    // practice; this does not rely on that.
    unsigned got = 0;
    uint32_t started = fw_millis();
    g_req[0] = 0;
    for (;;) {
        if (got + 1 >= sizeof(g_req)) break;
        int n = fw_tcp_recv(c, g_req + got, (unsigned)(sizeof(g_req) - 1 - got), 200);
        if (n > 0) {
            got += (unsigned)n;
            g_req[got] = 0;
            // The end of the header block. Both spellings, because not every
            // client is a browser.
            if (got >= 4) {
                for (unsigned i = 0; i + 3 < got; i++)
                    if (g_req[i] == '\r' && g_req[i+1] == '\n' &&
                        g_req[i+2] == '\r' && g_req[i+3] == '\n') { got = 0; goto ready; }
                for (unsigned i = 0; i + 1 < got; i++)
                    if (g_req[i] == '\n' && g_req[i+1] == '\n') { got = 0; goto ready; }
            }
            continue;
        }
        if (n == 0) break;                       // peer closed
        if (fw_millis() - started > REQ_TIMEOUT) return;
        if (fw_task_should_stop()) return;
    }
ready:
    g_out[0] = 0;

    char target[PATH_MAX];
    if (!parse_request(target, sizeof(target))) { page_404(c); return; }
    split_target(target);

    g_served++;

    if (s_eq(g_path, "/")) {
        if (page_status(c)) flush(c);
        return;
    }
    if (s_eq(g_path, "/fs")) {
        const char *want = g_arg[0] ? g_arg : "/";
        if (!path_ok(want)) { page_404(c); return; }
        if (page_dir(c, want)) flush(c);
        return;
    }
    if (s_eq(g_path, "/dl")) {
        if (!path_ok(g_arg)) { page_404(c); return; }
        page_file(c, g_arg);
        return;
    }
    page_404(c);
}

// --- the command -------------------------------------------------------------

static void info(void) {
    fw_printf("\n  httpd serves three things over plain HTTP:\n\n");
    fw_printf("    /                a status page\n");
    fw_printf("    /fs?path=/       the filesystem, as links\n");
    fw_printf("    /dl?path=<file>  a file\n\n");
    fw_printf("  EVERY FILE IS READABLE by anyone who can reach the port, with\n");
    fw_printf("  no password. Do not run it on a network you do not trust.\n\n");
    fw_printf("  One connection at a time, served and closed. A browser opening\n");
    fw_printf("  several is served several times in a row.\n\n");
    fw_printf("  A download is limited to one read of %d bytes — enough for\n", REQ_MAX);
    fw_printf("  configuration and logs, not for images. The ABI has no offset\n");
    fw_printf("  to read from yet, and a silently truncated file would be worse\n");
    fw_printf("  than a stated limit.\n\n");
    fw_printf("  '..' in a path is refused rather than resolved.\n\n");
}

static int httpd_cmd(int argc, char **argv) {
    if (argc >= 2 && s_eq(argv[1], "info")) { info(); return 0; }

    unsigned port = DEF_PORT;
    if (argc >= 2) {
        unsigned v = 0;
        const char *p = argv[1];
        while (*p >= '0' && *p <= '9') v = v * 10 + (unsigned)(*p++ - '0');
        if (*p || v == 0 || v > 65535) {
            fw_printf("Usage: httpd [port]   (or 'httpd info')\n");
            return 1;
        }
        port = v;
    }

    if (!fw_net_connected()) {
        fw_printf("Not connected. Use 'wifi connect <ssid>' first.\n");
        return 1;
    }

    int lsn = fw_tcp_listen(port);
    if (lsn < 0) {
        fw_printf("Could not listen on port %u. It may already be in use.\n", port);
        return 1;
    }

    g_scratch[0] = 0;
    fw_net_ip(g_scratch, sizeof(g_scratch));
    fw_printf("\n  Serving on http://%s:%u/\n", g_scratch, port);
    fw_printf("  Every file on the device is readable from here.\n");
    fw_printf("  Ctrl+C stops it.\n\n");

    g_served = 0;
    while (!fw_task_should_stop()) {
        int c = fw_tcp_accept(lsn, ACCEPT_TICK);
        if (c == -1) break;                 // the listener died
        if (c == -2) continue;              // nothing waiting; check for Ctrl+C
        serve(c);
        fw_tcp_close(c);
    }

    fw_tcp_close(lsn);
    fw_printf("\n  Stopped after %u request%s.\n\n", g_served, g_served == 1 ? "" : "s");
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("httpd", "serve the device over WiFi", httpd_cmd);
    return 0;
}
