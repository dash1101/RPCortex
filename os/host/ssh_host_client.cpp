// The SSH client core behind a real socket, for ssh_interop_test.sh: against
// the server core (ssh_host_server) and against a real OpenSSH sshd.
//
//   ssh_host_client [-p port] [-i keyfile] [-P password] [-k known_hosts]
//                   [-H hostkey-algs] [-t] user@host [command]
//
// -i names a 32-byte raw P-256 secret, the format the device keeps its keys
// in; it is made if missing and its public line written to <keyfile>.pub.
// known_hosts is trust on first use with an automatic yes, and a changed key
// is refused. Exits with the remote exit status, or 255.
#include "sshclient.h"
#include "sshkeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

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

static const char *g_kh;
static const char *g_host;
static uint16_t    g_port = 22;
static const char *g_password;

static bool op_hostkey(void *, const uint8_t *blob, uint32_t n) {
    char fp[64];
    ssh_fingerprint(blob, n, fp, sizeof(fp));
    fprintf(stderr, "host key %s\n", fp);
    if (!g_kh) return true;
    static char text[65536];
    size_t len = 0;
    if (FILE *f = fopen(g_kh, "rb")) { len = fread(text, 1, sizeof(text), f); fclose(f); }
    SshKnown k = ssh_known_hosts_check(text, (uint32_t)len, g_host, g_port, blob, n);
    if (k == SSH_KH_MATCH) { fprintf(stderr, "known host\n"); return true; }
    if (k == SSH_KH_CHANGED) { fprintf(stderr, "HOST KEY CHANGED\n"); return false; }
    char line[1600];
    uint32_t ln = ssh_known_hosts_line(g_host, g_port, blob, n, line, sizeof(line));
    if (FILE *f = fopen(g_kh, "ab")) { fwrite(line, 1, ln, f); fclose(f); }
    fprintf(stderr, "new host recorded\n");
    return true;
}

static bool op_prompt(void *, const char *prompt, bool, char *out, uint32_t cap) {
    fprintf(stderr, "prompt: %s\n", prompt);
    if (!g_password) return false;
    snprintf(out, cap, "%s", g_password);
    return true;
}
static void op_banner(void *, const char *t) { fprintf(stderr, "banner: %s\n", t); }
static void op_data(void *, const uint8_t *d, uint32_t n, bool err) {
    fwrite(d, 1, n, err ? stderr : stdout);
    fflush(err ? stderr : stdout);
}

static const SshClientOps kOps = { op_hostkey, op_prompt, op_banner, op_data };

static bool load_key(const char *path, SshHostKey *k) {
    uint8_t d[32];
    FILE *f = fopen(path, "rb");
    if (f) {
        bool ok = fread(d, 1, 32, f) == 32;
        fclose(f);
        return ok && ssh_hostkey_load(k, d);
    }
    if (!ssh_hostkey_generate(k)) return false;
    f = fopen(path, "wb");
    if (!f) return false;
    fwrite(k->d, 1, 32, f);
    fclose(f);
    uint8_t blob[160];
    uint32_t bn = ssh_hostkey_blob(k, blob, sizeof(blob));
    char line[400];
    uint32_t ln = ssh_pubkey_line(blob, bn, "rpcortex-test", line, sizeof(line));
    char pub[512];
    snprintf(pub, sizeof(pub), "%s.pub", path);
    f = fopen(pub, "w");
    if (!f) return false;
    fprintf(f, "%.*s\n", (int)ln, line);
    fclose(f);
    return true;
}

int main(int argc, char **argv) {
    const char *keyfile = nullptr, *hk_algs = nullptr;
    bool tty = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) g_port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) keyfile = argv[++i];
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) g_password = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) g_kh = argv[++i];
        else if (!strcmp(argv[i], "-H") && i + 1 < argc) hk_algs = argv[++i];
        else if (!strcmp(argv[i], "-t")) tty = true;
        else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "usage: ssh_host_client [opts] user@host [command]\n"); return 2; }
    char user[64];
    const char *at = strchr(argv[i], '@');
    if (!at) return 2;
    snprintf(user, sizeof(user), "%.*s", (int)(at - argv[i]), argv[i]);
    g_host = at + 1;
    const char *command = i + 1 < argc ? argv[i + 1] : nullptr;

    static SshHostKey idkey;
    if (keyfile && !load_key(keyfile, &idkey)) { fprintf(stderr, "key file\n"); return 255; }

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[8]; snprintf(ps, sizeof(ps), "%u", (unsigned)g_port);
    if (getaddrinfo(g_host, ps, &hints, &res) != 0) return 255;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { perror("connect"); return 255; }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    static SshClient k;
    if (!ssh_cli_init(&k, user, &kOps, nullptr)) return 255;
    if (keyfile) k.idkey = &idkey;
    if (hk_algs) k.c.hostkey_algs = hk_algs;
    k.want_pty = tty;
    k.command = command;
    snprintf(k.term, sizeof(k.term), "xterm");
    k.cols = 80; k.rows = 24;

    uint8_t buf[4096], inb[512];
    uint32_t pend = 0, pend_at = 0, inpend = 0, in_at = 0;
    bool stdin_open = !command || tty;
    for (;;) {
        pollfd p[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
        int np = (stdin_open && !inpend) ? 2 : 1;
        if (pend) np = 0;
        if (np && poll(p, (nfds_t)np, 5) > 0) {
            if (p[0].revents & (POLLIN | POLLHUP)) {
                ssize_t r = recv(fd, buf, sizeof(buf), 0);
                if (r <= 0) break;
                pend = (uint32_t)r; pend_at = 0;
            }
            if (np == 2 && (p[1].revents & (POLLIN | POLLHUP))) {
                ssize_t r = read(0, inb, sizeof(inb));
                if (r <= 0) stdin_open = false;
                else { inpend = (uint32_t)r; in_at = 0; }
            }
        }
        if (pend) {
            uint32_t took = ssh_feed(&k.c, buf + pend_at, pend);
            pend -= took; pend_at += took;
        }
        ssh_process(&k.c);
        if (inpend && k.stage == SSH_CLI_RUNNING) {
            uint32_t s = ssh_cli_send(&k, inb + in_at, inpend);
            inpend -= s; in_at += s;
        }
        uint32_t n;
        const uint8_t *o = ssh_out(&k.c, &n);
        while (n) {
            ssize_t w = send(fd, o, n, MSG_NOSIGNAL);
            if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; goto done; }
            ssh_out_consume(&k.c, (uint32_t)w);
            o = ssh_out(&k.c, &n);
        }
        if (ssh_closed(&k.c)) break;
    }
done:
    close(fd);
    if (k.failed[0]) fprintf(stderr, "failed: %s\n", k.failed);
    else if (k.c.why[0] && !k.have_status) fprintf(stderr, "closed: %s\n", k.c.why);
    int rc = k.have_status ? k.exit_status : 255;
    ssh_cli_free(&k);
    return rc;
}
