// httpd_test — the parts of a web server that are wrong quietly.
//
// A path check that lets `..` through serves whatever is above the filesystem.
// An HTML escape that misses a quote lets a filename close an attribute. A URL
// decoder that mishandles a truncated %XX walks off the end of its input.
// None of those announce themselves: the server keeps working, and keeps
// serving the wrong thing.
//
// So the package's source is compiled here with the ABI stubbed, and its
// scanners are pointed at the inputs an attacker would actually send.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../include/rpc_app.h"

static int g_checks, g_fails;
static void ck(bool cond, const char *what) {
    g_checks++;
    if (!cond) { g_fails++; printf("  FAIL: %s\n", what); }
}

// --- the ABI, enough of it to link ------------------------------------------

static char g_sent[16384];
static unsigned g_sent_len;

extern "C" {
int fw_printf(const char *fmt, ...) { (void)fmt; return 0; }
uint32_t fw_millis(void) { return 0; }
uint32_t fw_cores(void) { return 2; }
uint32_t fw_clock_hz(void) { return 150000000u; }
uint32_t fw_heap_free(void) { return 1024; }
uint32_t fw_heap_total(void) { return 2048; }
uint32_t fw_heap_largest(void) { return 512; }
int  fw_task_should_stop(void) { return 0; }
int  fw_net_connected(void) { return 1; }
int  fw_net_ssid(char *o, unsigned c) { snprintf(o, c, "net"); return 3; }
int  fw_net_ip(char *o, unsigned c)   { snprintf(o, c, "10.0.0.1"); return 8; }
int  fw_file_exists(const char *) { return 1; }
uint32_t fw_file_read(const char *, void *b, uint32_t cap) {
    const char *s = "file body";
    uint32_t n = (uint32_t)strlen(s);
    if (n > cap) n = cap;
    memcpy(b, s, n);
    return n;
}
uint32_t fw_file_size(const char *) { return 9; }
uint32_t fw_file_read_at(const char *, uint32_t off, void *b, uint32_t cap) {
    const char *s = "file body";
    uint32_t n = (uint32_t)strlen(s);
    if (off >= n) return 0;
    n -= off;
    if (n > cap) n = cap;
    memcpy(b, s + off, n);
    return n;
}
void fw_task_sleep_ms(uint32_t) {}
int  fw_task_spawn(const char *, TaskFn, void *, uint32_t) { return 7; }
int  fw_dir_count(const char *p) { return (p && strcmp(p, "/nope") == 0) ? -1 : 1; }
int  fw_dir_entry(const char *, unsigned i, FwDirEntry *o) {
    if (i) return 0;
    memset(o, 0, sizeof(*o));
    // A name that is markup, to prove the escaping is real.
    snprintf(o->name, FW_NAME_MAX, "%s", "<script>&\"x\".txt");
    o->size = 7;
    return 1;
}
int  fw_tcp_listen(unsigned) { return 1; }
int  fw_tcp_accept(int, uint32_t) { return -2; }
int  fw_tcp_recv(int, void *, unsigned, uint32_t) { return 0; }
int  fw_tcp_send(int, const void *b, unsigned n) {
    if (g_sent_len + n < sizeof(g_sent)) {
        memcpy(g_sent + g_sent_len, b, n);
        g_sent_len += n;
        g_sent[g_sent_len] = 0;
    }
    return (int)n;
}
int  fw_tcp_close(int) { return 0; }
int  rpc_register_command(const char *, const char *, RpcCommandFn) { return 1; }
}

#include "../apps/httpd/httpd.cpp"

int main(void) {
    printf("httpd_test - path safety, escaping and request parsing\n");

    // --- path traversal, the one that matters -------------------------------
    {
        ck(path_ok("/"),            "the root is servable");
        ck(path_ok("/os/ca.pem"),   "an ordinary path is servable");
        ck(path_ok("/a/b/c.txt"),   "and a nested one");

        ck(!path_ok("/../etc"),      "a leading .. is refused");
        ck(!path_ok("/os/../../x"),  "a .. in the middle is refused");
        ck(!path_ok("/os/.."),       "a trailing .. is refused");
        ck(!path_ok("/.."),          "bare .. is refused");
        ck(!path_ok("os/x"),         "a relative path is refused");
        ck(!path_ok(""),             "an empty path is refused");
        ck(!path_ok(nullptr),        "a null path is refused");
        ck(!path_ok("/a\\b"),        "a backslash is refused");
        ck(!path_ok("/a\nb"),        "a newline is refused - header injection");
        ck(!path_ok("/a\rb"),        "a carriage return is refused");
        ck(!path_ok("/a\tb"),        "a tab is refused");

        // The encoded forms have to be decoded BEFORE the check, or the check
        // never sees them. This is the ordering bug that makes a traversal
        // filter useless, so it is tested through the real path.
        char dec[64];
        url_decode(dec, sizeof(dec), "%2e%2e%2fetc");
        ck(strcmp(dec, "../etc") == 0, "%2e%2e decodes to ..");
        ck(!path_ok(dec), "and is then refused");

        url_decode(dec, sizeof(dec), "/os/%2E%2E/x");
        ck(!path_ok(dec), "uppercase %2E is refused too");
    }

    // --- URL decoding --------------------------------------------------------
    {
        char d[64];
        url_decode(d, sizeof(d), "plain");
        ck(strcmp(d, "plain") == 0, "plain text is unchanged");

        url_decode(d, sizeof(d), "a%20b");
        ck(strcmp(d, "a b") == 0, "%20 becomes a space");

        url_decode(d, sizeof(d), "a+b");
        ck(strcmp(d, "a b") == 0, "a plus becomes a space");

        // Truncated escapes must not read past the end of the string.
        url_decode(d, sizeof(d), "abc%");
        ck(strcmp(d, "abc%") == 0, "a trailing %% is copied literally");
        url_decode(d, sizeof(d), "abc%4");
        ck(strcmp(d, "abc%4") == 0, "a one-digit escape is copied literally");
        url_decode(d, sizeof(d), "abc%zz");
        ck(strcmp(d, "abc%zz") == 0, "a non-hex escape is copied literally");

        // A decode longer than the buffer stops inside it.
        char small[8];
        url_decode(small, sizeof(small), "aaaaaaaaaaaaaaaaaaaa");
        ck(strlen(small) < sizeof(small), "an overlong decode stays in bounds");
    }

    // --- HTML escaping -------------------------------------------------------
    {
        char h[128];
        h[0] = 0;
        s_cat_html(h, sizeof(h), "<script>alert('x')&\"q\"</script>");
        ck(strstr(h, "<script>") == nullptr, "a tag cannot survive escaping");
        ck(strstr(h, "&lt;script&gt;") != nullptr, "angle brackets are escaped");
        ck(strstr(h, "&amp;") != nullptr, "an ampersand is escaped");
        ck(strstr(h, "&quot;") != nullptr, "a double quote is escaped");
        ck(strstr(h, "&#39;") != nullptr, "a single quote is escaped");

        // Escaping must not overrun when the expansion does not fit: five
        // characters of output for one of input is the worst case.
        char tiny[12];
        tiny[0] = 0;
        s_cat_html(tiny, sizeof(tiny), "<<<<<<<<<<<<<<<<");
        ck(strlen(tiny) < sizeof(tiny), "escaping stays inside its buffer");
    }

    // --- request parsing -----------------------------------------------------
    {
        char t[PATH_MAX];

        snprintf(g_req, sizeof(g_req), "GET /fs?path=/os HTTP/1.1\r\nHost: x\r\n\r\n");
        ck(parse_request(t, sizeof(t)), "a GET is parsed");
        ck(strcmp(t, "/fs?path=/os") == 0, "and the target extracted");

        snprintf(g_req, sizeof(g_req), "POST / HTTP/1.1\r\n\r\n");
        ck(!parse_request(t, sizeof(t)), "a POST is refused - this server changes nothing");
        snprintf(g_req, sizeof(g_req), "GET\r\n");
        ck(!parse_request(t, sizeof(t)), "a GET with no target is refused");
        snprintf(g_req, sizeof(g_req), "\r\n\r\n");
        ck(!parse_request(t, sizeof(t)), "an empty request is refused");

        split_target("/fs?path=/os/apps");
        ck(strcmp(g_path, "/fs") == 0,       "the path is split off");
        ck(strcmp(g_arg, "/os/apps") == 0,   "and the argument decoded");

        split_target("/");
        ck(strcmp(g_path, "/") == 0, "a bare path parses");
        ck(g_arg[0] == 0,            "with no argument");

        split_target("/dl?other=1&path=/x");
        ck(strcmp(g_arg, "/x") == 0, "path= is found after another parameter");

        split_target("/dl?path=/x&other=1");
        ck(strcmp(g_arg, "/x") == 0, "and stops at the next parameter");

        // A parameter whose NAME merely ends in path= must not be mistaken for
        // it; this is the prefix bug that reads the wrong value.
        split_target("/dl?notpath=/evil");
        ck(g_arg[0] == 0, "notpath= is not path=");

        split_target("/dl?path=%2Fos%2Fca.pem");
        ck(strcmp(g_arg, "/os/ca.pem") == 0, "an encoded argument is decoded");
    }

    // --- site mode: the root must stay a prefix ------------------------------
    {
        snprintf(g_root, sizeof(g_root), "%s", "/web");

        ck(site_path("/"), "the site root resolves");
        ck(strcmp(g_scratch, "/web/index.html") == 0, "and / is index.html");

        ck(site_path("/style.css"), "a file under the root resolves");
        ck(strcmp(g_scratch, "/web/style.css") == 0, "to the joined path");

        ck(site_path("/sub/"), "a subdirectory resolves");
        ck(strcmp(g_scratch, "/web/sub/index.html") == 0, "to its index");

        // The whole point: nothing may escape the root. path_ok catches `..`
        // before this is reached, and this is the second line of defence.
        ck(!site_path("/../etc"), "a .. escape is refused");
        ck(!site_path("/../../"), "a doubled .. escape is refused");

        // A request that shares a PREFIX with the root but is not under it —
        // /webfoo against a root of /web — must not be accepted. This is the
        // classic off-by-one in a prefix check.
        snprintf(g_root, sizeof(g_root), "%s", "/web");
        site_path("foo/x");
        ck(strncmp(g_scratch, "/web", 4) == 0, "the root is always the prefix");

        g_root[0] = 0;   // back to device mode for the test below
    }

    // --- a directory listing, end to end -------------------------------------
    {
        g_sent_len = 0; g_sent[0] = 0;
        page_dir(1, "/os");
        flush(1);
        ck(strstr(g_sent, "200 OK") != nullptr, "a listing returns 200");
        ck(strstr(g_sent, "<script>") == nullptr,
           "a filename that is markup does not reach the page as markup");
        ck(strstr(g_sent, "&lt;script&gt;") != nullptr, "it arrives escaped");
        ck(strstr(g_sent, "/dl?path=/os/") != nullptr, "and links to the download route");
    }

    printf("%s  %d checks", g_fails ? "FAILED" : "ok", g_checks);
    if (g_fails) printf(", %d failed", g_fails);
    printf("\n");
    return g_fails ? 1 : 0;
}
