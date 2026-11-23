#include "bridge.h"
#include "b64.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The heap block, laid out once. OUT is the frame being written and lives the
// whole hop. REQ is the request while send() collects it; once it has gone out as
// frames the same bytes become the line being read, the chunk being handed out and
// the response head.
#define OUT_CAP    1280
#define REQ_AT     OUT_CAP
#define LINE_AT    REQ_AT
#define DATA_AT    (LINE_AT + BRIDGE_LINE_MAX)
#define HEAD_AT    (DATA_AT + BRIDGE_CHUNK)
#define HEAD_CAP   1200
#define MEM_TOTAL  (REQ_AT + BRIDGE_REQ_MAX)

static_assert(HEAD_AT + HEAD_CAP <= MEM_TOTAL, "the response half must fit where the request was");
static_assert(((BRIDGE_CHUNK + 2) / 3) * 4 + 24 < OUT_CAP, "an FBODY frame must fit");

enum Phase : uint8_t { P_IDLE = 0, P_REQ, P_RESP };

static char *out_buf(BridgeConn *c)  { return (char *)c->mem; }
static char *line_buf(BridgeConn *c) { return (char *)c->mem + LINE_AT; }
static uint8_t *data_buf(BridgeConn *c) { return c->mem + DATA_AT; }
static char *head_buf(BridgeConn *c) { return (char *)c->mem + HEAD_AT; }

void bridge_init(BridgeConn *c, const BridgeIo *io) {
    memset(c, 0, sizeof(*c));
    c->io = *io;
    c->next_id = 1;
}

const char *bridge_detail(const BridgeConn *c) { return c->detail; }

static void set_detail(BridgeConn *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->detail, sizeof(c->detail), fmt, ap);
    va_end(ap);
}

// --- writing frames ------------------------------------------------------------

static void frame(BridgeConn *c, const char *fmt, ...) {
    char *o = out_buf(c);
    o[0] = 0x1E;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(o + 1, OUT_CAP - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > OUT_CAP - 3) n = OUT_CAP - 3;
    o[1 + n] = '\n';
    c->io.write(c->io.ctx, o, (uint32_t)n + 2);
}

// TYPE id, then `n` bytes in base64. `n` is at most BRIDGE_CHUNK.
static void frame_b64(BridgeConn *c, const char *type, const uint8_t *d, uint32_t n) {
    char *o = out_buf(c);
    int h = snprintf(o, OUT_CAP, "\x1e%s %lu ", type, (unsigned long)c->id);
    uint32_t e = b64_encode(d, n, o + h);
    o[h + e] = '\n';
    c->io.write(c->io.ctx, o, (uint32_t)h + e + 1);
}

// --- reading the host's lines --------------------------------------------------

enum LineResult { L_OK, L_STOP, L_QUIET };

// One line from the host: the bytes after a 0x1E, up to the newline. Anything
// not inside a 0x1E line is not the host's answer (somebody typing, a byte of
// noise) and is dropped. `stoppable` is false while draining after FSTOP, when a
// second Ctrl+C has nothing left to stop.
static LineResult read_line(BridgeConn *c, uint32_t quiet_ms, bool stoppable, uint32_t *len) {
    char *l = line_buf(c);
    uint32_t n = 0, last = c->io.now_ms(c->io.ctx);
    bool in_frame = false, over = false;
    for (;;) {
        if (stoppable && c->io.stop(c->io.ctx)) return L_STOP;
        int ch = c->io.getc(c->io.ctx);
        if (ch < 0) {
            if (c->io.now_ms(c->io.ctx) - last >= quiet_ms) return L_QUIET;
            c->io.idle(c->io.ctx);
            continue;
        }
        last = c->io.now_ms(c->io.ctx);
        if (ch == 0x03) { if (stoppable) return L_STOP; continue; }
        if (ch == 0x1E) { in_frame = true; n = 0; over = false; continue; }
        if (!in_frame || ch == '\r') continue;
        if (ch == '\n') {
            in_frame = false;
            if (over) continue;          // longer than any line the host may send
            l[n] = 0;
            *len = n;
            return L_OK;
        }
        if (n + 1 < BRIDGE_LINE_MAX) l[n++] = (char)ch;
        else over = true;
    }
}

// "TYPE id rest": the type and the rest, or false for a line with another id
// (a stale one, from a fetch that has ended) or none.
static bool split_line(BridgeConn *c, char *l, const char **type, const char **rest) {
    char *sp = strchr(l, ' ');
    if (!sp) return false;
    *sp = 0;
    *type = l;
    char *end = nullptr;
    unsigned long id = strtoul(sp + 1, &end, 10);
    if (end == sp + 1 || id != c->id) return false;
    while (*end == ' ') end++;
    *rest = end;
    return true;
}

// --- giving up -----------------------------------------------------------------

// The device is stopping (Ctrl+C, a body too big to store, a full filesystem):
// say so, then read up to the host's FDONE so that nothing it already sent is
// left on the wire to be read as a request, or typed at the prompt.
static void drain(BridgeConn *c, bool send_stop) {
    if (send_stop) frame(c, "FSTOP %lu", (unsigned long)c->id);
    uint32_t start = c->io.now_ms(c->io.ctx);
    while (c->io.now_ms(c->io.ctx) - start < BRIDGE_DRAIN_MS) {
        uint32_t n = 0;
        uint32_t left = BRIDGE_DRAIN_MS - (c->io.now_ms(c->io.ctx) - start);
        if (read_line(c, left, false, &n) != L_OK) break;
        const char *type, *rest;
        if (split_line(c, line_buf(c), &type, &rest) && !strcmp(type, "FDONE")) break;
    }
    c->ended = true;
}

static void fail(BridgeConn *c) { c->failed = true; }

static void went_quiet(BridgeConn *c) {
    set_detail(c, "the computer did not answer for %u s; is rpc or the browser tool still attached?",
               (unsigned)(BRIDGE_QUIET_MS / 1000));
    c->ended = true;
    fail(c);
    if (c->io.lost) c->io.lost(c->io.ctx);
}

static void stopped(BridgeConn *c) {
    drain(c, true);
    set_detail(c, "cancelled");
    fail(c);
}

// FDONE <id> err <code> <message>.
static void host_failed(BridgeConn *c, const char *rest) {
    const char *code = rest;
    if (!strncmp(code, "err", 3)) code += 3;
    while (*code == ' ') code++;
    const char *msg = strchr(code, ' ');
    size_t clen = msg ? (size_t)(msg - code) : strlen(code);
    msg = msg ? msg + 1 : "";
    if (clen == 7 && !strncmp(code, "refused", 7))
        set_detail(c, "the computer will not fetch that: %.80s", msg);
    else if (clen == 6 && !strncmp(code, "toobig", 6))
        set_detail(c, "over the computer's size limit: %.80s", msg);
    else if (clen == 7 && !strncmp(code, "timeout", 7))
        set_detail(c, "the server went quiet: %.90s", msg);
    else if (clen == 9 && !strncmp(code, "cancelled", 9))
        set_detail(c, "cancelled");
    else
        set_detail(c, "the computer could not fetch it: %.80s", msg[0] ? msg : "no reason given");
    c->ended = true;
    fail(c);
}

// --- the request ---------------------------------------------------------------

// Headers httpfetch writes itself, which the host replaces with its own.
static bool header_is(const char *line, uint32_t n, const char *name) {
    size_t k = strlen(name);
    if (n <= k || line[k] != ':') return false;
    for (size_t i = 0; i < k; i++) {
        char ch = line[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (ch != name[i]) return false;
    }
    return true;
}

static bool hop_by_hop(const char *line, uint32_t n) {
    return header_is(line, n, "host") || header_is(line, n, "user-agent") ||
           header_is(line, n, "accept-encoding") || header_is(line, n, "connection") ||
           header_is(line, n, "content-length");
}

// Turn the request httpfetch sent into frames. False, with the reason set, when
// it is not one this can carry.
static bool issue(BridgeConn *c) {
    char *req = (char *)c->mem + REQ_AT;
    uint32_t n = c->req_len;
    // The headers end at the first blank line; the body is what follows it.
    uint32_t hend = 0;
    for (uint32_t i = 0; i + 3 < n; i++) {
        if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') {
            hend = i;
            break;
        }
    }
    if (!hend) { set_detail(c, "the request could not be read"); return false; }

    // "METHOD /path HTTP/1.1"
    uint32_t i = 0;
    char method[12];
    uint32_t m = 0;
    while (i < hend && req[i] != ' ' && m + 1 < sizeof(method)) {
        char ch = req[i++];
        if (ch < 'A' || ch > 'Z') { set_detail(c, "the request could not be read"); return false; }
        method[m++] = ch;
    }
    method[m] = 0;
    if (!m || i >= hend || req[i] != ' ') { set_detail(c, "the request could not be read"); return false; }
    uint32_t path_at = ++i;
    while (i < hend && req[i] != ' ' && req[i] != '\r') i++;
    uint32_t path_len = i - path_at;
    if (!path_len || req[path_at] != '/') { set_detail(c, "the request could not be read"); return false; }

    c->id = c->next_id++;
    if (!c->next_id) c->next_id = 1;

    char port[8] = "";
    if ((c->tls && c->port != 443) || (!c->tls && c->port != 80))
        snprintf(port, sizeof(port), ":%u", (unsigned)c->port);
    frame(c, "FETCH %lu %s %s://%s%s%.*s", (unsigned long)c->id, method,
          c->tls ? "https" : "http", c->host, port, (int)path_len, req + path_at);

    // The headers after the request line, one frame each.
    while (i < hend && req[i] != '\n') i++;
    i++;
    while (i < hend) {
        uint32_t s = i;
        while (i < hend && req[i] != '\r') i++;
        uint32_t len = i - s;
        if (len && !hop_by_hop(req + s, len))
            frame_b64(c, "FHDR", (const uint8_t *)req + s, len < BRIDGE_CHUNK ? len : BRIDGE_CHUNK);
        i += 2;                                  // the CRLF
    }

    for (uint32_t at = hend + 4; at < n; at += BRIDGE_CHUNK) {
        uint32_t k = n - at < BRIDGE_CHUNK ? n - at : BRIDGE_CHUNK;
        frame_b64(c, "FBODY", (const uint8_t *)req + at, k);
    }
    frame(c, "FGO %lu chunk=%d", (unsigned long)c->id, BRIDGE_CHUNK);
    return true;
}

// --- the response --------------------------------------------------------------

static void head_add(BridgeConn *c, const char *fmt, ...) {
    char *h = head_buf(c);
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(h + c->head_len, HEAD_CAP - c->head_len, fmt, ap);
    va_end(ap);
    if (n > 0 && c->head_len + (uint32_t)n < HEAD_CAP) c->head_len += (uint32_t)n;
}

// A response header from the host, if it is one that may be passed on. Anything
// that decides how the body is framed stays out: the body's length is FHEAD's
// len, and a "Transfer-Encoding: chunked" from the host would have the parser
// looking for chunk sizes in plain data.
static void head_header(BridgeConn *c, const char *b64, uint32_t n) {
    uint8_t *tmp = data_buf(c);            // free until the first FDATA
    int32_t m = b64_decode(b64, n, tmp, BRIDGE_HDR_MAX);
    if (m <= 0) return;
    const char *line = (const char *)tmp;
    bool colon = false;
    for (int32_t i = 0; i < m; i++) {
        if (tmp[i] == '\r' || tmp[i] == '\n' || tmp[i] == 0) return;
        if (tmp[i] == ':') colon = true;
    }
    if (!colon) return;
    if (header_is(line, (uint32_t)m, "content-length") ||
        header_is(line, (uint32_t)m, "transfer-encoding") ||
        header_is(line, (uint32_t)m, "content-encoding") ||
        header_is(line, (uint32_t)m, "connection")) return;
    // Room is kept for the lines that finish the head.
    if (c->head_len + (uint32_t)m + 2 + 64 >= HEAD_CAP) return;
    memcpy(head_buf(c) + c->head_len, tmp, (size_t)m);
    c->head_len += (uint32_t)m;
    memcpy(head_buf(c) + c->head_len, "\r\n", 2);
    c->head_len += 2;
}

static void head_finish(BridgeConn *c) {
    if (c->len >= 0) head_add(c, "Content-Length: %lld\r\n", (long long)c->len);
    head_add(c, "Connection: close\r\n\r\n");
}

static bool take_data(BridgeConn *c, const char *b64, uint32_t n) {
    int32_t m = b64_decode(b64, n, data_buf(c), BRIDGE_CHUNK);
    if (m <= 0 || (c->len >= 0 && c->got + (uint64_t)m > (uint64_t)c->len)) {
        set_detail(c, "the computer sent a damaged or oversized chunk");
        drain(c, true);
        fail(c);
        return false;
    }
    c->data_len = (uint32_t)m;
    c->data_off = 0;
    c->got += (uint64_t)m;
    c->ack_due = true;
    return true;
}

// From FGO to the first byte of body (or the end): FWAIT while the host works,
// FHEAD, the headers it passes on, then whatever comes next.
static bool await_head(BridgeConn *c) {
    bool have_head = false;
    for (;;) {
        uint32_t n = 0;
        LineResult r = read_line(c, BRIDGE_QUIET_MS, true, &n);
        if (r == L_STOP)  { stopped(c); return false; }
        if (r == L_QUIET) { went_quiet(c); return false; }
        const char *type, *rest;
        if (!split_line(c, line_buf(c), &type, &rest)) continue;

        if (!strcmp(type, "FWAIT")) continue;
        if (!strcmp(type, "FHEAD") && !have_head) {
            int status = (int)strtol(rest, nullptr, 10);
            if (status < 100 || status > 599) {
                set_detail(c, "the computer sent a bad status");
                drain(c, true);
                fail(c);
                return false;
            }
            c->len = -1;
            if (const char *l = strstr(rest, "len=")) c->len = strtoll(l + 4, nullptr, 10);
            if (c->len < -1) c->len = -1;
            c->head_len = c->head_off = 0;
            head_add(c, "HTTP/1.1 %d %s\r\n", status, status < 300 ? "OK" : "Bridged");
            have_head = true;
            continue;
        }
        if (!have_head) {
            if (!strcmp(type, "FDONE") && !strncmp(rest, "err", 3)) { host_failed(c, rest); return false; }
            if (!strcmp(type, "FDONE")) {
                set_detail(c, "the computer ended the fetch without an answer");
                c->ended = true;
                fail(c);
                return false;
            }
            continue;
        }
        if (!strcmp(type, "FHDR")) { head_header(c, rest, (uint32_t)strlen(rest)); continue; }
        if (!strcmp(type, "FDATA")) {
            head_finish(c);
            return take_data(c, rest, (uint32_t)strlen(rest));
        }
        if (!strcmp(type, "FDONE")) {
            if (!strncmp(rest, "err", 3)) { host_failed(c, rest); return false; }
            head_finish(c);
            c->ended = true;
            return true;
        }
    }
}

// --- the transport -------------------------------------------------------------

static int br_open(void *ctx, const char *host, uint16_t port, bool tls) {
    BridgeConn *c = (BridgeConn *)ctx;
    free(c->mem);
    c->mem = (uint8_t *)malloc(MEM_TOTAL);
    c->detail[0] = 0;
    if (!c->mem) { set_detail(c, "not enough memory for the bridge"); return -1; }
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    c->tls = tls;                // the host does the TLS, so this is only the scheme
    c->req_len = 0;
    c->phase = P_REQ;
    c->head_len = c->head_off = c->data_len = c->data_off = 0;
    c->got = 0;
    c->len = -1;
    c->ack_due = c->ended = c->failed = false;
    return 0;
}

static int br_send(void *ctx, const uint8_t *data, uint32_t len) {
    BridgeConn *c = (BridgeConn *)ctx;
    if (c->phase != P_REQ || !c->mem) return -1;
    if (len > BRIDGE_REQ_MAX - c->req_len) {
        set_detail(c, "the request is too big for the bridge (%u bytes at most)", (unsigned)BRIDGE_REQ_MAX);
        fail(c);
        return -1;
    }
    memcpy(c->mem + REQ_AT + c->req_len, data, len);
    c->req_len += len;
    return 0;
}

static int br_recv(void *ctx, uint8_t *buf, uint32_t cap) {
    BridgeConn *c = (BridgeConn *)ctx;
    if (!c->mem || c->failed) return -1;
    if (c->phase == P_REQ) {
        if (!issue(c)) { fail(c); return -1; }
        c->phase = P_RESP;
        if (!await_head(c)) return -1;
    }
    for (;;) {
        if (c->head_off < c->head_len) {
            uint32_t k = c->head_len - c->head_off;
            if (k > cap) k = cap;
            memcpy(buf, head_buf(c) + c->head_off, k);
            c->head_off += k;
            return (int)k;
        }
        if (c->data_off < c->data_len) {
            uint32_t k = c->data_len - c->data_off;
            if (k > cap) k = cap;
            memcpy(buf, data_buf(c) + c->data_off, k);
            c->data_off += k;
            return (int)k;
        }
        if (c->ended) return 0;            // the body is complete: a clean close
        // Everything handed out has been dealt with, since the caller has come
        // back for more: that is when the host may send the next chunk.
        if (c->ack_due) {
            frame(c, "FACK %lu %llu", (unsigned long)c->id, (unsigned long long)c->got);
            c->ack_due = false;
        }
        uint32_t n = 0;
        LineResult r = read_line(c, BRIDGE_QUIET_MS, true, &n);
        if (r == L_STOP)  { stopped(c); return -1; }
        if (r == L_QUIET) { went_quiet(c); return -1; }
        const char *type, *rest;
        if (!split_line(c, line_buf(c), &type, &rest)) continue;
        if (!strcmp(type, "FDATA")) {
            if (!take_data(c, rest, (uint32_t)strlen(rest))) return -1;
        } else if (!strcmp(type, "FDONE")) {
            if (!strncmp(rest, "err", 3)) { host_failed(c, rest); return -1; }
            c->ended = true;
        }
        // FWAIT (the server is slow) and anything else: keep reading.
    }
}

static void br_close(void *ctx) {
    BridgeConn *c = (BridgeConn *)ctx;
    if (c->mem && c->phase == P_RESP && !c->ended) {
        // The caller is done before the host is. Either it has every byte it
        // was told to expect (the host's FDONE follows the last FACK), or it
        // stopped early, a full filesystem or a body over its limit, and the
        // host has to be told.
        bool complete = c->len >= 0 && c->got >= (uint64_t)c->len;
        if (complete && c->ack_due) {
            frame(c, "FACK %lu %llu", (unsigned long)c->id, (unsigned long long)c->got);
            c->ack_due = false;
        }
        // Stopping early, FSTOP takes the place of the FACK the host waits for.
        drain(c, !complete);
    }
    free(c->mem);
    c->mem = nullptr;
    c->phase = P_IDLE;
}

void bridge_transport(BridgeConn *c, HttpTransport *t) {
    t->open = br_open;
    t->send = br_send;
    t->recv = br_recv;
    t->close = br_close;
    t->ctx = c;
}

// --- the offer -------------------------------------------------------------------

void bridge_offer_set(BridgeOffer *o, bool on, bool usb_connected) {
    o->on = on;
    o->usb = on && usb_connected;
}

void bridge_offer_dtr(BridgeOffer *o, bool dtr) {
    if (!dtr) o->on = false;
}

bool bridge_offer_valid(BridgeOffer *o, bool usb_connected) {
    if (o->on && o->usb && !usb_connected) o->on = false;
    return o->on;
}

bool bridge_may_use(BridgeOffer *o, bool usb_connected, bool console_shell) {
    return bridge_offer_valid(o, usb_connected) && console_shell;
}

// --- the input door ------------------------------------------------------------

enum DoorState : uint8_t { D_IDLE = 0, D_RS, D_SKIP };

int bridge_door(BridgeDoor *d, int c, bool *offer) {
    if (c < 0) return c;
    if (c == 0x03) { d->st = D_IDLE; return c; }         // Ctrl+C always gets through
    switch (d->st) {
    case D_IDLE:
        if (c == 0x1E) { d->st = D_RS; return -1; }
        return c;
    case D_RS:
        if (c == 0x1F) { d->st = D_IDLE; if (offer) *offer = true; return -1; }
        if (c == '\n' || c == '\r') { d->st = D_IDLE; return -1; }
        d->st = D_SKIP;
        d->n = 1;
        return -1;
    default:
        if (c == '\n' || c == '\r') { d->st = D_IDLE; return -1; }
        // Longer than any line a host sends: whatever this is, it is somebody
        // typing after a stray Ctrl+^, and it is theirs again.
        if (++d->n > 2 * BRIDGE_LINE_MAX) { d->st = D_IDLE; return c; }
        return -1;
    }
}
