// The fetch driver: redirects, limits, refusals and cancellation.
//
// Two transports are used. A fake one replays canned responses, which is how the
// failure modes get provoked on demand — a redirect loop, a server that closes
// mid-body, a sink that runs out of room. Then a real POSIX socket runs the same
// driver against a real python3 -m http.server, because a fake transport can
// only ever be wrong in the same direction as the code that reads it.
//
// What that leaves unproven on the host is the lwIP socket layer alone, which is
// exactly the point of putting the seam where it is.
#include "httpfetch.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <initializer_list>

static int checks = 0, fails = 0;
static void ok(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("    FAIL %s\n", what); fails++; }
}

// --- collecting sink --------------------------------------------------------

struct Buf { char d[8192]; uint32_t n; int refuse_after; };
static int sink(void *ctx, const uint8_t *b, uint32_t n) {
    Buf *o = (Buf *)ctx;
    if (o->refuse_after >= 0 && (int)(o->n + n) > o->refuse_after) return -1;
    if (o->n + n >= sizeof(o->d)) return -1;
    memcpy(o->d + o->n, b, n); o->n += n; o->d[o->n] = 0;
    return 0;
}

// --- fake transport ---------------------------------------------------------
//
// Hands back a scripted response per connection, in `step`-sized pieces so the
// driver is exercised at realistic and unrealistic read sizes alike.

struct Fake {
    const char *responses[8];
    int   count;
    int   conn;           // which connection we are on
    int   at;             // offset within the current response
    uint32_t step;
    bool  fail_open;
    bool  fail_send;
    bool  cut_short;      // close mid-body rather than finishing
    char  last_request[512];
    char  hosts[8][64];   // the host each connection asked for
};

static int fk_open(void *c, const char *host, uint16_t, bool) {
    Fake *f = (Fake *)c;
    if (f->fail_open) return -1;
    if (f->conn < 8) snprintf(f->hosts[f->conn], 64, "%s", host);
    f->at = 0;
    return 0;
}
static int fk_send(void *c, const uint8_t *d, uint32_t n) {
    Fake *f = (Fake *)c;
    if (f->fail_send) return -1;
    uint32_t k = n < sizeof(f->last_request) - 1 ? n : sizeof(f->last_request) - 1;
    memcpy(f->last_request, d, k); f->last_request[k] = 0;
    return 0;
}
static int fk_recv(void *c, uint8_t *buf, uint32_t cap) {
    Fake *f = (Fake *)c;
    if (f->conn >= f->count) return 0;
    const char *r = f->responses[f->conn];
    uint32_t len = (uint32_t)strlen(r);
    if (f->cut_short && len > 8) len -= 4;          // drop the tail
    if ((uint32_t)f->at >= len) return 0;           // clean close
    uint32_t step = f->step ? f->step : cap;
    uint32_t n = len - f->at;
    if (n > step) n = step;
    if (n > cap)  n = cap;
    memcpy(buf, r + f->at, n);
    f->at += n;
    return (int)n;
}
static void fk_close(void *c) { Fake *f = (Fake *)c; f->conn++; f->at = 0; }

static HttpTransport fake_transport(Fake *f) {
    HttpTransport t{};
    t.open = fk_open; t.send = fk_send; t.recv = fk_recv; t.close = fk_close; t.ctx = f;
    return t;
}

// --- real POSIX transport ---------------------------------------------------

struct Posix { int fd; };
static int px_open(void *c, const char *host, uint16_t port, bool tls) {
    if (tls) return -1;                    // stage 3's job
    Posix *p = (Posix *)c;
    p->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (p->fd < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(p->fd); p->fd = -1; return -1; }
    if (connect(p->fd, (sockaddr *)&a, sizeof(a)) != 0) { close(p->fd); p->fd = -1; return -1; }
    return 0;
}
static int px_send(void *c, const uint8_t *d, uint32_t n) {
    Posix *p = (Posix *)c;
    uint32_t sent = 0;
    while (sent < n) {
        ssize_t k = write(p->fd, d + sent, n - sent);
        if (k <= 0) return -1;
        sent += (uint32_t)k;
    }
    return 0;
}
static int px_recv(void *c, uint8_t *buf, uint32_t cap) {
    Posix *p = (Posix *)c;
    ssize_t k = read(p->fd, buf, cap);
    return k < 0 ? -1 : (int)k;
}
static void px_close(void *c) {
    Posix *p = (Posix *)c;
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
}

// --- helpers ----------------------------------------------------------------

static const char *R200 =
    "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world";

int main(void) {
    printf("  fetch\n");

    // --- the request we actually send --------------------------------------
    {
        HttpUrl u;
        http_parse_url("http://example.com/a/b.pkg", &u);
        char req[512];
        int n = http_build_request(req, sizeof(req), &u, nullptr);
        ok(n > 0, "request builds");
        ok(strstr(req, "GET /a/b.pkg HTTP/1.1\r\n") == req, "request line");
        ok(strstr(req, "Host: example.com\r\n") != nullptr, "Host header");
        // Without this a server may gzip the body and nothing here can undo it.
        ok(strstr(req, "Accept-Encoding: identity\r\n") != nullptr,
           "asks for no compression");
        ok(strstr(req, "Connection: close\r\n") != nullptr, "asks to close");
        ok(strstr(req, "\r\n\r\n") != nullptr, "header block terminated");

        char tiny[16];
        ok(http_build_request(tiny, sizeof(tiny), &u, nullptr) < 0,
           "refuses to build into too small a buffer");
    }

    // --- a plain success, at several read sizes ----------------------------
    for (uint32_t step : {(uint32_t)1, (uint32_t)7, (uint32_t)0}) {
        Fake f{}; f.responses[0] = R200; f.count = 1; f.step = step;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1;
        FetchResult r;
        bool good = http_fetch(&t, "http://example.com/x", sink, &b, nullptr, &r);
        char msg[80]; snprintf(msg, sizeof(msg), "200 at read size %u", step);
        ok(good && r.status == 200 && r.bytes == 11 && !strcmp(b.d, "hello world"), msg);
    }

    // --- redirects ----------------------------------------------------------
    {
        Fake f{};
        f.responses[0] = "HTTP/1.1 302 Found\r\nLocation: http://b.test/two\r\n"
                         "Content-Length: 5\r\n\r\nnope!";
        f.responses[1] = R200;
        f.count = 2;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1;
        FetchResult r;
        bool good = http_fetch(&t, "http://a.test/one", sink, &b, nullptr, &r);
        ok(good && r.status == 200, "redirect: followed");
        ok(r.redirects == 1, "redirect: counted");
        ok(!strcmp(b.d, "hello world"), "redirect: only the final body reaches the sink");
        ok(!strcmp(f.hosts[0], "a.test") && !strcmp(f.hosts[1], "b.test"),
           "redirect: second connection goes to the new host");
        ok(r.bytes == 11, "redirect: byte count excludes the redirect body");
    }
    {
        // A relative Location keeps the host.
        Fake f{};
        f.responses[0] = "HTTP/1.1 301 Moved\r\nLocation: /elsewhere\r\nContent-Length: 0\r\n\r\n";
        f.responses[1] = R200;
        f.count = 2;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1; FetchResult r;
        ok(http_fetch(&t, "http://a.test/one", sink, &b, nullptr, &r), "redirect: relative followed");
        ok(!strcmp(f.hosts[1], "a.test"), "redirect: relative keeps the host");
        ok(strstr(f.last_request, "GET /elsewhere ") != nullptr, "redirect: new path requested");
    }
    {
        // A loop must terminate rather than run until the watchdog notices.
        Fake f{};
        for (int i = 0; i < 8; i++)
            f.responses[i] = "HTTP/1.1 302 Found\r\nLocation: http://a.test/loop\r\n"
                             "Content-Length: 0\r\n\r\n";
        f.count = 8;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1; FetchResult r;
        ok(!http_fetch(&t, "http://a.test/loop", sink, &b, nullptr, &r), "redirect loop stops");
        ok(r.error == FETCH_ERR_REDIRECT, "redirect loop reports the right reason");
    }
    {
        // A redirect with a Location nothing can resolve.
        Fake f{};
        f.responses[0] = "HTTP/1.1 302 Found\r\nLocation: gopher://old.test/x\r\n"
                         "Content-Length: 0\r\n\r\n";
        f.count = 1;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1; FetchResult r;
        ok(!http_fetch(&t, "http://a.test/x", sink, &b, nullptr, &r), "unresolvable redirect fails");
        ok(r.error == FETCH_ERR_REDIRECT, "unresolvable redirect reason");
    }

    // --- statuses -----------------------------------------------------------
    {
        Fake f{};
        f.responses[0] = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot here!";
        f.count = 1;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1; FetchResult r;
        ok(!http_fetch(&t, "http://a.test/missing", sink, &b, nullptr, &r), "404 is a failure");
        ok(r.error == FETCH_ERR_STATUS && r.status == 404, "404 keeps the status");
        ok(strstr(r.detail, "404") != nullptr, "404 detail names the code");
    }

    // --- limits and refusals ------------------------------------------------
    {
        Fake f{}; f.responses[0] = R200; f.count = 1;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1;
        FetchOpts o{}; o.max_bytes = 5;
        FetchResult r;
        ok(!http_fetch(&t, "http://a.test/x", sink, &b, &o, &r), "size limit enforced");
        ok(r.error == FETCH_ERR_TOO_BIG, "size limit reason");
        // The cap must apply before the write, or a full filesystem is the
        // thing that reports the problem.
        ok(b.n == 0, "nothing written once the limit would be exceeded");
    }
    {
        Fake f{}; f.responses[0] = R200; f.count = 1;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = 4;         // "disk full" after 4 bytes
        FetchResult r;
        ok(!http_fetch(&t, "http://a.test/x", sink, &b, nullptr, &r), "sink refusal stops it");
        ok(r.error == FETCH_ERR_SINK, "sink refusal reason");
    }

    // --- truncation ---------------------------------------------------------
    {
        Fake f{}; f.responses[0] = R200; f.count = 1; f.cut_short = true;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1; FetchResult r;
        ok(!http_fetch(&t, "http://a.test/x", sink, &b, nullptr, &r),
           "a body cut short is a failure");
        ok(r.error == FETCH_ERR_PROTOCOL, "truncation reason");
        ok(strstr(r.detail, "truncated") != nullptr, "truncation detail is legible");
    }

    // --- transport failures -------------------------------------------------
    {
        Fake f{}; f.fail_open = true; f.count = 1;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1; FetchResult r;
        ok(!http_fetch(&t, "http://down.test:8080/x", sink, &b, nullptr, &r), "connect failure");
        ok(r.error == FETCH_ERR_CONNECT, "connect failure reason");
        ok(strstr(r.detail, "down.test:8080") != nullptr, "connect detail names host and port");
    }
    {
        Fake f{}; f.responses[0] = R200; f.count = 1; f.fail_send = true;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1; FetchResult r;
        ok(!http_fetch(&t, "http://a.test/x", sink, &b, nullptr, &r), "send failure");
        ok(r.error == FETCH_ERR_SEND, "send failure reason");
    }
    {
        Buf b{}; b.refuse_after = -1; FetchResult r;
        Fake f{}; HttpTransport t = fake_transport(&f);
        ok(!http_fetch(&t, "ftp://a.test/x", sink, &b, nullptr, &r), "bad scheme rejected early");
        ok(r.error == FETCH_ERR_URL, "bad scheme reason");
    }

    // --- cancellation -------------------------------------------------------
    {
        Fake f{}; f.responses[0] = R200; f.count = 1; f.step = 1;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1;
        static int ticks;
        ticks = 0;
        FetchOpts o{};
        o.poll = [](void *) { return ++ticks > 3 ? 1 : 0; };
        FetchResult r;
        ok(!http_fetch(&t, "http://a.test/x", sink, &b, &o, &r), "poll can cancel");
        ok(r.error == FETCH_ERR_ABORTED, "cancel reason");
    }

    // --- progress -----------------------------------------------------------
    {
        Fake f{}; f.responses[0] = R200; f.count = 1; f.step = 3;
        HttpTransport t = fake_transport(&f);
        Buf b{}; b.refuse_after = -1;
        static uint64_t last_got, last_total; static int calls;
        last_got = last_total = calls = 0;
        FetchOpts o{};
        o.progress = [](void *, uint64_t g, uint64_t tot) { last_got = g; last_total = tot; calls++; };
        FetchResult r;
        ok(http_fetch(&t, "http://a.test/x", sink, &b, &o, &r), "progress: fetch still works");
        ok(calls > 1, "progress: called repeatedly");
        ok(last_got == 11, "progress: final count is the body size");
        ok(last_total == 11, "progress: total comes from Content-Length");
    }

    // --- against a real server ----------------------------------------------
    {
        char dir[] = "/tmp/rpcfetchXXXXXX";
        bool ran = false;
        if (mkdtemp(dir)) {
            // A body big enough to span many segments, so the real socket
            // delivers it in pieces the fake never would.
            char path[256]; snprintf(path, sizeof(path), "%s/big.bin", dir);
            FILE *fp = fopen(path, "wb");
            static char payload[6000];
            for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (char)('a' + i % 26);
            fwrite(payload, 1, sizeof(payload), fp);
            fclose(fp);

            int port = 18080 + (getpid() % 900);
            // Flush first: fork() duplicates whatever is still buffered, and
            // the child would print this run's output a second time.
            fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) {
                // Quiet, and rooted at the temp directory.
                freopen("/dev/null", "w", stderr);
                freopen("/dev/null", "w", stdout);
                char ps[16]; snprintf(ps, sizeof(ps), "%d", port);
                execlp("python3", "python3", "-m", "http.server", ps,
                       "--bind", "127.0.0.1", "--directory", dir, (char *)nullptr);
                _exit(127);
            }
            if (pid > 0) {
                // Wait for it to accept, rather than sleeping a guessed amount.
                Posix probe{-1};
                char hostport[64]; snprintf(hostport, sizeof(hostport), "127.0.0.1");
                bool up = false;
                for (int i = 0; i < 100 && !up; i++) {
                    if (px_open(&probe, hostport, (uint16_t)port, false) == 0) {
                        px_close(&probe); up = true;
                    } else {
                        struct timespec ts{0, 50 * 1000 * 1000};
                        nanosleep(&ts, nullptr);
                    }
                }
                if (up) {
                    ran = true;
                    Posix px{-1};
                    HttpTransport t{};
                    t.open = px_open; t.send = px_send; t.recv = px_recv; t.close = px_close;
                    t.ctx = &px;

                    char url[128];
                    snprintf(url, sizeof(url), "http://127.0.0.1:%d/big.bin", port);
                    Buf b{}; b.refuse_after = -1;
                    FetchResult r;
                    bool good = http_fetch(&t, url, sink, &b, nullptr, &r);
                    ok(good, "real server: fetch succeeded");
                    ok(r.status == 200, "real server: 200");
                    ok(b.n == sizeof(payload), "real server: whole body received");
                    ok(memcmp(b.d, payload, sizeof(payload)) == 0,
                       "real server: bytes are byte-for-byte correct");

                    snprintf(url, sizeof(url), "http://127.0.0.1:%d/nope.bin", port);
                    Buf b2{}; b2.refuse_after = -1;
                    FetchResult r2;
                    ok(!http_fetch(&t, url, sink, &b2, nullptr, &r2), "real server: 404 fails");
                    ok(r2.status == 404, "real server: 404 status reported");

                    // A cap smaller than the file, against a real socket.
                    snprintf(url, sizeof(url), "http://127.0.0.1:%d/big.bin", port);
                    Buf b3{}; b3.refuse_after = -1;
                    FetchOpts o{}; o.max_bytes = 1000;
                    FetchResult r3;
                    ok(!http_fetch(&t, url, sink, &b3, &o, &r3), "real server: size cap holds");
                    ok(r3.error == FETCH_ERR_TOO_BIG, "real server: size cap reason");
                }
                kill(pid, SIGTERM);
                waitpid(pid, nullptr, 0);
            }
            unlink(path);
            rmdir(dir);
        }
        if (!ran) printf("    SKIP real-server checks (no python3 http.server)\n");
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
