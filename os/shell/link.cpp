// link — the console as a framed request/response channel for host tools.
//
// The protocol is LINK.md at the root of the repository; this file is the
// device's half of it, and the two are kept in step. In short: no echo, no line
// editor, no colour; one request per line from the host; every answer a frame
// beginning with 0x1E, ending with exactly one OK or ERR.
//
// Runs on the shell's own task, as the logged-in account, so every request has
// exactly the permissions the same thing typed at the prompt would have.
#include "command.h"
#include "out.h"
#include "interrupt.h"
#include "task.h"
#include "storage.h"
#include "loader.h"
#include "fsguard.h"
#include "session.h"
#include "kernel.h"
#include "path.h"
#include "sha256.h"
#include "b64.h"
#include "linkreq.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

const char *fs_cwd(void);
int shell_run_line_now(char *line);

// The most output one EXEC hands back. Tried at this size, then halved down to
// 2 KB if the heap cannot spare it, so a busy device still answers.
#define LINK_EXEC_MAX  32768
// A transfer that goes quiet this long is abandoned and its partial file removed.
#define LINK_XFER_MS   10000
// The USB host not asserting DTR for this long means the port was closed.
#define LINK_GONE_MS   3000

// Everything link mode needs that is bigger than a few words, in one block on
// the heap for as long as it runs. It sits on the shell's own stack otherwise,
// and an EXEC runs a whole shell line underneath it.
struct LinkState {
    char    req[LINK_LINE_MAX];               // the request being read
    char    cmd[LINK_LINE_MAX];               // EXEC's line, which the runner splits in place
    char    frame[LINK_LINE_MAX + 16];        // one frame being written
    uint8_t data[LINK_CHUNK];                 // one chunk of a file
};

static bool g_in_link;

// --- writing frames ---------------------------------------------------------

static void send_frame(LinkState *st, uint32_t n) {
    st->frame[n++] = '\n';
    out_console(st->frame, n);
    stdio_flush();
}

static void frame(LinkState *st, const char *fmt, ...) {
    st->frame[0] = 0x1E;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(st->frame + 1, sizeof(st->frame) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((uint32_t)n > sizeof(st->frame) - 3) n = (int)sizeof(st->frame) - 3;
    send_frame(st, 1 + (uint32_t)n);
}

// TYPE then the bytes in base64. `n` is at most LINK_CHUNK.
static void frame_data(LinkState *st, const char *type, const uint8_t *d, uint32_t n) {
    uint32_t t = (uint32_t)strlen(type);
    st->frame[0] = 0x1E;
    memcpy(st->frame + 1, type, t);
    st->frame[1 + t] = ' ';
    uint32_t e = b64_encode(d, n, st->frame + 2 + t);
    send_frame(st, 2 + t + e);
}

#define ERR(st, code, ...) do { frame(st, "ERR " code " " __VA_ARGS__); } while (0)

// --- reading requests -------------------------------------------------------

enum ReadResult { RL_OK, RL_TIMEOUT, RL_INTR, RL_LONG, RL_GONE };

// One line, without its newline. `timeout_ms` 0 waits for ever; otherwise it is
// the longest gap between bytes. `watch_usb` ends the wait when the host has
// closed the port.
static ReadResult read_line(char *buf, uint32_t cap, uint32_t timeout_ms,
                            bool watch_usb, uint32_t *len) {
    uint32_t n = 0, last = task_now_ms(), gone_since = 0;
    bool over = false;
    for (;;) {
        int c = intr_getkey();
        if (c < 0) {
            uint32_t now = task_now_ms();
            if (timeout_ms && now - last >= timeout_ms) return RL_TIMEOUT;
            if (watch_usb) {
                if (stdio_usb_connected()) gone_since = 0;
                else if (!gone_since) gone_since = now ? now : 1;
                else if (now - gone_since >= LINK_GONE_MS) return RL_GONE;
            }
            // A millisecond, not a yield: link mode mostly waits, and a yield
            // loop would hold a core at 100% doing it. Bytes already queued
            // are all read before this is reached again.
            task_sleep_ms(1);
            continue;
        }
        last = task_now_ms();
        if (c == 0x03) return RL_INTR;
        if (c == '\r') continue;
        if (c == '\n') {
            if (over) return RL_LONG;
            buf[n] = 0;
            *len = n;
            return RL_OK;
        }
        if (n + 1 < cap) buf[n++] = (char)c;
        else over = true;
    }
}

// --- paths ------------------------------------------------------------------

// Resolve, then ask the same question the shell's commands ask, without
// printing: in link mode a refusal is a frame, not a line on the console.
static bool link_path(LinkState *st, const char *in, char *out, uint32_t cap, bool change) {
    if (!in[0]) { ERR(st, "badreq", "a path is needed"); return false; }
    if (!path_resolve(fs_cwd(), in, out, cap)) { ERR(st, "toolong", "path too long"); return false; }
    if (change ? !fs_may_change(out) : !fs_may_read(out)) {
        ERR(st, "perm", "%s is protected for this account", out);
        return false;
    }
    return true;
}

// --- the requests -----------------------------------------------------------

// The cwd travels percent-encoded (space as %20, % as %25) so the HELLO frame
// stays one key=value per space-separated word.
static void hello(LinkState *st) {
    char cwd[200];
    uint32_t o = 0;
    for (const char *p = fs_cwd(); *p && o + 4 < sizeof(cwd); p++) {
        if (*p == ' ' || *p == '%') { snprintf(cwd + o, 4, "%%%02X", (unsigned)*p); o += 3; }
        else cwd[o++] = *p;
    }
    cwd[o] = 0;
    const char *u = session_user();
    frame(st, "HELLO proto=%d os=%s build=%s board=%s user=%s cwd=%s chunk=%d exec_max=%d",
          LINK_PROTO, RPC_OS_VERSION, RPC_OS_BUILD, PICO_BOARD, u ? u : "", cwd,
          LINK_CHUNK, LINK_EXEC_MAX);
}

static void do_exec(LinkState *st, const char *line) {
    if (!line[0]) { ERR(st, "badreq", "EXEC needs a command"); return; }
    uint32_t cap = LINK_EXEC_MAX;
    char *out = nullptr;
    while (cap >= 2048 && !(out = (char *)malloc(cap))) cap /= 2;
    if (!out) { ERR(st, "busy", "not enough memory to run it"); return; }

    snprintf(st->cmd, sizeof(st->cmd), "%s", line);
    // Nothing the host sent may be left over to be read as the command's input,
    // and a Ctrl+C from before must not stop this one.
    intr_clear();
    intr_stash_clear();
    if (!out_capture_begin_all(out, cap)) {
        free(out);
        ERR(st, "busy", "too many captures running at once");
        return;
    }
    int rc = shell_run_line_now(st->cmd);
    uint32_t n = out_capture_end();
    bool over = out_capture_overflowed();
    intr_clear();
    intr_stash_clear();

    for (uint32_t i = 0; i < n; i += LINK_CHUNK) {
        uint32_t m = n - i < LINK_CHUNK ? n - i : LINK_CHUNK;
        frame_data(st, "OUT", (const uint8_t *)out + i, m);
    }
    free(out);
    frame(st, "OK rc=%d truncated=%d", rc ? 1 : 0, over ? 1 : 0);
}

static void do_put(LinkState *st, const char *rest) {
    uint32_t size = 0;
    char want[65];
    const char *rp = nullptr;
    if (!link_parse_put(rest, &size, want, &rp)) {
        ERR(st, "badreq", "PUT <size> <sha256> <path>");
        return;
    }
    char path[200];
    if (!link_path(st, rp, path, sizeof(path), /*change*/true)) return;
    bool is_dir = false;
    if (storage_stat(path, &is_dir, nullptr) && is_dir) {
        ERR(st, "nofile", "%s is a directory", path);
        return;
    }
    if (!storage_would_fit(size)) {
        ERR(st, "nospace", "%lu bytes free", (unsigned long)storage_free_bytes());
        return;
    }
    void *sink = storage_open_sink(path);
    if (!sink) { ERR(st, "io", "could not open %s for writing", path); return; }

    Sha256Ctx sha;
    sha256_init(&sha);
    frame(st, "READY chunk=%d", LINK_CHUNK);

    uint32_t got = 0;
    const char *fail_code = nullptr, *fail_msg = nullptr;
    while (got < size) {
        uint32_t n = 0;
        ReadResult r = read_line(st->req, sizeof(st->req), LINK_XFER_MS, false, &n);
        if (r == RL_TIMEOUT) { fail_code = "timeout"; fail_msg = "nothing from the host for 10 s"; break; }
        if (r == RL_INTR || (r == RL_OK && !strcmp(st->req, "CANCEL"))) {
            fail_code = "cancelled"; fail_msg = "cancelled by the host"; break;
        }
        if (r != RL_OK || n < 3 || st->req[0] != 'D' || st->req[1] != ' ') {
            fail_code = "badreq"; fail_msg = "expected a D line"; break;
        }
        int32_t m = b64_decode(st->req + 2, n - 2, st->data, sizeof(st->data));
        if (m <= 0 || got + (uint32_t)m > size) {
            fail_code = "badreq"; fail_msg = "bad or oversized D line"; break;
        }
        if (!storage_sink_write(sink, st->data, (uint32_t)m)) {
            fail_code = "io"; fail_msg = "the filesystem refused the write"; break;
        }
        sha256_update(&sha, st->data, (size_t)m);
        got += (uint32_t)m;
        frame(st, "ACK %lu", (unsigned long)got);
    }
    bool closed = storage_close_sink(sink);
    if (fail_code) {
        storage_remove(path);
        frame(st, "ERR %s %s", fail_code, fail_msg);
        return;
    }
    uint8_t dg[SHA256_DIGEST_LEN];
    char hex[65];
    sha256_final(&sha, dg);
    hex_encode(dg, sizeof(dg), hex);
    if (!closed) { storage_remove(path); ERR(st, "io", "the last write did not complete"); return; }
    if (strcmp(hex, want) != 0) {
        storage_remove(path);
        ERR(st, "digest", "received %s; nothing was kept", hex);
        return;
    }
    frame(st, "OK sha=%s", hex);
}

static void do_get(LinkState *st, const char *rest) {
    char path[200];
    if (!link_path(st, rest, path, sizeof(path), /*change*/false)) return;
    bool is_dir = false;
    if (!storage_stat(path, &is_dir, nullptr)) { ERR(st, "nofile", "there is no %s", path); return; }
    if (is_dir) { ERR(st, "nofile", "%s is a directory", path); return; }
    AppSource src;
    void *h = nullptr;
    if (!storage_open_source(path, &src, &h)) { ERR(st, "io", "could not open %s", path); return; }

    frame(st, "SIZE %lu", (unsigned long)src.size);
    Sha256Ctx sha;
    sha256_init(&sha);
    const char *fail_code = nullptr, *fail_msg = nullptr;
    for (uint32_t off = 0; off < src.size; ) {
        uint32_t m = src.size - off < LINK_CHUNK ? src.size - off : LINK_CHUNK;
        int r = src.read(src.ctx, off, st->data, m);
        if (r != (int)m) { fail_code = "io"; fail_msg = "the filesystem refused a read"; break; }
        sha256_update(&sha, st->data, m);
        frame_data(st, "D", st->data, m);
        off += m;
        uint32_t n = 0;
        ReadResult rr = read_line(st->req, sizeof(st->req), LINK_XFER_MS, false, &n);
        if (rr == RL_TIMEOUT) { fail_code = "timeout"; fail_msg = "nothing from the host for 10 s"; break; }
        if (rr == RL_INTR || (rr == RL_OK && !strcmp(st->req, "CANCEL"))) {
            fail_code = "cancelled"; fail_msg = "cancelled by the host"; break;
        }
        if (rr != RL_OK || st->req[0] != 'A') { fail_code = "badreq"; fail_msg = "expected A"; break; }
    }
    storage_close_source(h);
    if (fail_code) { frame(st, "ERR %s %s", fail_code, fail_msg); return; }
    uint8_t dg[SHA256_DIGEST_LEN];
    char hex[65];
    sha256_final(&sha, dg);
    hex_encode(dg, sizeof(dg), hex);
    frame(st, "OK sha=%s", hex);
}

struct LsCtx { LinkState *st; uint32_t n; };

static void do_ls(LinkState *st, const char *rest) {
    char path[200];
    if (!link_path(st, rest[0] ? rest : ".", path, sizeof(path), /*change*/false)) return;
    bool is_dir = false;
    if (!storage_stat(path, &is_dir, nullptr)) { ERR(st, "nofile", "there is no %s", path); return; }
    if (!is_dir) { ERR(st, "nofile", "%s is not a directory", path); return; }
    LsCtx c{st, 0};
    bool ok = storage_walk(path, [](void *ctx, const char *name, bool d, uint32_t size) {
        LsCtx *x = (LsCtx *)ctx;
        frame(x->st, "E %c %lu %s", d ? 'd' : 'f', (unsigned long)(d ? 0 : size), name);
        x->n++;
    }, &c);
    if (!ok) { ERR(st, "io", "could not read %s", path); return; }
    frame(st, "OK n=%lu", (unsigned long)c.n);
}

static void do_stat(LinkState *st, const char *rest) {
    char path[200];
    if (!link_path(st, rest, path, sizeof(path), /*change*/false)) return;
    bool is_dir = false;
    uint32_t size = 0;
    if (!storage_stat(path, &is_dir, &size)) { ERR(st, "nofile", "there is no %s", path); return; }
    frame(st, "OK type=%c size=%lu", is_dir ? 'd' : 'f', (unsigned long)(is_dir ? 0 : size));
}

// --- the command ------------------------------------------------------------

static int cmd_link(int, char **) {
    // On the console only: the requests arrive there, and only the shell's task
    // reads keys.
    if (task_shell_pid() && task_self() != task_shell_pid()) {
        out_err("'link' talks to a host tool over the console, so it runs only at the prompt.");
        return 1;
    }
    if (g_in_link) { out_err("Link mode is already running."); return 1; }

    LinkState *st = (LinkState *)malloc(sizeof(LinkState));
    if (!st) { out_err("Not enough memory for link mode."); return 1; }
    InputClaim console;
    if (!console.ok) {
        free(st);
        out_err("Another transfer has the console.");
        return 1;
    }

    out_multi("Link mode, for host tools (LINK.md). Ctrl+C returns to the prompt.");
    out_flush();
    g_in_link = true;
    // Watch for the port closing only if it was open to begin with: on a UART
    // console (the emulator) there is no DTR to watch.
    bool watch = stdio_usb_connected();
    intr_clear();
    intr_stash_clear();
    hello(st);

    for (;;) {
        uint32_t n = 0;
        ReadResult r = read_line(st->req, sizeof(st->req), 0, watch, &n);
        if (r == RL_INTR || r == RL_GONE) break;
        if (r == RL_LONG) { ERR(st, "badreq", "line too long"); continue; }
        if (r != RL_OK || n == 0) continue;
        LinkReq q;
        if (!link_parse(st->req, &q)) { ERR(st, "badreq", "no verb"); continue; }
        if      (!strcmp(q.verb, "PING"))  frame(st, "OK");
        else if (!strcmp(q.verb, "HELLO")) { hello(st); frame(st, "OK"); }
        else if (!strcmp(q.verb, "EXEC"))  do_exec(st, q.rest);
        else if (!strcmp(q.verb, "PUT"))   do_put(st, q.rest);
        else if (!strcmp(q.verb, "GET"))   do_get(st, q.rest);
        else if (!strcmp(q.verb, "LS"))    do_ls(st, q.rest);
        else if (!strcmp(q.verb, "STAT"))  do_stat(st, q.rest);
        else if (!strcmp(q.verb, "BYE"))   { frame(st, "OK"); break; }
        else ERR(st, "badreq", "unknown verb %s", q.verb);
    }

    g_in_link = false;
    intr_clear();
    intr_stash_clear();
    free(st);
    out_multi("Left link mode.");
    return 0;
}

void link_register(void) {
    static const Command c{"link", "link  -- framed mode for host tools (LINK.md)", cmd_link, nullptr};
    cmd_register(&c);
}
