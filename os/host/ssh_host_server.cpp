// The SSH server core behind a real socket, so a real OpenSSH client can talk
// to it. Not a test on its own: ssh_interop_test.sh starts it and drives it
// with `ssh`.
//
// It is built from the same sources the firmware uses for everything that
// speaks the protocol — sshtrans, sshserver, sshsess, sshkeys, sshcrypto and
// the line editor — and shaped the way the device glue is: a network side that
// owns the connection, and a shell side on another thread that only ever sees
// the two rings in SshSess. What stands in for the device is the shell itself,
// a handful of commands that exist to be checked: echo, whoami, big N (N bytes
// in a fixed pattern), sleep N (interruptible by Ctrl+C), false, exit [N].
//
//   ssh_host_server <port> <authorized_keys> <hostkey.pub out>
//
// One account: "tester", password "secret". Each connection is a child
// process, so two sessions can run at once and one crashing cannot take the
// other with it.
#include "sshserver.h"
#include "sshsess.h"
#include "sshkeys.h"
#include "lineedit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

extern "C" int mbedtls_hardware_poll(void *, unsigned char *out, size_t len, size_t *olen) {
    static int fd = -1;
    if (fd < 0) fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, out + got, len - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    *olen = got;
    return 0;
}

static const char *g_authkeys;
static SshHostKey  g_hk;

struct Conn {
    SshServer srv;
    SshSess   sess;
    pthread_t th;
    bool      started;
};

// --- the account ---------------------------------------------------------------------

static bool op_user_ok(void *, const char *u) { return strcmp(u, "tester") == 0; }
static bool op_password(void *, const char *u, const char *pw) {
    return strcmp(u, "tester") == 0 && strcmp(pw, "secret") == 0;
}
static bool op_pubkey(void *, const char *, const uint8_t *blob, uint32_t n) {
    FILE *f = fopen(g_authkeys, "rb");
    if (!f) return false;
    static char text[16384];
    size_t len = fread(text, 1, sizeof(text), f);
    fclose(f);
    return ssh_authkeys_match(text, (uint32_t)len, blob, n);
}
static void op_auth_log(void *, const char *u, const char *m, bool ok) {
    fprintf(stderr, "auth %s %s %s\n", u, m, ok ? "accepted" : "refused");
}

// --- the shell side -----------------------------------------------------------------

static void out(SshSess *s, const char *d, uint32_t n) {
    while (n) {
        if (s->hangup) return;
        uint32_t w = ssh_sess_write(s, d, n);
        d += w; n -= w;
        if (n) usleep(200);
    }
}
static void outs(SshSess *s, const char *str) { out(s, str, (uint32_t)strlen(str)); }

static const char kPattern[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/";

static int run_cmd(SshSess *s, char *line, bool *quit) {
    char *argv[8]; int argc = 0;
    for (char *t = strtok(line, " "); t && argc < 8; t = strtok(nullptr, " ")) argv[argc++] = t;
    if (!argc) return 0;
    if (!strcmp(argv[0], "echo")) {
        for (int i = 1; i < argc; i++) { if (i > 1) outs(s, " "); outs(s, argv[i]); }
        outs(s, "\n");
        return 0;
    }
    if (!strcmp(argv[0], "whoami")) { outs(s, "tester\n"); return 0; }
    if (!strcmp(argv[0], "false"))  return 1;
    if (!strcmp(argv[0], "exit"))   { *quit = true; return argc > 1 ? atoi(argv[1]) : 0; }
    if (!strcmp(argv[0], "big") && argc > 1) {
        long n = atol(argv[1]);
        char buf[512];
        for (long j = 0; j < n; ) {
            uint32_t k = 0;
            while (k < sizeof(buf) && j < n) {
                buf[k++] = (j % 64 == 63) ? '\n' : kPattern[j % 64];
                j++;
            }
            out(s, buf, k);
            if (s->hangup) return 1;
        }
        return 0;
    }
    if (!strcmp(argv[0], "sleep") && argc > 1) {
        int ms = atoi(argv[1]) * 1000;
        for (int t = 0; t < ms; t += 10) {
            if (s->intr) { s->intr = false; outs(s, "interrupted\n"); return 130; }
            if (s->hangup) return 1;
            usleep(10000);
        }
        outs(s, "slept\n");
        return 0;
    }
    outs(s, argv[0]); outs(s, ": unknown command\n");
    return 127;
}

struct EditCtx { SshSess *s; bool ended; };

static int le_getch(void *ctx, uint32_t) {
    EditCtx *e = (EditCtx *)ctx;
    int c = ssh_sess_getch(e->s);
    if (c >= 0) return c;
    // Nothing more will come: end the line so the loop can see it.
    if (e->s->hangup || e->s->eof) { e->ended = true; return '\n'; }
    usleep(1000);
    return LE_NO_KEY;
}
static void le_putch(void *ctx, char c) { out(((EditCtx *)ctx)->s, &c, 1); }

static void *shell_main(void *arg) {
    Conn *k = (Conn *)arg;
    SshSess *s = &k->sess;
    int status = 0;
    if (k->srv.mode == SSH_MODE_EXEC) {
        bool quit = false;
        s->running = true;
        status = run_cmd(s, k->srv.exec, &quit);
        s->running = false;
    } else {
        outs(s, "host test shell\n");
        EditCtx e{ s, false };
        LineEdit le{};
        le.io.getch = le_getch;
        le.io.putch = le_putch;
        le.io.ctx = &e;
        le.prompt = "tester@host> ";
        for (;;) {
            char line[128];
            line_edit(&le, line, sizeof(line));
            if (s->hangup) break;
            bool quit = false;
            s->intr = false;
            s->running = true;
            status = run_cmd(s, line, &quit);
            s->running = false;
            if (quit || e.ended) break;
        }
    }
    s->exit_status = status;
    __atomic_store_n(&s->done, true, __ATOMIC_RELEASE);
    return nullptr;
}

// --- the network side ---------------------------------------------------------------

static void op_start(void *ctx) {
    Conn *k = (Conn *)ctx;
    ssh_sess_init(&k->sess, k->srv.pty);
    k->sess.cols = (uint16_t)k->srv.cols;
    k->sess.rows = (uint16_t)k->srv.rows;
    k->started = pthread_create(&k->th, nullptr, shell_main, k) == 0;
}
static void op_data(void *ctx, const uint8_t *d, uint32_t n) {
    Conn *k = (Conn *)ctx;
    if (!k->started || k->srv.mode == SSH_MODE_EXEC) { ssh_srv_consumed(&k->srv, n); return; }
    uint32_t dropped = ssh_sess_input(&k->sess, d, n);
    if (dropped) ssh_srv_consumed(&k->srv, dropped);
}
static void op_signal(void *ctx, const char *name) {
    Conn *k = (Conn *)ctx;
    if (k->started && !strcmp(name, "INT")) k->sess.intr = true;
}
static void op_winch(void *ctx) {
    Conn *k = (Conn *)ctx;
    k->sess.cols = (uint16_t)k->srv.cols; k->sess.rows = (uint16_t)k->srv.rows;
}
static void op_eof(void *ctx)   { ((Conn *)ctx)->sess.eof = true; }
static void op_close(void *ctx) { ((Conn *)ctx)->sess.hangup = true; }

static const SshServerOps kOps = {
    op_user_ok, op_password, op_pubkey, op_auth_log,
    op_start, op_data, op_signal, op_winch, op_eof, op_close,
};

static bool send_all(int fd, SshConn *c) {
    uint32_t n;
    const uint8_t *p = ssh_out(c, &n);
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EAGAIN || errno == EINTR) { usleep(500); continue; } return false; }
        ssh_out_consume(c, (uint32_t)w);
        p = ssh_out(c, &n);
    }
    return true;
}

static void serve(int fd) {
    static Conn k;
    memset(&k, 0, sizeof(k));
    if (!ssh_srv_init(&k.srv, &g_hk, &kOps, &k)) { close(fd); return; }
    uint8_t buf[4096];
    uint32_t pend = 0, pend_at = 0;
    uint32_t regranted = 0;
    bool exited = false;
    for (;;) {
        // Read, unless what was read last time is still waiting for room.
        if (!pend) {
            struct pollfd p = { fd, POLLIN, 0 };
            if (poll(&p, 1, 5) > 0) {
                ssize_t r = recv(fd, buf, sizeof(buf), 0);
                if (r <= 0) { k.sess.hangup = true; break; }
                pend = (uint32_t)r; pend_at = 0;
            }
        }
        if (pend) {
            uint32_t took = ssh_feed(&k.srv.c, buf + pend_at, pend);
            pend -= took; pend_at += took;
        }
        ssh_process(&k.srv.c);
        if (k.srv.penalty_ms) {
            // The device holds the connection the same way: no answer until
            // the pause is over.
            usleep(k.srv.penalty_ms * 1000u);
            k.srv.penalty_ms = 0;
        }
        if (k.started) {
            // Output from the shell, as much as the window and buffer allow.
            const uint8_t *p;
            uint32_t n;
            while ((n = ssh_ring_peek(&k.sess.out, &p)) != 0) {
                uint32_t sent = ssh_srv_send(&k.srv, p, n);
                ssh_ring_drop(&k.sess.out, sent);
                if (sent < n) break;
            }
            uint32_t used = __atomic_load_n(&k.sess.consumed, __ATOMIC_ACQUIRE);
            if (used != regranted) { ssh_srv_consumed(&k.srv, used - regranted); regranted = used; }
            if (!exited && __atomic_load_n(&k.sess.done, __ATOMIC_ACQUIRE) &&
                ssh_ring_used(&k.sess.out) == 0)
                exited = ssh_srv_exit(&k.srv, (uint32_t)k.sess.exit_status);
        }
        if (!send_all(fd, &k.srv.c)) { k.sess.hangup = true; break; }
        if (ssh_closed(&k.srv.c)) break;
        if (exited && k.srv.close_in) break;
    }
    if (k.srv.c.why[0]) fprintf(stderr, "closed: %s\n", k.srv.c.why);
    k.sess.hangup = true;
    if (k.started) pthread_join(k.th, nullptr);
    send_all(fd, &k.srv.c);
    ssh_srv_free(&k.srv);
    shutdown(fd, SHUT_WR);
    close(fd);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s port authorized_keys hostkey.pub\n", argv[0]); return 2; }
    g_authkeys = argv[2];
    if (!ssh_hostkey_generate(&g_hk)) { fprintf(stderr, "no host key\n"); return 1; }
    uint8_t blob[160];
    uint32_t bn = ssh_hostkey_blob(&g_hk, blob, sizeof(blob));
    char line[400];
    uint32_t ln = ssh_pubkey_line(blob, bn, "host-test", line, sizeof(line));
    FILE *f = fopen(argv[3], "w");
    if (!f || !ln) return 1;
    fprintf(f, "%s\n", line);
    fclose(f);
    char fp[64];
    ssh_fingerprint(blob, bn, fp, sizeof(fp));
    printf("FINGERPRINT %s\n", fp);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)atoi(argv[1]));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 4) != 0) { perror("listen"); return 1; }
    signal(SIGCHLD, SIG_IGN);
    printf("READY\n");
    fflush(stdout);
    for (;;) {
        int fd = accept(ls, nullptr, nullptr);
        if (fd < 0) continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pid_t pid = fork();
        if (pid == 0) { close(ls); serve(fd); _exit(0); }
        close(fd);
    }
}
