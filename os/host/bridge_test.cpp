// The network bridge (core/bridge.cpp, LINK.md protocol 3), against a fake host.
//
// The bridge is an HttpTransport, so the tests drive it the way the device does:
// through http_fetch, with a sink, exactly as wget and pkg install call it. The
// fake host on the other end reads the device's frames and answers them the way
// LINK.md says a host must (FWAIT, FHEAD, FHDR, one FDATA per FACK, FDONE), and
// can be told to misbehave: go quiet, answer for another fetch, cut a body short,
// pass on a header that would reframe the body, refuse the URL.
//
// What it checks that matters most:
//   * a chunk is acknowledged only after the sink has had it, so the host is held
//     to the speed of the flash;
//   * whatever the reason the device stops (Ctrl+C, a size cap, a full disk), it
//     says FSTOP and then reads the wire clean, so nothing the host sent is left
//     over to be read as a request or typed at the prompt;
//   * the input door takes out the offer and stray 0x1E lines and never Ctrl+C.
#include "bridge.h"
#include "b64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>

static int checks = 0, fails = 0;
static void ok(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("    FAIL %s\n", what); fails++; }
}

static std::string b64(const std::string &s) {
    std::string out(b64_encoded_len((uint32_t)s.size()) + 1, '\0');
    uint32_t n = b64_encode((const uint8_t *)s.data(), (uint32_t)s.size(), &out[0]);
    out.resize(n);
    return out;
}

static std::string unb64(const std::string &s) {
    std::string out(s.size(), '\0');
    int32_t n = b64_decode(s.data(), (uint32_t)s.size(), (uint8_t *)&out[0], (uint32_t)out.size());
    if (n < 0) return "<bad base64>";
    out.resize((size_t)n);
    return out;
}

// --- the fake host -----------------------------------------------------------

struct Host {
    // What to answer with.
    int status = 200;
    std::string body;
    bool give_len = true;
    long long claim_len = -1;           // >= 0: say this len whatever the body is
    std::vector<std::string> headers;   // passed on as FHDR
    std::string fail_code;              // non-empty: FDONE err <code> <fail_msg>, no FHEAD
    std::string fail_msg;
    bool silent = false;                // never answer at all
    uint32_t slow_ms = 0;               // FWAIT for this long before FHEAD
    bool stale_lines = false;           // lines for other ids, and noise, in between
    bool noise = false;
    size_t cut_at = (size_t)-1;         // FDONE ok after this many body bytes
    int ctrl_c_after = -1;              // send 0x03 after this many FDATA lines
    bool overlong_line = false;         // one line far past BRIDGE_LINE_MAX before FHEAD
    bool after_stop_fdata = false;      // a late FDATA after FSTOP, before FDONE
    // Redirect the first fetch to this URL (the host not following it itself).
    std::string redirect_to;

    // What happened.
    std::vector<std::string> frames;    // every frame from the device, without 0x1E and \n
    std::vector<std::string> fetch_lines;
    std::vector<std::string> req_headers;
    std::string req_body;
    std::vector<unsigned long long> acks;
    std::vector<uint64_t> sink_at_ack;  // sink bytes when each FACK was written
    int stops = 0;
    int lost = 0;
    int fetches = 0;

    // The wire to the device, and a clock.
    std::deque<int> wire;
    uint32_t now = 1000;
    uint32_t last_wait = 0;
    bool waiting = false;               // FGO seen, FHEAD not sent yet (slow)
    uint32_t wait_until = 0;
    size_t sent = 0;
    int data_lines = 0;
    unsigned long cur = 0;
    bool done = false;
    uint64_t *sink_total = nullptr;
    bool stop_flag = false;
    bool intr = false;

    void line(const std::string &s) {
        wire.push_back(0x1E);
        for (char ch : s) wire.push_back((unsigned char)ch);
        wire.push_back('\n');
    }
    std::string id() const { return std::to_string(cur); }

    void head() {
        if (stale_lines) {
            line("FHEAD 999 500");
            line("FDATA 999 " + b64("not yours"));
        }
        if (noise) { for (char ch : std::string("[svc] tick\r\n")) wire.push_back(ch); }
        if (overlong_line) line("FHEAD " + id() + " 500 " + std::string(3000, 'x'));
        std::string h = "FHEAD " + id() + " " + std::to_string(redirect_to.size() && fetches == 1 ? 302 : status);
        long long len = claim_len >= 0 ? claim_len : (give_len ? (long long)body.size() : -1);
        if (redirect_to.size() && fetches == 1) len = 0;
        if (len >= 0) h += " len=" + std::to_string(len);
        line(h);
        for (auto &x : headers) line("FHDR " + id() + " " + b64(x));
        if (redirect_to.size() && fetches == 1) {
            line("FHDR " + id() + " " + b64("Location: " + redirect_to));
            line("FDONE " + id() + " ok");
            done = true;
            return;
        }
        next_data();
    }

    void next_data() {
        size_t lim = body.size() < cut_at ? body.size() : cut_at;
        if (sent >= lim) { line("FDONE " + id() + " ok"); done = true; return; }
        size_t k = lim - sent < BRIDGE_CHUNK ? lim - sent : BRIDGE_CHUNK;
        if (stale_lines && data_lines == 1) line("FDATA " + std::to_string(cur + 7) + " " + b64("stale"));
        if (noise && data_lines == 2) { for (char ch : std::string("half a line ")) wire.push_back(ch); }
        line("FDATA " + id() + " " + b64(body.substr(sent, k)));
        sent += k;
        data_lines++;
        if (ctrl_c_after >= 0 && data_lines == ctrl_c_after) wire.push_back(0x03);
    }

    void from_device(const std::string &f) {
        frames.push_back(f);
        std::string type = f.substr(0, f.find(' '));
        std::string rest = f.find(' ') == std::string::npos ? "" : f.substr(f.find(' ') + 1);
        if (type == "FETCH") {
            fetches++;
            fetch_lines.push_back(rest);
            cur = strtoul(rest.c_str(), nullptr, 10);
            sent = 0; data_lines = 0; done = false;
        } else if (type == "FHDR") {
            req_headers.push_back(unb64(rest.substr(rest.find(' ') + 1)));
        } else if (type == "FBODY") {
            req_body += unb64(rest.substr(rest.find(' ') + 1));
        } else if (type == "FGO") {
            if (silent) return;
            line("FWAIT " + id());
            if (!fail_code.empty()) { line("FDONE " + id() + " err " + fail_code + " " + fail_msg); done = true; return; }
            if (slow_ms) { waiting = true; wait_until = now + slow_ms; last_wait = now; return; }
            head();
        } else if (type == "FACK") {
            unsigned long long v = strtoull(rest.substr(rest.find(' ') + 1).c_str(), nullptr, 10);
            acks.push_back(v);
            sink_at_ack.push_back(sink_total ? *sink_total : 0);
            if (!done) next_data();
        } else if (type == "FSTOP") {
            stops++;
            if (after_stop_fdata) line("FDATA " + id() + " " + b64("late"));
            line("FDONE " + id() + " err cancelled stopped by the device");
            done = true;
        }
    }

    // Called when the device finds nothing waiting: time passes, and a slow
    // host keeps saying it is still there.
    void tick() {
        now += 5;
        if (waiting) {
            if (now >= wait_until) { waiting = false; head(); }
            else if (now - last_wait >= 2000) { line("FWAIT " + id()); last_wait = now; }
        }
    }
};

static Host *H;

static void io_write(void *, const char *d, uint32_t n) {
    ok(n >= 3 && d[0] == 0x1E && d[n - 1] == '\n', "every frame is 0x1E ... \\n, whole");
    H->from_device(std::string(d + 1, n - 2));
}
static int io_getc(void *) {
    if (H->wire.empty()) return -1;
    int c = H->wire.front();
    H->wire.pop_front();
    if (c == 0x03) H->intr = true;      // as shell/bridge.cpp raises it
    return c;
}
static uint32_t io_now(void *) { return H->now; }
static bool io_stop(void *) { return H->stop_flag || H->intr; }
static void io_idle(void *) { H->tick(); }
static void io_lost(void *) { H->lost++; }

// --- driving a fetch ---------------------------------------------------------

struct Sink {
    std::string got;
    uint64_t total = 0;
    size_t refuse_after = (size_t)-1;
    int stop_after = -1;                // raise the stop flag after this many calls
    int calls = 0;
};

static int sink_fn(void *ctx, const uint8_t *d, uint32_t n) {
    Sink *s = (Sink *)ctx;
    s->calls++;
    if (s->got.size() + n > s->refuse_after) return -1;
    s->got.append((const char *)d, n);
    s->total += n;
    if (s->stop_after >= 0 && s->calls == s->stop_after) H->stop_flag = true;
    return 0;
}

static int poll_fn(void *) { return (H->stop_flag || H->intr) ? 1 : 0; }

struct Hdrs { std::vector<std::string> lines; };
static void on_header(void *ctx, const char *line) { ((Hdrs *)ctx)->lines.push_back(line); }

static BridgeConn conn;
static bool conn_ready;

static bool fetch(Host &h, const char *url, Sink &s, FetchResult &r, FetchOpts *o = nullptr,
                  Hdrs *hd = nullptr) {
    H = &h;
    h.sink_total = &s.total;
    if (!conn_ready) {
        BridgeIo io{io_write, io_getc, io_now, io_stop, io_idle, io_lost, nullptr};
        bridge_init(&conn, &io);
        conn_ready = true;
    }
    HttpTransport t;
    bridge_transport(&conn, &t);
    FetchOpts def{};
    if (!o) o = &def;
    o->poll = poll_fn;
    if (hd) { o->on_header = on_header; o->on_header_ctx = hd; }
    return http_fetch(&t, url, sink_fn, &s, o, &r);
}

static std::string pattern(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = (char)((i * 131 + 7) & 0xFF);
    return s;
}

static bool has_frame(const Host &h, const char *type) {
    for (auto &f : h.frames) if (!f.compare(0, strlen(type), type)) return true;
    return false;
}

// --- the tests -----------------------------------------------------------------

static void test_simple(void) {
    printf("  a GET, with len, in several chunks\n");
    Host h; h.body = pattern(5000);
    h.headers = {"Content-Type: application/octet-stream", "ETag: \"abc\""};
    Sink s; FetchResult r; Hdrs hd;
    bool good = fetch(h, "https://raw.example.com/a/b.app", s, r, nullptr, &hd);
    ok(good, "the fetch succeeds");
    ok(r.status == 200, "status 200");
    ok(s.got == h.body, "the body arrives intact");
    ok(h.fetches == 1 && h.fetch_lines[0].find(" GET https://raw.example.com/a/b.app") != std::string::npos,
       "FETCH carries the method and the URL, default port left out");
    ok(has_frame(h, "FGO"), "FGO ends the request");
    bool chunk_ok = false;
    for (auto &f : h.frames) if (f.find("FGO") == 0 && f.find("chunk=768") != std::string::npos) chunk_ok = true;
    ok(chunk_ok, "FGO says the chunk size");
    ok(h.acks.size() == 7, "one FACK per FDATA (5000 bytes in 7 chunks)");
    ok(!h.acks.empty() && h.acks.back() == 5000, "the last FACK counts every byte");
    bool after = true;
    for (size_t i = 0; i < h.acks.size(); i++) if (h.sink_at_ack[i] < h.acks[i]) after = false;
    ok(after, "each FACK is sent only after the sink has had that chunk");
    ok(h.wire.empty(), "nothing is left on the wire afterwards");
    ok(h.stops == 0, "no FSTOP on a fetch that finished");
    bool ct = false, et = false;
    for (auto &l : hd.lines) { if (l == "Content-Type: application/octet-stream") ct = true; if (l == "ETag: \"abc\"") et = true; }
    ok(ct && et, "the headers the host passed on reach the caller");
    for (auto &x : h.req_headers) {
        ok(x.find("Host:") != 0 && x.find("User-Agent:") != 0 && x.find("Accept-Encoding:") != 0 &&
           x.find("Connection:") != 0, "httpfetch's own headers are not forwarded");
    }
}

static void test_no_len(void) {
    printf("  no len: the body ends at FDONE\n");
    Host h; h.body = pattern(2000); h.give_len = false;
    Sink s; FetchResult r;
    ok(fetch(h, "http://example.com/x", s, r), "succeeds");
    ok(s.got == h.body, "all of it");
    ok(h.wire.empty(), "wire clean");
}

static void test_big(void) {
    printf("  a 300 KB body, as an update image would be\n");
    Host h; h.body = pattern(300 * 1024 + 17);
    Sink s; FetchResult r;
    ok(fetch(h, "https://raw.example.com/rpcortex.bin", s, r), "succeeds");
    ok(s.got == h.body, "byte for byte");
    ok(r.bytes == h.body.size(), "FetchResult counts it");
}

static void test_request_shape(void) {
    printf("  a POST with headers and a body, on another port\n");
    Host h; h.body = "{\"ok\":1}";
    Sink s; FetchResult r;
    FetchOpts o{};
    o.method = "POST";
    char hdrs[HTTP_EXTRA_MAX + 1] = "";
    http_add_header(hdrs, sizeof(hdrs), "X-Test: one");
    http_add_header(hdrs, sizeof(hdrs), "Authorization: Bearer t");
    o.headers = hdrs;
    std::string body(2000, 'b');     // more than one FBODY
    o.body = (const uint8_t *)body.data();
    o.body_len = (uint32_t)body.size();
    ok(fetch(h, "http://example.com:8080/p?q=1", s, r, &o), "succeeds");
    ok(!h.fetch_lines.empty() && h.fetch_lines.back().find(" POST http://example.com:8080/p?q=1") != std::string::npos,
       "the port is kept when it is not the scheme's");
    bool x = false, auth = false, ctype = false;
    for (auto &l : h.req_headers) {
        if (l == "X-Test: one") x = true;
        if (l == "Authorization: Bearer t") auth = true;
        if (l == "Content-Type: application/x-www-form-urlencoded") ctype = true;
    }
    ok(x && auth && ctype, "the caller's headers, and the body's type, are forwarded");
    ok(h.req_body == body, "the body arrives in FBODY frames, whole");
}

static void test_head(void) {
    printf("  HEAD: headers and no body\n");
    Host h; h.body = ""; h.claim_len = 868604;
    Sink s; FetchResult r; FetchOpts o{}; o.method = "HEAD"; Hdrs hd;
    ok(fetch(h, "https://example.com/big.bin", s, r, &o, &hd), "succeeds");
    ok(s.got.empty(), "no body");
    bool cl = false;
    for (auto &l : hd.lines) if (l == "Content-Length: 868604") cl = true;
    ok(cl, "the length the host gave is shown");
    ok(h.wire.empty(), "wire clean");
}

static void test_status(void) {
    printf("  a 404 fails as a 404\n");
    Host h; h.status = 404; h.body = "Not Found";
    Sink s; FetchResult r;
    ok(!fetch(h, "https://example.com/missing", s, r), "fails");
    ok(r.error == FETCH_ERR_STATUS && r.status == 404, "FETCH_ERR_STATUS, 404");
    ok(h.wire.empty(), "the rest of the answer is read off the wire");
}

static void test_host_errors(void) {
    printf("  the host refuses, or cannot reach the server\n");
    {
        Host h; h.fail_code = "refused"; h.fail_msg = "example.org is not on the allowlist";
        Sink s; FetchResult r;
        ok(!fetch(h, "https://example.org/x", s, r), "fails");
        ok(strstr(bridge_detail(&conn), "will not fetch") && strstr(bridge_detail(&conn), "allowlist"),
           "the detail says the computer refused, and why");
        ok(h.lost == 0, "a refusal does not withdraw the offer");
    }
    {
        Host h; h.fail_code = "net"; h.fail_msg = "Name or service not known";
        Sink s; FetchResult r;
        ok(!fetch(h, "https://nowhere.invalid/", s, r), "fails");
        ok(strstr(bridge_detail(&conn), "could not fetch it: Name or service"), "net error worded");
    }
    {
        Host h; h.fail_code = "toobig"; h.fail_msg = "16 MB is over the 8 MB cap";
        Sink s; FetchResult r;
        ok(!fetch(h, "https://example.com/huge", s, r), "fails");
        ok(strstr(bridge_detail(&conn), "size limit"), "toobig worded");
    }
}

static void test_quiet(void) {
    printf("  no host at all: a plain message after the quiet time\n");
    Host h; h.silent = true;
    Sink s; FetchResult r;
    uint32_t t0 = h.now;
    ok(!fetch(h, "https://example.com/x", s, r), "fails");
    ok(strstr(bridge_detail(&conn), "did not answer"), "says the computer did not answer");
    ok(h.lost == 1, "and withdraws the offer");
    ok(h.now - t0 >= BRIDGE_QUIET_MS && h.now - t0 < BRIDGE_QUIET_MS + 1000, "after the quiet time, not before");
}

static void test_slow(void) {
    printf("  a slow server: FWAIT keeps the fetch alive past the quiet time\n");
    Host h; h.body = pattern(1000); h.slow_ms = 3 * BRIDGE_QUIET_MS;
    Sink s; FetchResult r;
    ok(fetch(h, "https://slow.example.com/", s, r), "succeeds after 30 s of FWAIT");
    ok(s.got == h.body, "body intact");
    ok(h.lost == 0, "the offer stands");
}

static void test_stale_and_noise(void) {
    printf("  lines for another fetch, noise and an overlong line are ignored\n");
    Host h; h.body = pattern(3000); h.stale_lines = true; h.noise = true; h.overlong_line = true;
    Sink s; FetchResult r;
    ok(fetch(h, "https://example.com/x", s, r), "succeeds");
    ok(s.got == h.body, "only this fetch's bytes reach the sink");
}

static void test_truncated(void) {
    printf("  a body cut short of its len fails\n");
    Host h; h.body = pattern(4000); h.cut_at = 1500;
    Sink s; FetchResult r;
    ok(!fetch(h, "https://example.com/x", s, r), "fails");
    ok(r.error == FETCH_ERR_PROTOCOL, "as a truncated response");
}

static void test_reframing_header(void) {
    printf("  a Transfer-Encoding from the host does not reframe the body\n");
    Host h; h.body = "plain bytes, not chunks"; h.headers = {"Transfer-Encoding: chunked", "Content-Length: 3"};
    Sink s; FetchResult r;
    ok(fetch(h, "https://example.com/x", s, r), "succeeds");
    ok(s.got == h.body, "the body is the body");
}

static void test_cap(void) {
    printf("  over the caller's cap: FSTOP, and the wire read clean\n");
    Host h; h.body = pattern(10000); h.after_stop_fdata = true;
    Sink s; FetchResult r; FetchOpts o{}; o.max_bytes = 3000;
    ok(!fetch(h, "https://example.com/big", s, r, &o), "fails");
    ok(r.error == FETCH_ERR_TOO_BIG, "too big");
    ok(h.stops == 1, "the device said FSTOP");
    ok(h.wire.empty(), "the late FDATA and the FDONE were read off the wire");
}

static void test_sink_full(void) {
    printf("  the filesystem fills: FSTOP and clean\n");
    Host h; h.body = pattern(6000);
    Sink s; s.refuse_after = 2500; FetchResult r;
    ok(!fetch(h, "https://example.com/big", s, r), "fails");
    ok(r.error == FETCH_ERR_SINK, "could not write");
    ok(h.stops == 1 && h.wire.empty(), "FSTOP, then drained");
}

static void test_ctrl_c(void) {
    printf("  Ctrl+C from the host side mid-body: cancelled, clean\n");
    Host h; h.body = pattern(20000); h.ctrl_c_after = 3;
    Sink s; FetchResult r;
    ok(!fetch(h, "https://example.com/big", s, r), "fails");
    ok(r.error == FETCH_ERR_ABORTED, "as a cancel, not a lost connection");
    ok(h.stops == 1 && h.wire.empty(), "FSTOP, then drained");
    h.intr = false;
}

static void test_stop_flag(void) {
    printf("  Ctrl+C seen by intr_check (the stop flag): the same\n");
    Host h; h.body = pattern(20000);
    Sink s; s.stop_after = 4; FetchResult r;
    ok(!fetch(h, "https://example.com/big", s, r), "fails");
    ok(r.error == FETCH_ERR_ABORTED, "cancelled");
    ok(h.stops == 1 && h.wire.empty(), "FSTOP, then drained");
}

static void test_redirect(void) {
    printf("  a redirect the host passes on is followed as a second fetch\n");
    Host h; h.body = pattern(900); h.redirect_to = "https://cdn.example.com/real.app";
    Sink s; FetchResult r;
    ok(fetch(h, "https://example.com/latest.app", s, r), "succeeds");
    ok(h.fetches == 2, "two fetches");
    ok(h.fetch_lines.size() == 2 && h.fetch_lines[1].find("https://cdn.example.com/real.app") != std::string::npos,
       "the second goes to the Location");
    unsigned long a = strtoul(h.fetch_lines[0].c_str(), nullptr, 10);
    unsigned long b = strtoul(h.fetch_lines[1].c_str(), nullptr, 10);
    ok(b == a + 1, "each fetch has a new id");
    ok(s.got == h.body, "only the final body reaches the sink");
}

static void test_door(void) {
    printf("  the input door\n");
    BridgeDoor d{};
    bool offer = false;
    std::string passed;
    auto feed = [&](const std::string &in) {
        for (unsigned char ch : in) {
            int r = bridge_door(&d, ch, &offer);
            if (r >= 0) passed += (char)r;
        }
    };
    feed("ls\r");
    ok(passed == "ls\r" && !offer, "typing passes untouched");
    passed.clear();
    feed(std::string("\x1e\x1f"));
    ok(offer && passed.empty(), "0x1E 0x1F is the offer, and is not a key");
    offer = false;
    feed(std::string("\x1e" "FDATA 3 QUJD\n"));
    ok(passed.empty(), "a stray host line is dropped to its end");
    feed("pwd\r");
    ok(passed == "pwd\r", "and the next line is typing again");
    passed.clear();
    feed(std::string("\x1e" "FDATA 3 QU") + "\x03" + "x");
    ok(passed == "\x03x", "Ctrl+C always gets through, and ends the stray line");
    passed.clear();
    feed(std::string("\x1e" "\r") + "y");
    ok(passed == "y", "a lone 0x1E before Enter costs only itself");
    passed.clear();
    feed(std::string("\x1e") + "ab" + std::string(3000, 'z'));
    ok(passed.size() > 500, "a line longer than any frame is given back as typing");
}

static void test_offer(void) {
    printf("  the offer: nothing is sent until a host has made one\n");
    BridgeOffer o{};
    ok(!bridge_may_use(&o, true, true), "no offer: the bridge is not used, even from the console");
    bridge_offer_set(&o, true, true);
    ok(bridge_may_use(&o, true, true), "offered over USB, asked from the console's shell: used");
    ok(!bridge_may_use(&o, true, false), "asked from a background job or a remote login: not used");
    ok(bridge_offer_valid(&o, true), "and the offer still stands after that");
    bridge_offer_dtr(&o, true);
    ok(bridge_offer_valid(&o, true), "DTR raised again changes nothing");
    bridge_offer_dtr(&o, false);
    ok(!bridge_may_use(&o, true, true), "the port closed (DTR dropped): the offer is gone");
    bridge_offer_set(&o, true, true);
    ok(!bridge_may_use(&o, false, true), "the port gone without the callback seeing it: gone too");
    ok(!bridge_offer_valid(&o, true), "and it does not come back when a port opens again");
    bridge_offer_set(&o, true, false);
    ok(bridge_may_use(&o, false, true), "offered over a UART console (no DTR): stands");
    bridge_offer_set(&o, false, true);
    ok(!bridge_may_use(&o, true, true), "BRIDGE off ends it");
}

int main(void) {
    printf("bridge_test\n");
    test_offer();
    test_simple();
    test_no_len();
    test_big();
    test_request_shape();
    test_head();
    test_status();
    test_host_errors();
    test_quiet();
    test_slow();
    test_stale_and_noise();
    test_truncated();
    test_reframing_header();
    test_cap();
    test_sink_full();
    test_ctrl_c();
    test_stop_flag();
    test_redirect();
    test_door();
    printf("bridge_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
