// The SSH server on the device: `ssh server on`, then `ssh root@<device>`.
//
// The protocol is core/sshserver.cpp and is tested on the host against a real
// OpenSSH client. This file is what only a board has: the socket, the tasks,
// the accounts and the files. DEVICE-UNCONFIRMED as a whole — it has been
// built for a Pico 2 W but not yet run on one; the host interop harness
// (os/host/ssh_host_server.cpp) is shaped the same way and is what is proven.
//
// --- the shape --------------------------------------------------------------
//
// ONE network task, `sshd`, owns the listener and every connection: it reads
// the sockets, runs the protocol (all the cryptography happens here), and
// writes. Each logged-in session has a SECOND task, its shell, which runs the
// line editor and every command typed — on the same 8 KB stack the console's
// shell has, since it runs the same lines. The two meet only in the session's
// SshSess: two rings and some flags (core/sshsess.h).
//
// Why two tasks rather than one per session doing both. A command that prints
// while holding the filesystem lock (ls inside its directory walk) would, if
// its own task had to push the bytes onto the network, take the network lock
// with the filesystem lock held — and a download takes them the other way
// round (net for the whole transfer, then the filesystem to write each piece).
// That is a deadlock, and with yielding locks it is a silent one: the
// watchdog is still fed. With the network on another task the shell task never
// takes the network lock at all; when it cannot hand its output over it waits
// for the ring, and while it holds a lock it does not wait: see shell_sink.
//
// The sshd task is pinned to core 0, the radio's core, so that its many small
// socket calls do not each migrate it there and back (netown.h). The cost is
// that a login's key exchange (two or three elliptic-curve multiplications,
// DEVICE-UNCONFIRMED at roughly 100-300 ms each on an M33 at 150 MHz) runs on
// the console's core and the console pauses for it.
//
// --- what it will not do -------------------------------------------------------
//
// Off unless turned on. Two sessions at most. No forwarding of any kind (the
// core refuses it). An account with no password (guest, as shipped) cannot log
// in by any method. Logins are limited: six attempts per connection, a pause
// after each wrong password, a minute to finish logging in, and a cool-down
// for everybody after many failures in a row. Every refusal and every login is
// in the log (`logdump`).
#include "command.h"
#include "out.h"
#include "registry.h"
#include "persist.h"
#include "storage.h"
#include "users.h"
#include "session.h"
#include "shell.h"
#include "task.h"
#include "interrupt.h"
#include "logring.h"
#include "lock.h"
#include "nettcp.h"
#include "lineedit.h"
#include "kernel.h"

#include "sshserver.h"
#include "sshsess.h"
#include "sshkeys.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool net_is_connected(void);

#define SSHD_SESSIONS        2
#define SSHD_ORPHANS         4
#define SSHD_DEFAULT_PORT    22
#define SSHD_LOGIN_GRACE_MS  60000    // to finish logging in
#define SSHD_CLOSE_GRACE_MS  10000    // for a shell to stop after the connection goes
#define SSHD_FAIL_WINDOW_MS  60000
#define SSHD_FAIL_LIMIT      10       // refusals, from anywhere, within the window...
#define SSHD_COOLDOWN_MS     60000    // ...and then nobody gets in for this long
#define SSHD_SPILL_MAX       8192
#define HOSTKEY_PATH         "/os/ssh/hostkey"

struct SshdSession {
    int       sock;
    SshServer srv;
    SshSess   sess;
    TaskTerm  term;
    int       shell_pid;
    uint16_t  dom;
    uint32_t  t_open, t_hold, t_closing, t_exit;
    uint32_t  regranted;
    bool      shell_started, exit_sent, closing, srv_freed;
    bool      sock_dead;           // a read or write failed: the peer is gone
    bool      sock_released;       // net_pkg_tcp_close has been called, once
    volatile bool logout;          // `logout` typed in the session
    char      peer[24];
    char      user[TASK_USER_MAX];
    char      exec[256];
    bool      exec_mode;

    // Shell side only.
    char      fmt[192];            // where the output layer formats a line
    uint8_t  *spill;
    uint32_t  spill_len;
    bool      spill_noted;
    bool      line_ended;
};

static SshdSession *g_slot[SSHD_SESSIONS];
static SshdSession *g_orphan[SSHD_ORPHANS];
// Held by the sshd task while it puts a session into the table or takes one
// out (and frees it), and by `ssh server status` while it reads the table. The
// sshd task's own reads need nothing: it is the only writer.
static RpcLock       g_slots_lock;
static volatile bool g_run;
static volatile int  g_pid;
static SshHostKey    g_hostkey;
static uint16_t      g_port;
static uint32_t      g_fail_at[SSHD_FAIL_LIMIT];
static uint32_t      g_fail_n;
static uint32_t      g_cool_until;
static bool          g_listen_noted;

static const char *peer_of(const SshdSession *s) { return s->peer[0] ? s->peer : "?"; }

// --- the host key ----------------------------------------------------------------

// Made on first start from the hardware random source and kept as its 32-byte
// secret in /os/ssh, which protect.cpp closes to everyone but an admin. Never
// printed: `ssh server fingerprint` shows the SHA256 fingerprint, which is
// what a client is asked to compare.
static bool hostkey_ready(bool make) {
    if (g_hostkey.ok) return true;
    uint8_t d[32];
    if (storage_read_file(HOSTKEY_PATH, d, sizeof(d)) == sizeof(d) && ssh_hostkey_load(&g_hostkey, d)) {
        ssh_wipe(d, sizeof(d));
        return true;
    }
    ssh_wipe(d, sizeof(d));
    if (!make) return false;
    if (!ssh_hostkey_generate(&g_hostkey)) return false;
    storage_mkdir("/os/ssh");
    if (!storage_write_file(HOSTKEY_PATH, g_hostkey.d, sizeof(g_hostkey.d))) {
        ssh_wipe(&g_hostkey, sizeof(g_hostkey));
        return false;
    }
    log_add(LOG_K_INFO, "ssh: made a new host key");
    return true;
}

// The fingerprint whether or not the server is running. With it off, the key
// is read, used and wiped again, so the secret does not sit in RAM because
// somebody asked what the fingerprint was.
static bool fingerprint_now(char *out, unsigned cap) {
    if (g_hostkey.ok) {
        uint8_t blob[160];
        uint32_t n = ssh_hostkey_blob(&g_hostkey, blob, sizeof(blob));
        ssh_fingerprint(blob, n, out, cap);
        return true;
    }
    if (!hostkey_ready(false)) return false;
    uint8_t blob[160];
    uint32_t n = ssh_hostkey_blob(&g_hostkey, blob, sizeof(blob));
    ssh_fingerprint(blob, n, out, cap);
    if (!g_pid) ssh_wipe(&g_hostkey, sizeof(g_hostkey));
    return true;
}

static void hostkey_fingerprint(char *out, unsigned cap) {
    uint8_t blob[160];
    uint32_t n = ssh_hostkey_blob(&g_hostkey, blob, sizeof(blob));
    ssh_fingerprint(blob, n, out, cap);
}

// --- the accounts -------------------------------------------------------------------

// Who may log in at all. An account that signs in without a password is
// refused outright, by every method: guest is password-less as shipped, and
// a device that answered `ssh guest@` with a shell would be open to anyone on
// the network. `passwd guest` gives it one, after which it is an account like
// any other.
static bool op_user_ok(void *, const char *user) {
    return users_exists(user) && !users_is_nopass(user);
}

static bool op_password(void *, const char *user, const char *pw) {
    if (!users_exists(user) || users_is_nopass(user)) return false;   // again, on purpose
    return users_verify(user, pw);
}

// /home/<user>/.ssh/authorized_keys, read fresh each time so a key added or
// removed takes effect at the next login. The name has already been checked
// to contain no '/' (sshserver.cpp), so the path cannot leave /home.
static bool op_pubkey(void *, const char *user, const uint8_t *blob, uint32_t n) {
    char path[TASK_USER_MAX + 32];
    snprintf(path, sizeof(path), "/home/%s/.ssh/authorized_keys", user);
    const uint32_t cap = 8192;
    char *text = (char *)malloc(cap);
    if (!text) return false;
    uint32_t len = storage_read_file(path, (uint8_t *)text, cap);
    bool ok = len && ssh_authkeys_match(text, len, blob, n);
    free(text);
    return ok;
}

static void note_failure(uint32_t now) {
    g_fail_at[g_fail_n % SSHD_FAIL_LIMIT] = now;
    g_fail_n++;
    // Ten refusals inside a minute, from any mix of connections: somebody is
    // guessing. Everyone waits a minute, which costs a guesser far more than
    // it costs an owner who mistyped.
    if (g_fail_n >= SSHD_FAIL_LIMIT) {
        uint32_t oldest = g_fail_at[g_fail_n % SSHD_FAIL_LIMIT];
        if ((uint32_t)(now - oldest) < SSHD_FAIL_WINDOW_MS) {
            g_cool_until = now + SSHD_COOLDOWN_MS;
            log_add(LOG_K_WARN, "ssh: many failed logins - refusing new connections for a minute");
        }
    }
}

static void clean_name(const char *in, char *out, unsigned cap) {
    unsigned i = 0;
    for (; in[i] && i + 1 < cap; i++) out[i] = (in[i] > ' ' && in[i] < 127) ? in[i] : '?';
    out[i] = 0;
}

static SshdSession *of_ctx(void *ctx) { return (SshdSession *)ctx; }

static void op_auth_log(void *ctx, const char *user, const char *method, bool ok) {
    SshdSession *s = of_ctx(ctx);
    char u[TASK_USER_MAX];
    clean_name(user, u, sizeof(u));
    if (ok) log_addf(LOG_K_INFO, "ssh: %s logged in (%s) from %s", u, method, peer_of(s));
    else    log_addf(LOG_K_WARN, "ssh: refused %s for '%s' from %s", method, u, peer_of(s));
}

// --- the shell side ------------------------------------------------------------------
//
// Everything below up to "the network side" runs on the SESSION'S task.

// Move what was set aside into the ring, in order, as room allows.
static void spill_drain(SshdSession *s) {
    if (!s->spill_len) return;
    uint32_t w = ssh_sess_write(&s->sess, (const char *)s->spill, s->spill_len);
    if (w) { memmove(s->spill, s->spill + w, s->spill_len - w); s->spill_len -= w; }
    // Emptied: a later gap gets a note of its own.
    if (!s->spill_len) s->spill_noted = false;
}

static void spill_add(SshdSession *s, const char *d, uint32_t n) {
    static const char note[] = "\r\n[ssh: output lost]\r\n";
    const uint32_t note_len = sizeof(note) - 1;
    if (!s->spill) s->spill = (uint8_t *)malloc(SSHD_SPILL_MAX);
    if (!s->spill) { s->sess.lost += n; return; }
    // Room for the note is kept back, so a full spill can still say it lost
    // something — a cut with no mark reads as the end of the output.
    uint32_t room = SSHD_SPILL_MAX - note_len - s->spill_len;
    if (s->spill_len > SSHD_SPILL_MAX - note_len) room = 0;
    uint32_t take = s->spill_noted ? 0 : (n < room ? n : room);
    if (take) { memcpy(s->spill + s->spill_len, d, take); s->spill_len += take; }
    if (take < n) {
        s->sess.lost += n - take;
        if (!s->spill_noted) {
            memcpy(s->spill + s->spill_len, note, note_len);
            s->spill_len += note_len;
            s->spill_noted = true;
        }
    }
}

// Where everything the session prints goes (out_capture_begin_term).
//
// The ring is 2 KB and the network side empties it as fast as the client's
// window allows. When it is full, this waits — unless the task is holding a
// lock. A command inside a filesystem walk holds the filesystem lock, and
// waiting here would hold it hostage to whatever the remote client is doing;
// worse, the network side could be waiting for the network lock held by a
// download that is itself waiting for the filesystem. So while a lock is held
// the output is set aside (up to 8 KB, then dropped with a note saying so) and
// passed on once the lock has gone.
static void shell_sink(void *ctx, const char *d, uint32_t n) {
    SshdSession *s = of_ctx(ctx);
    while (n) {
        if (s->sess.hangup) { s->sess.lost += n; return; }
        spill_drain(s);
        if (!s->spill_len) {
            uint32_t w = ssh_sess_write(&s->sess, d, n);
            d += w; n -= w;
            if (!n) return;
        }
        if (task_crit_active()) { spill_add(s, d, n); return; }
        // Interrupted, or being stopped: what is left is not wanted.
        if (task_should_stop() || intr_pending()) { s->sess.lost += n; return; }
        task_sleep_ms(2);
    }
}

static int term_getkey(void *ctx) { return ssh_sess_getch(&of_ctx(ctx)->sess); }
static void term_hangup(void *ctx) { of_ctx(ctx)->logout = true; }
static bool term_size(void *ctx, uint16_t *cols, uint16_t *rows) {
    SshdSession *s = of_ctx(ctx);
    *cols = s->sess.cols; *rows = s->sess.rows;
    return s->sess.pty;
}

// The line editor's terminal.
static int le_getch(void *ctx, uint32_t) {
    SshdSession *s = of_ctx(ctx);
    spill_drain(s);
    int c = ssh_sess_getch(&s->sess);
    if (c >= 0) return c;
    // The connection has gone, the session was told to end, or the client
    // has sent everything it ever will (ssh with input piped in): end the line
    // so the loop can see it.
    if (s->sess.hangup || s->logout || task_should_stop() ||
        (s->sess.eof && ssh_ring_used(&s->sess.in) == 0)) {
        s->line_ended = true;
        return '\n';
    }
    task_sleep_ms(5);
    return LE_NO_KEY;
}
static void le_putch(void *, char c) { out_write(&c, 1); }

static int sshd_shell(void *arg) {
    SshdSession *s = of_ctx(arg);
    // FIRST, before anything can run: this task is the remote user's, not the
    // console's (see TaskShell in task.h). Nothing has run on it before now.
    session_attach(s->user, &s->term, s->dom);
    int rc = 1;
    if (out_capture_begin_term(s->fmt, sizeof(s->fmt), shell_sink, s)) {
        if (s->exec_mode) {
            intr_clear();
            s->sess.running = true;
            rc = shell_run_input(s->exec);
            s->sess.running = false;
        } else {
            out_multi("RPCortex %s on %s. Signed in as %s.", RPC_OS_VERSION,
                      reg_get("System.Device_ID", "vela"), s->user);
            rc = 0;
            for (;;) {
                char prompt[96];
                shell_prompt_text(prompt, sizeof(prompt));
                LineEdit le{};
                le.io.getch = le_getch;
                le.io.putch = le_putch;
                le.io.ctx = s;
                le.complete = shell_complete;
                le.prompt = prompt;
                char line[128];
                s->line_ended = false;
                line_edit(&le, line, sizeof(line));
                if (s->sess.hangup || s->logout) break;
                if (line[0]) {
                    intr_clear();
                    s->sess.running = true;
                    rc = shell_run_input(line);
                    s->sess.running = false;
                }
                // A stop request aimed at the last command (the watchdog's
                // first warning, or `kill`) must not end the session that
                // ran it — the console's shell does the same at its prompt.
                if (!s->sess.hangup) task_clear_stop();
                if (s->logout || s->line_ended) break;
            }
        }
        while (s->spill_len && !s->sess.hangup) { spill_drain(s); if (s->spill_len) task_sleep_ms(5); }
        out_capture_end();
    }
    free(s->spill);
    s->spill = nullptr;
    s->spill_len = 0;
    s->sess.exit_status = rc;
    __atomic_store_n(&s->sess.done, true, __ATOMIC_RELEASE);
    return rc;
}

// --- the network side ------------------------------------------------------------------
//
// Everything below runs on the sshd task.

static void op_start(void *ctx) {
    SshdSession *s = of_ctx(ctx);
    snprintf(s->user, sizeof(s->user), "%s", s->srv.user);
    s->exec_mode = s->srv.mode == SSH_MODE_EXEC;
    snprintf(s->exec, sizeof(s->exec), "%s", s->srv.exec);
    ssh_sess_init(&s->sess, s->srv.pty);
    s->sess.cols = (uint16_t)(s->srv.cols ? s->srv.cols : 80);
    s->sess.rows = (uint16_t)(s->srv.rows ? s->srv.rows : 24);
    s->term.getkey = term_getkey;
    s->term.hangup = term_hangup;
    s->term.size = term_size;
    s->term.ctx = s;
    s->dom = intr_domain_new();
    char name[TASK_NAME_MAX];
    snprintf(name, sizeof(name), "ssh-%.11s", s->user);
    int pid = s->dom ? task_spawn(name, "(ssh)", sshd_shell, s, TASK_STACK_SHELL, AFFINITY_ANY) : -1;
    s->shell_started = true;
    if (pid < 0) {
        // No interrupt domain or no memory for the stack. Say so on the
        // channel — the ring has no producer, so this side may write it — and
        // end the session with a failure status.
        static const char msg[] = "The device cannot start another session right now.\r\n";
        ssh_ring_put(&s->sess.out, (const uint8_t *)msg, sizeof(msg) - 1);
        intr_domain_free(s->dom);
        s->dom = 0;
        s->sess.exit_status = 1;
        s->sess.done = true;
        log_addf(LOG_K_WARN, "ssh: could not start a shell for %s", s->user);
        return;
    }
    s->shell_pid = pid;
}

static void op_data(void *ctx, const uint8_t *d, uint32_t n) {
    SshdSession *s = of_ctx(ctx);
    if (!s->shell_started || s->exec_mode || s->sess.done) { ssh_srv_consumed(&s->srv, n); return; }
    uint32_t dropped = ssh_sess_input(&s->sess, d, n);
    if (dropped) ssh_srv_consumed(&s->srv, dropped);
    // A Ctrl+C while a command runs: that session's interrupt, and only its.
    if (s->sess.intr) { s->sess.intr = false; intr_raise_in(s->dom); }
}

static void op_signal(void *ctx, const char *name) {
    SshdSession *s = of_ctx(ctx);
    if (!strcmp(name, "INT") && s->dom) intr_raise_in(s->dom);
}
static void op_winch(void *ctx) {
    SshdSession *s = of_ctx(ctx);
    s->sess.cols = (uint16_t)s->srv.cols;
    s->sess.rows = (uint16_t)s->srv.rows;
}
static void op_eof(void *ctx)   { of_ctx(ctx)->sess.eof = true; }
static void op_close(void *ctx) { of_ctx(ctx)->sess.hangup = true; }

static const SshServerOps kOps = {
    op_user_ok, op_password, op_pubkey, op_auth_log,
    op_start, op_data, op_signal, op_winch, op_eof, op_close,
};

static bool shell_gone(SshdSession *s) {
    if (!s->shell_pid) return true;
    const TaskInfo *t = task_find(s->shell_pid);
    return !t || t->state == TASK_DONE;
}

static void start_close(SshdSession *s, uint32_t now, const char *why) {
    if (s->closing) return;
    s->closing = true;
    s->t_closing = now;
    s->sess.hangup = true;
    // Whatever the session is running stops at its next yield point.
    if (s->dom) intr_raise_in(s->dom);
    if (s->shell_pid && !shell_gone(s)) task_kill(s->shell_pid);
    if (s->srv.authed)
        log_addf(LOG_K_INFO, "ssh: %s from %s: %s", s->user, peer_of(s), why && why[0] ? why : "closed");
}

// A failed write marks the socket DEAD; it does not release it. Releasing is
// one call in one place (pump's closing branch), and folding the two into one
// flag is how a connection that dropped part way left its slot in nettcp's
// table for good — six of those and nobody could connect.
static void send_pending(SshdSession *s) {
    uint32_t n;
    const uint8_t *p = ssh_out(&s->srv.c, &n);
    while (n && !s->sock_dead) {
        int w = net_tcp_write_now(s->sock, p, n);
        if (w <= 0) { if (w < 0) s->sock_dead = true; return; }
        ssh_out_consume(&s->srv.c, (uint32_t)w);
        p = ssh_out(&s->srv.c, &n);
    }
}

static void release_sock(SshdSession *s) {
    if (s->sock_released) return;
    net_pkg_tcp_close(s->sock);
    s->sock_released = true;
}

static void session_free(SshdSession *s) {
    release_sock(s);
    if (!s->srv_freed) { ssh_srv_free(&s->srv); s->srv_freed = true; }
    if (s->shell_pid) {
        out_capture_reclaim(s->shell_pid);    // a shell ended by the watchdog left its frame
        task_reap(s->shell_pid);
    }
    if (s->dom) intr_domain_free(s->dom);
    ssh_wipe(s, sizeof(*s));
    free(s);
}

// One turn for one connection. True when something happened, so the loop
// does not sleep while there is work.
static bool pump(SshdSession *s, uint32_t now) {
    if (s->closing) {
        // Out with whatever is left (a DISCONNECT, the last of the output),
        // then the socket, then — once the shell has stopped — the memory.
        if (!s->sock_released) {
            if (!s->srv_freed && !s->sock_dead) send_pending(s);
            uint32_t left = 0;
            if (!s->srv_freed) ssh_out(&s->srv.c, &left);
            if (s->sock_dead || (!left && net_tcp_drained(s->sock)) ||
                (uint32_t)(now - s->t_closing) > 2000)
                release_sock(s);
            return true;
        }
        if (!s->srv_freed) { ssh_srv_free(&s->srv); s->srv_freed = true; }
        return false;
    }

    bool busy = false;
    if (!s->srv.authed && (uint32_t)(now - s->t_open) > SSHD_LOGIN_GRACE_MS)
        ssh_disconnect(&s->srv.c, SSH_DISC_BY_APPLICATION, "login took too long");
    // Logged in and never asked for a shell or a command: a slot held for
    // nothing. Two of them would lock everyone else out.
    if (s->srv.authed && !s->shell_started && (uint32_t)(now - s->t_open) > 2 * SSHD_LOGIN_GRACE_MS)
        ssh_disconnect(&s->srv.c, SSH_DISC_BY_APPLICATION, "no session requested");

    // A refused attempt holds the connection still: no answer, no reading.
    bool held = s->t_hold && (int32_t)(now - s->t_hold) < 0;
    if (!held) {
        s->t_hold = 0;
        for (int i = 0; i < 4; i++) {
            uint32_t room = s->srv.c.in_cap - s->srv.c.in_len;
            if (!room || ssh_closed(&s->srv.c)) break;
            uint8_t buf[512];
            int n = net_tcp_read_now(s->sock, buf, room < sizeof(buf) ? room : sizeof(buf));
            // The peer has gone; what it sent before it went (often a
            // DISCONNECT) is still handled below, so the log says why.
            if (n < 0) { s->sock_dead = true; break; }
            if (n == 0) break;
            ssh_feed(&s->srv.c, buf, (uint32_t)n);
            busy = true;
        }
        ssh_process(&s->srv.c);
        if (s->srv.penalty_ms) {
            s->t_hold = now + s->srv.penalty_ms;
            s->srv.penalty_ms = 0;
            note_failure(now);
        }
        // The name, the moment there is one. OpenSSH waits for the success
        // before it opens a channel, so the session is a turn away from having
        // a shell; everything that names the login in between (the removed-
        // account check below, the log line when it closes) has to see it.
        if (s->srv.authed && !s->user[0]) snprintf(s->user, sizeof(s->user), "%s", s->srv.user);
    }

    // The shell's output, onto the channel as the window allows.
    if (s->shell_started) {
        const uint8_t *p;
        uint32_t n;
        while ((n = ssh_ring_peek(&s->sess.out, &p)) != 0) {
            uint32_t sent = ssh_srv_send(&s->srv, p, n);
            ssh_ring_drop(&s->sess.out, sent);
            if (sent) busy = true;
            if (sent < n) break;
        }
        uint32_t used = __atomic_load_n(&s->sess.consumed, __ATOMIC_ACQUIRE);
        if (used != s->regranted) { ssh_srv_consumed(&s->srv, used - s->regranted); s->regranted = used; }
        bool finished = __atomic_load_n(&s->sess.done, __ATOMIC_ACQUIRE) || shell_gone(s);
        if (finished && !s->exit_sent && ssh_ring_used(&s->sess.out) == 0) {
            // A shell the watchdog ended never said how it finished.
            if (!s->sess.done) s->sess.exit_status = 255;
            s->exit_sent = ssh_srv_exit(&s->srv, (uint32_t)s->sess.exit_status);
            if (s->exit_sent) s->t_exit = now;
        }
        if (s->logout && !s->sess.done) busy = true;    // keep turning until it notices
    }

    // An account removed while it was logged in loses the session.
    if (s->srv.authed && !users_exists(s->user))
        ssh_disconnect(&s->srv.c, SSH_DISC_BY_APPLICATION, "account removed");

    if (!held) send_pending(s);

    bool done_ok = s->exit_sent && (s->srv.close_in || (uint32_t)(now - s->t_exit) > 2000);
    if (s->sock_dead || ssh_closed(&s->srv.c) || done_ok) {
        const char *why = s->exit_sent ? "logged out"
                        : ssh_closed(&s->srv.c) && s->srv.c.why[0] ? s->srv.c.why
                        : "connection lost";
        start_close(s, now, why);
    }
    return busy;
}

static void accept_one(int h, uint32_t now) {
    int slot = -1;
    for (int i = 0; i < SSHD_SESSIONS; i++) if (!g_slot[i]) { slot = i; break; }
    char ip[24];
    net_tcp_peer(h, ip, sizeof(ip));
    if (slot < 0 || (int32_t)(g_cool_until - now) > 0) {
        // Closing at once is the only answer before a version line: SSH has
        // no way to say "busy" until the transport is up.
        net_pkg_tcp_close(h);
        log_addf(LOG_K_WARN, "ssh: turned away %s (%s)", ip[0] ? ip : "?",
                 slot < 0 ? "two sessions already" : "cooling down after failed logins");
        return;
    }
    SshdSession *s = (SshdSession *)calloc(1, sizeof(SshdSession));
    if (!s || !ssh_srv_init(&s->srv, &g_hostkey, &kOps, s)) {
        free(s);
        net_pkg_tcp_close(h);
        log_add(LOG_K_WARN, "ssh: not enough memory for another connection");
        return;
    }
    s->sock = h;
    s->t_open = now;
    snprintf(s->peer, sizeof(s->peer), "%s", ip);
    net_tcp_adopt(h);
    LockGuard g(&g_slots_lock);
    g_slot[slot] = s;
}

static void park_orphan(SshdSession *s) {
    for (int i = 0; i < SSHD_ORPHANS; i++) if (!g_orphan[i]) { g_orphan[i] = s; return; }
    // Nowhere to park it. Kept in its slot instead, which keeps that slot
    // busy; a leak this size beats freeing memory a running task points at.
}

// Finished sessions: freed once their shell has stopped; a shell that will not
// stop is left with its memory (it still points into it) and the slot reused.
static void reap(uint32_t now) {
    LockGuard g(&g_slots_lock);              // `ssh server status` reads the table
    for (int i = 0; i < SSHD_SESSIONS; i++) {
        SshdSession *s = g_slot[i];
        if (!s || !s->closing || !s->sock_released) continue;
        if (shell_gone(s)) { g_slot[i] = nullptr; session_free(s); continue; }
        if ((uint32_t)(now - s->t_closing) > SSHD_CLOSE_GRACE_MS) {
            bool parked = false;
            for (int k = 0; k < SSHD_ORPHANS; k++) if (!g_orphan[k]) { parked = true; break; }
            if (!parked) continue;
            log_addf(LOG_K_WARN, "ssh: %s's shell (pid %d) would not stop; keeping its memory",
                     s->user, s->shell_pid);
            if (!s->srv_freed) { ssh_srv_free(&s->srv); s->srv_freed = true; }
            park_orphan(s);
            g_slot[i] = nullptr;
        }
    }
    for (int k = 0; k < SSHD_ORPHANS; k++)
        if (g_orphan[k] && shell_gone(g_orphan[k])) { session_free(g_orphan[k]); g_orphan[k] = nullptr; }
}

static int sshd_main(void *) {
    // This task serves everybody and runs no commands: it follows the console
    // and has no terminal, whoever typed `ssh server on`.
    TaskShell *ts = task_shell();
    ts->own_user = false;
    ts->term = nullptr;
    ts->intr_dom = 0;

    int lsn = -1;
    uint32_t last_try = 0;
    bool first = true;
    while (g_run) {
        uint32_t now = task_now_ms();
        if (lsn < 0) {
            if (net_is_connected() && (first || (uint32_t)(now - last_try) > 3000)) {
                first = false;
                last_try = now;
                lsn = net_pkg_tcp_listen(g_port);
                if (lsn > 0) {
                    log_addf(LOG_K_INFO, "ssh: listening on port %u", (unsigned)g_port);
                    g_listen_noted = false;
                } else if (!g_listen_noted) {
                    g_listen_noted = true;
                    log_addf(LOG_K_WARN, "ssh: could not listen on port %u", (unsigned)g_port);
                }
            }
        } else {
            int h = net_tcp_accept_now(lsn);
            if (h > 0) accept_one(h, now);
            else if (h < 0) { net_pkg_tcp_close(lsn); lsn = -1; }
        }
        bool busy = false;
        for (int i = 0; i < SSHD_SESSIONS; i++)
            if (g_slot[i]) busy |= pump(g_slot[i], now);
        reap(now);
        if (busy) task_yield();
        else      task_sleep_ms(10);
    }

    // Stopping: no new connections, every session told, and a bounded wait.
    if (lsn > 0) net_pkg_tcp_close(lsn);
    uint32_t t0 = task_now_ms();
    for (;;) {
        uint32_t now = task_now_ms();
        bool any = false;
        for (int i = 0; i < SSHD_SESSIONS; i++) {
            SshdSession *s = g_slot[i];
            if (!s) continue;
            any = true;
            if (!s->closing) {
                ssh_disconnect(&s->srv.c, SSH_DISC_BY_APPLICATION, "server stopping");
                start_close(s, now, "server stopped");
            }
            pump(s, now);
        }
        reap(now);
        if (!any || (uint32_t)(now - t0) > SSHD_CLOSE_GRACE_MS + 3000) break;
        task_sleep_ms(10);
    }
    ssh_wipe(&g_hostkey, sizeof(g_hostkey));
    g_pid = 0;
    return 0;
}

// --- turning it on and off ---------------------------------------------------------------

static bool sshd_start(bool quiet) {
    if (g_pid) return true;
    const char *p = reg_get("SSH.Port", "22");
    long port = strtol(p, nullptr, 10);
    g_port = (port > 0 && port < 65536) ? (uint16_t)port : SSHD_DEFAULT_PORT;
    if (!hostkey_ready(true)) {
        if (!quiet) out_err("Could not make or read the host key in /os/ssh.");
        log_add(LOG_K_ERR, "ssh: no host key - server not started");
        return false;
    }
    g_run = true;
    // TASK_STACK_NET: lwIP and the radio driver call deep and do not yield on
    // the way, and the elliptic-curve arithmetic sits on top of that.
    int pid = task_spawn("sshd", "(ssh)", sshd_main, nullptr, TASK_STACK_NET, AFFINITY_CORE0);
    if (pid < 0) {
        g_run = false;
        if (!quiet) out_err("Could not start the SSH server: no memory for its task.");
        return false;
    }
    g_pid = pid;
    return true;
}

static void sshd_stop(void) {
    g_run = false;
    // The task finishes its sessions and clears g_pid; a few seconds at most.
    for (int i = 0; i < 1600 && g_pid; i++) task_sleep_ms(10);
}

void sshd_boot(void) {
    if (strcmp(reg_get("SSH.Server", "off"), "on") == 0) sshd_start(true);
}

int sshd_command(int argc, char **argv) {
    const char *sub = argc > 2 ? argv[2] : "status";
    bool admin = users_is_admin(session_user());

    if (!strcmp(sub, "status")) {
        bool want = strcmp(reg_get("SSH.Server", "off"), "on") == 0;
        if (!g_pid) out_infop("ssh", "Server off%s.", want ? " (set to start at boot)" : "");
        else {
            // Copied out under the lock: the sshd task frees a session when it
            // ends, and this runs on whichever task typed the command.
            char who[SSHD_SESSIONS][TASK_USER_MAX], from[SSHD_SESSIONS][24];
            int n = 0;
            {
                LockGuard g(&g_slots_lock);
                for (int i = 0; i < SSHD_SESSIONS; i++) {
                    SshdSession *s = g_slot[i];
                    if (!s || !s->srv.authed || s->closing) continue;
                    snprintf(who[n], sizeof(who[n]), "%s", s->user);
                    snprintf(from[n], sizeof(from[n]), "%s", peer_of(s));
                    n++;
                }
            }
            out_okp("ssh", "Server on, port %u, %d of %d sessions in use.", (unsigned)g_port, n, SSHD_SESSIONS);
            for (int i = 0; i < n; i++) out_multi("  %-12s from %s", who[i], from[i]);
        }
        char fp[64];
        if (fingerprint_now(fp, sizeof(fp))) out_multi("  host key  ECDSA %s", fp);
        return 0;
    }
    if (!strcmp(sub, "fingerprint")) {
        char fp[64];
        if (!fingerprint_now(fp, sizeof(fp))) { out_info("No host key yet: one is made when the server first starts."); return 1; }
        out_multi("256 %s (ECDSA)", fp);
        return 0;
    }
    if (!admin) { out_err("'ssh server %s' needs an admin account.", sub); return 1; }

    if (!strcmp(sub, "on")) {
        reg_set("SSH.Server", "on");
        persist_save_dirty();
        if (!sshd_start(false)) return 1;
        char fp[64]; hostkey_fingerprint(fp, sizeof(fp));
        out_okp("ssh", "Server on, port %u. It starts again at every boot.", (unsigned)g_port);
        out_multi("  host key  ECDSA %s", fp);
        out_multi("  Accounts without a password cannot log in. Keys go in ~/.ssh/authorized_keys.");
        if (!net_is_connected()) out_multi("  Not connected yet: it will listen once WiFi is up.");
        log_add(LOG_K_WARN, "ssh: server turned on");
        return 0;
    }
    if (!strcmp(sub, "off")) {
        reg_set("SSH.Server", "off");
        persist_save_dirty();
        if (session_is_remote()) {
            // Stopping the server from inside one of its own sessions: this
            // session is one of those it is about to end, so do not wait here.
            g_run = false;
            out_okp("ssh", "Server stopping; this session ends with it.");
            return 0;
        }
        sshd_stop();
        out_okp("ssh", "Server off.");
        log_add(LOG_K_WARN, "ssh: server turned off");
        return 0;
    }
    if (!strcmp(sub, "port") && argc > 3) {
        long port = strtol(argv[3], nullptr, 10);
        if (port < 1 || port > 65535) { out_err("A port is 1 to 65535."); return 1; }
        char v[8]; snprintf(v, sizeof(v), "%ld", port);
        reg_set("SSH.Port", v);
        persist_save_dirty();
        out_okp("ssh", "Port %ld from the next start ('ssh server off', then 'on').", port);
        return 0;
    }
    if (!strcmp(sub, "rekey")) {
        // A fresh host key. Every client that knew the old one will refuse to
        // connect until its known_hosts entry is removed — which is the point.
        if (g_pid) { out_err("Turn the server off first."); return 1; }
        if (!session_confirm("Replace the host key? Every client will warn about the change")) {
            out_info("Cancelled."); return 1;
        }
        storage_remove(HOSTKEY_PATH);
        ssh_wipe(&g_hostkey, sizeof(g_hostkey));
        if (!hostkey_ready(true)) { out_err("Could not make a new host key."); return 1; }
        char fp[64]; hostkey_fingerprint(fp, sizeof(fp));
        out_okp("ssh", "New host key ECDSA %s", fp);
        ssh_wipe(&g_hostkey, sizeof(g_hostkey));     // the server is off: no need to hold it
        log_add(LOG_K_WARN, "ssh: host key replaced");
        return 0;
    }
    out_multi("Usage: ssh server status|on|off|fingerprint|port <n>|rekey");
    return 1;
}
