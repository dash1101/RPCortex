// Mutation fuzzing of the SSH core: everything a peer can send before and
// after it has authenticated, and the key files a user can write.
//
// The same discipline as fuzz_test.cpp: a valid input, damaged a few bytes at
// a time, fed in under AddressSanitizer and UBSan with halt-on-error, plus the
// few promises each target makes checked on every iteration.
//
//   pre-kex     the plaintext an attacker controls before any key exists: the
//               version line, the KEXINIT and the ECDH_INIT, recorded from the
//               client core and delivered in random-sized pieces
//   on-wire     an encrypted session with bits flipped in transit: it must
//               end, and never with damaged data handed up as good
//   server      every service-layer message the server parses — userauth,
//               channel open, requests, data, window — called on a session
//               forced past the key exchange, authenticated or not
//   client      the same for the client's parsers
//   keys        authorized_keys and known_hosts text, ECDSA and Ed25519 lines
//   ed25519     an Ed25519 key and signature blob made by OpenSSH, and an
//               ssh-ed25519 login request signed by RFC 8032's own code:
//               damage to either must never be accepted
//
// Deterministic; RPC_FUZZ_SEED and RPC_FUZZ_ITERS as for fuzz_test.
#include "sshserver.h"
#include "sshclient.h"
#include "sshkeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#include "ssh_vectors.inc"

typedef std::vector<uint8_t> Bytes;

static uint32_t unhex(const char *h, uint8_t *out, uint32_t cap) {
    uint32_t n = 0;
    for (; h[0] && h[1] && n < cap; h += 2) {
        unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v;
    }
    return n;
}

extern "C" int mbedtls_hardware_poll(void *, unsigned char *out, size_t len, size_t *olen) {
    static int fd = -1;
    if (fd < 0) fd = open("/dev/urandom", O_RDONLY);
    if (read(fd, out, len) != (ssize_t)len) return -1;
    *olen = len;
    return 0;
}

static int checks = 0, fails = 0;
static void ck(bool c, const char *what, uint32_t iter) {
    checks++;
    if (!c) { fails++; if (fails < 20) printf("  FAIL: %s  (iteration %u)\n", what, iter); }
}

static uint64_t g_rng;
static uint32_t rnd(void) {
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

static void mutate(Bytes &b, uint32_t max_len) {
    uint32_t ops = 1 + below(4);
    while (ops--) {
        uint32_t n = (uint32_t)b.size();
        switch (below(8)) {
        case 0: if (n) b[below(n)] ^= (uint8_t)(1u << below(8)); break;
        case 1: if (n) b[below(n)] = (uint8_t)rnd(); break;
        case 2: {
            uint32_t at = below(n + 1), k = 1 + below(8);
            if (n + k > max_len) break;
            for (uint32_t i = 0; i < k; i++) b.insert(b.begin() + at, (uint8_t)rnd());
            break;
        }
        case 3: if (n > 1) {
            uint32_t at = below(n), k = 1 + below(n - at < 16 ? n - at : 16);
            b.erase(b.begin() + at, b.begin() + at + k);
        }
            break;
        case 4: if (n > 2) b.resize(1 + below(n - 1)); break;
        case 5: if (n >= 4) {           // a big-endian length at a boundary
            static const uint32_t v[] = { 0, 1, 3, 4, 0x7fffffffu, 0x80000000u, 0xffffffffu,
                                          0xffff, 0x10000, 35000, 35001, 2048, 2049 };
            uint32_t x = v[below(sizeof(v) / sizeof(v[0]))], at = below(n - 3);
            for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(x >> (24 - 8 * i));
        }
            break;
        case 6: if (n > 2) {
            uint32_t at = below(n), k = 1 + below(n - at < 32 ? n - at : 32);
            if (n + k > max_len) break;
            Bytes run(b.begin() + at, b.begin() + at + k);
            b.insert(b.begin() + below(n + 1), run.begin(), run.end());
        }
            break;
        default: if (n) b[below(n)] = below(2) ? 0x00 : 0xff; break;
        }
        if (b.empty()) b.push_back((uint8_t)rnd());
    }
}

static SshHostKey g_hk, g_uk;

static bool user_ok(void *, const char *u) { return !strcmp(u, "alice"); }
static bool password(void *, const char *, const char *pw) { return !strcmp(pw, "pw"); }
// alice's keys: the device-made ECDSA one, and the Ed25519 one the signed
// login seed uses, so a request with either reaches the signature check.
static bool pubkey(void *, const char *, const uint8_t *b, uint32_t n) {
    uint8_t blob[160]; uint32_t bn = ssh_hostkey_blob(&g_uk, blob, sizeof(blob));
    if (n == bn && !memcmp(b, blob, n)) return true;
    bn = unhex(kED_UA_BLOB, blob, sizeof(blob));
    return n == bn && !memcmp(b, blob, n);
}
static uint32_t g_delivered;
static bool     g_bad_delivery;
static uint8_t  g_expect;
static void on_data(void *ctx, const uint8_t *d, uint32_t n) {
    g_delivered += n;
    for (uint32_t i = 0; i < n; i++) if (d[i] != g_expect) g_bad_delivery = true;
    if (ctx) ssh_srv_consumed((SshServer *)ctx, n);
}
static const SshServerOps kOps = { user_ok, password, pubkey, nullptr, nullptr, on_data,
                                   nullptr, nullptr, nullptr, nullptr };

static bool cl_hostkey(void *, const uint8_t *, uint32_t) { return true; }
static bool cl_prompt(void *, const char *, bool, char *out, uint32_t cap) { snprintf(out, cap, "pw"); return true; }
static void cl_data(void *, const uint8_t *, uint32_t, bool) {}
static const SshClientOps kClOps = { cl_hostkey, cl_prompt, nullptr, cl_data };

static void drive(SshConn *a, SshConn *b) {
    for (int r = 0; r < 60; r++) {
        ssh_process(a); ssh_process(b);
        uint32_t n; const uint8_t *p;
        p = ssh_out(a, &n); if (n) ssh_out_consume(a, ssh_feed(b, p, n));
        p = ssh_out(b, &n); if (n) ssh_out_consume(b, ssh_feed(a, p, n));
        if (ssh_closed(a) || ssh_closed(b)) break;
    }
}

// --- pre-kex ---------------------------------------------------------------------

static void fuzz_prekex(uint32_t iters) {
    // Record what a client sends before it has any keys: its version, its
    // KEXINIT and, once it has seen the server's KEXINIT, its ECDH_INIT.
    for (int alg = 0; alg < 2; alg++) {
        SshServer s; SshClient k;
        ssh_srv_init(&s, &g_hk, &kOps, nullptr);
        ssh_cli_init(&k, "alice", &kClOps, nullptr);
        if (alg) k.c.hostkey_algs = "ecdsa-sha2-nistp256";
        ssh_process(&k.c); ssh_process(&s.c);
        Bytes seed;
        uint32_t n; const uint8_t *p = ssh_out(&k.c, &n);
        seed.insert(seed.end(), p, p + n); ssh_out_consume(&k.c, n);
        p = ssh_out(&s.c, &n); ssh_feed(&k.c, p, n); ssh_out_consume(&s.c, n);
        ssh_process(&k.c);
        p = ssh_out(&k.c, &n);
        seed.insert(seed.end(), p, p + n);
        ssh_srv_free(&s); ssh_cli_free(&k);

        for (uint32_t it = 0; it < iters; it++) {
            Bytes in = seed;
            mutate(in, 4096);
            SshServer t;
            ssh_srv_init(&t, &g_hk, &kOps, nullptr);
            uint32_t at = 0;
            while (at < in.size() && !ssh_closed(&t.c)) {
                uint32_t piece = 1 + below(200);
                if (piece > in.size() - at) piece = (uint32_t)in.size() - at;
                at += ssh_feed(&t.c, in.data() + at, piece);
                ssh_process(&t.c);
                uint32_t on; ssh_out(&t.c, &on); ssh_out_consume(&t.c, on);
            }
            ck(!t.authed && !t.chan, "nothing past the key exchange from plaintext", it);
            ck(t.c.in_len <= t.c.in_cap, "input buffer accounting", it);
            ssh_srv_free(&t);
        }
    }
}

// --- an encrypted session with damage on the wire ---------------------------------

static void fuzz_onwire(uint32_t iters) {
    for (uint32_t it = 0; it < iters; it++) {
        SshServer s; SshClient k;
        ssh_srv_init(&s, &g_hk, &kOps, &s);
        ssh_cli_init(&k, "alice", &kClOps, nullptr);
        k.command = "x";
        drive(&k.c, &s.c);
        if (!s.chan) { ck(false, "session came up", it); ssh_srv_free(&s); ssh_cli_free(&k); continue; }
        g_expect = (uint8_t)(0x41 + it % 26);
        uint8_t data[300]; memset(data, g_expect, sizeof(data));
        g_delivered = 0; g_bad_delivery = false;
        for (int r = 0; r < 6; r++) ssh_cli_send(&k, data, sizeof(data));
        uint32_t n; const uint8_t *p = ssh_out(&k.c, &n);
        Bytes wire(p, p + n);
        ssh_out_consume(&k.c, n);
        mutate(wire, 8192);
        uint32_t at = 0;
        while (at < wire.size() && !ssh_closed(&s.c)) {
            uint32_t piece = 1 + below(400);
            if (piece > wire.size() - at) piece = (uint32_t)wire.size() - at;
            uint32_t took = ssh_feed(&s.c, wire.data() + at, piece);
            at += took;
            ssh_process(&s.c);
            uint32_t on; ssh_out(&s.c, &on); ssh_out_consume(&s.c, on);
            if (!took && !ssh_closed(&s.c) && s.c.in_len == s.c.in_cap) break;
        }
        ck(!g_bad_delivery, "damaged data never handed up as good", it);
        ssh_srv_free(&s); ssh_cli_free(&k);
    }
}

// --- the service layers, parsed directly ---------------------------------------------

static void put32(Bytes &b, uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back((uint8_t)(v >> (8 * i))); }
static void putstr(Bytes &b, const void *d, uint32_t n) { put32(b, n); b.insert(b.end(), (const uint8_t *)d, (const uint8_t *)d + n); }
static void putc_(Bytes &b, const char *s) { putstr(b, s, (uint32_t)strlen(s)); }

static std::vector<Bytes> server_seeds() {
    std::vector<Bytes> v;
    Bytes b;
    b = { SSH_MSG_SERVICE_REQUEST }; putc_(b, "ssh-userauth"); v.push_back(b);
    b = { SSH_MSG_USERAUTH_REQUEST }; putc_(b, "alice"); putc_(b, "ssh-connection"); putc_(b, "none"); v.push_back(b);
    b = { SSH_MSG_USERAUTH_REQUEST }; putc_(b, "alice"); putc_(b, "ssh-connection"); putc_(b, "password");
    b.push_back(0); putc_(b, "pw"); v.push_back(b);
    uint8_t blob[160]; uint32_t bn = ssh_hostkey_blob(&g_uk, blob, sizeof(blob));
    uint8_t sig[160]; uint32_t sn = ssh_hostkey_sign(&g_uk, (const uint8_t *)"x", 1, sig, sizeof(sig));
    b = { SSH_MSG_USERAUTH_REQUEST }; putc_(b, "alice"); putc_(b, "ssh-connection"); putc_(b, "publickey");
    b.push_back(1); putc_(b, "ecdsa-sha2-nistp256"); putstr(b, blob, bn); putstr(b, sig, sn); v.push_back(b);
    b = { SSH_MSG_USERAUTH_REQUEST }; putc_(b, "alice"); putc_(b, "ssh-connection"); putc_(b, "publickey");
    b.push_back(0); putc_(b, "rsa-sha2-256"); putstr(b, blob, bn); v.push_back(b);
    // The Ed25519 query form, which ends in PK_OK. (The signed form runs a
    // verify per input and has its own, shorter, target below.)
    uint8_t eb[64]; uint32_t ebn = unhex(kED_UA_BLOB, eb, sizeof(eb));
    b = { SSH_MSG_USERAUTH_REQUEST }; putc_(b, "alice"); putc_(b, "ssh-connection"); putc_(b, "publickey");
    b.push_back(0); putc_(b, "ssh-ed25519"); putstr(b, eb, ebn); v.push_back(b);
    b = { SSH_MSG_CHANNEL_OPEN }; putc_(b, "session"); put32(b, 0); put32(b, 1 << 20); put32(b, 32768); v.push_back(b);
    b = { SSH_MSG_CHANNEL_OPEN }; putc_(b, "direct-tcpip"); put32(b, 1); put32(b, 100); put32(b, 100); v.push_back(b);
    b = { SSH_MSG_CHANNEL_REQUEST }; put32(b, 0); putc_(b, "pty-req"); b.push_back(1); putc_(b, "xterm");
    put32(b, 80); put32(b, 24); put32(b, 0); put32(b, 0); putc_(b, ""); v.push_back(b);
    b = { SSH_MSG_CHANNEL_REQUEST }; put32(b, 0); putc_(b, "exec"); b.push_back(1); putc_(b, "ls /"); v.push_back(b);
    b = { SSH_MSG_CHANNEL_REQUEST }; put32(b, 0); putc_(b, "window-change"); b.push_back(0);
    put32(b, 100); put32(b, 40); put32(b, 0); put32(b, 0); v.push_back(b);
    b = { SSH_MSG_CHANNEL_REQUEST }; put32(b, 0); putc_(b, "signal"); b.push_back(0); putc_(b, "INT"); v.push_back(b);
    b = { SSH_MSG_CHANNEL_DATA }; put32(b, 0); putc_(b, "typed"); v.push_back(b);
    b = { SSH_MSG_CHANNEL_EXTENDED_DATA }; put32(b, 0); put32(b, 1); putc_(b, "err"); v.push_back(b);
    b = { SSH_MSG_CHANNEL_WINDOW_ADJUST }; put32(b, 0); put32(b, 4096); v.push_back(b);
    b = { SSH_MSG_CHANNEL_EOF }; put32(b, 0); v.push_back(b);
    b = { SSH_MSG_CHANNEL_CLOSE }; put32(b, 0); v.push_back(b);
    b = { SSH_MSG_GLOBAL_REQUEST }; putc_(b, "tcpip-forward"); b.push_back(1); putc_(b, "0.0.0.0"); put32(b, 80); v.push_back(b);
    return v;
}

static void fuzz_server(uint32_t iters) {
    std::vector<Bytes> seeds = server_seeds();
    for (uint32_t it = 0; it < iters; it++) {
        SshServer s;
        ssh_srv_init(&s, &g_hk, &kOps, &s);
        // Past the key exchange, by fiat: the parsers are what is under test.
        s.c.state = SSH_ST_OPEN;
        s.c.first_kex_done = true;
        s.c.have_session_id = true;
        s.service_ok = below(4) != 0;
        s.authed = below(2) != 0;
        if (s.authed && below(2)) {
            s.chan = true; s.peer_window = below(5000); s.peer_maxpkt = below(40000);
            s.local_window = below(SSH_SRV_WINDOW + 1);
        }
        for (int m = 0; m < 4 && !ssh_closed(&s.c); m++) {
            Bytes msg = seeds[below((uint32_t)seeds.size())];
            mutate(msg, 2048);
            SshR r; sshr_init(&r, msg.data() + 1, (uint32_t)msg.size() - 1);
            s.c.dispatch(&s.c, msg[0], &r);
            ck(r.pos <= r.len, "reader stayed inside the message", it);
            ck(s.local_window <= SSH_SRV_WINDOW, "window never above what was offered", it);
            uint32_t on; ssh_out(&s.c, &on); ssh_out_consume(&s.c, on);
            ssh_srv_send(&s, (const uint8_t *)"output", 6);
            ssh_out(&s.c, &on); ssh_out_consume(&s.c, on);
        }
        ck(!(s.authed && strcmp(s.user, "alice") && s.user[0]), "only the real account authenticates", it);
        ssh_srv_free(&s);
    }
}

static void fuzz_client(uint32_t iters) {
    std::vector<Bytes> seeds;
    Bytes b;
    b = { SSH_MSG_USERAUTH_FAILURE }; putc_(b, "publickey,password,keyboard-interactive"); b.push_back(0); seeds.push_back(b);
    b = { SSH_MSG_USERAUTH_BANNER }; putc_(b, "Welcome\x1b[2J"); putc_(b, ""); seeds.push_back(b);
    b = { 60 }; putc_(b, "name"); putc_(b, "instr"); putc_(b, ""); put32(b, 2);
    putc_(b, "Password: "); b.push_back(0); putc_(b, "OTP: "); b.push_back(1); seeds.push_back(b);
    b = { SSH_MSG_USERAUTH_SUCCESS }; seeds.push_back(b);
    b = { SSH_MSG_CHANNEL_OPEN_CONFIRMATION }; put32(b, 0); put32(b, 7); put32(b, 1 << 20); put32(b, 32768); seeds.push_back(b);
    b = { SSH_MSG_CHANNEL_OPEN_FAILURE }; put32(b, 0); put32(b, 2); putc_(b, "no"); putc_(b, ""); seeds.push_back(b);
    b = { SSH_MSG_CHANNEL_DATA }; put32(b, 0); putc_(b, "remote output"); seeds.push_back(b);
    b = { SSH_MSG_CHANNEL_REQUEST }; put32(b, 0); putc_(b, "exit-status"); b.push_back(0); put32(b, 3); seeds.push_back(b);
    b = { SSH_MSG_CHANNEL_SUCCESS }; put32(b, 0); seeds.push_back(b);
    b = { SSH_MSG_CHANNEL_CLOSE }; put32(b, 0); seeds.push_back(b);
    b = { SSH_MSG_GLOBAL_REQUEST }; putc_(b, "hostkeys-00@openssh.com"); b.push_back(1); seeds.push_back(b);
    b = { SSH_MSG_CHANNEL_OPEN }; putc_(b, "forwarded-tcpip"); put32(b, 3); put32(b, 10); put32(b, 10); seeds.push_back(b);
    for (uint32_t it = 0; it < iters; it++) {
        SshClient k;
        ssh_cli_init(&k, "alice", &kClOps, nullptr);
        k.c.state = SSH_ST_OPEN;
        k.c.first_kex_done = true;
        k.stage = (uint8_t)below(5);
        k.kbd_active = below(2) != 0;
        k.idkey = below(2) ? &g_uk : nullptr;
        if (k.stage == SSH_CLI_RUNNING) { k.peer_window = below(5000); k.peer_maxpkt = below(9000); k.local_window = SSH_CLI_WINDOW; }
        for (int m = 0; m < 4 && !ssh_closed(&k.c); m++) {
            Bytes msg = seeds[below((uint32_t)seeds.size())];
            mutate(msg, 2048);
            SshR r; sshr_init(&r, msg.data() + 1, (uint32_t)msg.size() - 1);
            k.c.dispatch(&k.c, msg[0], &r);
            ck(r.pos <= r.len, "client reader stayed inside the message", it);
            uint32_t on; ssh_out(&k.c, &on); ssh_out_consume(&k.c, on);
        }
        ssh_cli_free(&k);
    }
}

// --- key files ----------------------------------------------------------------------

static void fuzz_keys(uint32_t iters) {
    uint8_t blob[160]; uint32_t bn = ssh_hostkey_blob(&g_uk, blob, sizeof(blob));
    char line[400]; uint32_t ln = ssh_pubkey_line(blob, bn, "c", line, sizeof(line));
    char kh[600]; uint32_t kl = ssh_known_hosts_line("host", 2222, blob, bn, kh, sizeof(kh));
    Bytes ak((const uint8_t *)line, (const uint8_t *)line + ln);
    Bytes kb((const uint8_t *)kh, (const uint8_t *)kh + kl);
    // The base64 alone, without the type in front or the comment behind — and
    // without its last three characters, whose spare low bits base64 does not
    // carry: a flip there decodes to the same key, which is a match and fine.
    char b64[300];
    snprintf(b64, sizeof(b64), "%.*s", (int)(strrchr(line, ' ') - (line + 20)) - 3, line + 20);
    for (uint32_t it = 0; it < iters; it++) {
        Bytes a = ak; mutate(a, 1024);
        bool m = ssh_authkeys_match((const char *)a.data(), (uint32_t)a.size(), blob, bn);
        // Damage that still matches has to have left the key itself intact,
        // with its type word in front of it.
        if (m) {
            Bytes probe = a; probe.push_back(0);
            // The type word in front, with any run of blanks between (a
            // second space is still one field separator).
            const char *base = (const char *)probe.data();
            const char *at = strstr(base, b64);
            const char *w = at;
            while (w && w > base && (w[-1] == ' ' || w[-1] == '\t')) w--;
            bool good = at != nullptr && w < at && w - base >= 19 &&
                        !strncmp(w - 19, "ecdsa-sha2-nistp256", 19);
            ck(good, "a match only with the key's base64 present", it);
            if (!good && fails < 5) {
                printf("    input: ");
                for (uint8_t ch : a) printf(ch >= 32 && ch < 127 ? "%c" : "\\x%02x", ch);
                printf("\n");
            }
        }
        Bytes k = kb; mutate(k, 1024);
        SshKnown v = ssh_known_hosts_check((const char *)k.data(), (uint32_t)k.size(), "host", 2222, blob, bn);
        ck(v == SSH_KH_UNKNOWN || v == SSH_KH_MATCH || v == SSH_KH_CHANGED, "known_hosts verdict", it);
    }

    // The same for an ssh-ed25519 line as ssh-keygen writes it, behind an
    // ECDSA line so the parser has to get past one to reach the other.
    uint8_t ed[64]; uint32_t edn = unhex(kED_KEY, ed, sizeof(ed));
    char edtext[600];
    int el = snprintf(edtext, sizeof(edtext), "%s\nssh-ed25519 %s fixture\n", line, kED_LINE);
    Bytes ek((const uint8_t *)edtext, (const uint8_t *)edtext + el);
    char edb64[100];
    snprintf(edb64, sizeof(edb64), "%.*s", (int)strlen(kED_LINE) - 3, kED_LINE);
    for (uint32_t it = 0; it < iters; it++) {
        Bytes a = ek; mutate(a, 1024);
        if (!ssh_authkeys_match((const char *)a.data(), (uint32_t)a.size(), ed, edn)) continue;
        // Searched for as bytes: damage to the ECDSA line in front may have
        // put a NUL there, which the parser reads past and strstr would not.
        const char *base = (const char *)a.data();
        const char *at = (const char *)memmem(base, a.size(), edb64, strlen(edb64));
        const char *w = at;
        while (w && w > base && (w[-1] == ' ' || w[-1] == '\t')) w--;
        bool good = at != nullptr && w < at && w - base >= 11 && !memcmp(w - 11, "ssh-ed25519", 11);
        ck(good, "an Ed25519 match only with its base64 behind ssh-ed25519", it);
    }
}

// --- Ed25519 signatures and logins -----------------------------------------------------
//
// A verify is tens of milliseconds under the sanitizers, so these run a
// fiftieth of the iterations, like the targets that run a key exchange; about
// a quarter of the damaged inputs still parse far enough to reach the verify.
// Both promises are the same: whatever is accepted is exactly what was signed.

static void fuzz_ed25519(uint32_t iters) {
    uint8_t k[64], s[128], d[128];
    uint32_t kn = unhex(kED_KEY, k, sizeof(k)), sn = unhex(kED_SIG, s, sizeof(s));
    uint32_t dn = unhex(kED_DATA, d, sizeof(d));
    // A target that never accepts anything proves nothing about what it accepts.
    ck(ssh_sig_verify(k, kn, s, sn, d, dn, "ssh-ed25519"), "the Ed25519 seed verifies", 0);
    const Bytes K(k, k + kn), S(s, s + sn), D(d, d + dn);
    for (uint32_t it = 0; it < iters; it++) {
        Bytes k2 = K, s2 = S, d2 = D;
        switch (below(3)) {
        case 0:  mutate(k2, 256); break;
        case 1:  mutate(s2, 256); break;
        default: mutate(d2, 256); break;
        }
        bool ok = ssh_sig_verify(k2.data(), (uint32_t)k2.size(), s2.data(), (uint32_t)s2.size(),
                                 d2.data(), (uint32_t)d2.size(), "ssh-ed25519");
        ck(!ok || (k2 == K && s2 == S && d2 == D), "an Ed25519 signature verifies only as it was made", it);
    }

    // A signed ssh-ed25519 userauth request (ssh_vectors.inc), delivered to a
    // server whose session id is the one it was signed for.
    uint8_t blob[64], sig[128];
    uint32_t bn = unhex(kED_UA_BLOB, blob, sizeof(blob)), usn = unhex(kED_UA_SIG, sig, sizeof(sig));
    Bytes req;
    putc_(req, "alice"); putc_(req, "ssh-connection"); putc_(req, "publickey");
    req.push_back(1); putc_(req, "ssh-ed25519"); putstr(req, blob, bn); putstr(req, sig, usn);
    for (uint32_t it = 0; it <= iters; it++) {
        Bytes m = req;
        if (it) mutate(m, 1024);            // the first one as it was signed
        SshServer t;
        ssh_srv_init(&t, &g_hk, &kOps, nullptr);
        t.c.state = SSH_ST_OPEN;
        t.c.first_kex_done = true;
        t.c.have_session_id = true;
        for (int i = 0; i < 32; i++) t.c.session_id[i] = (uint8_t)i;
        t.service_ok = true;
        SshR r; sshr_init(&r, m.data(), (uint32_t)m.size());
        t.c.dispatch(&t.c, SSH_MSG_USERAUTH_REQUEST, &r);
        if (!it) ck(t.authed, "the signed Ed25519 request logs in", it);
        else     ck(!t.authed || m == req, "an Ed25519 login only from the request as signed", it);
        ssh_srv_free(&t);
    }
}

int main(void) {
    const char *s = getenv("RPC_FUZZ_SEED");
    g_rng = s ? strtoull(s, nullptr, 10) * 0x9E3779B97F4A7C15ull + 1 : 0x5348u;
    const char *i = getenv("RPC_FUZZ_ITERS");
    uint32_t iters = i ? (uint32_t)strtoul(i, nullptr, 10) : 20000;
    ssh_hostkey_generate(&g_hk);
    ssh_hostkey_generate(&g_uk);
    // The two that run a key agreement per surviving input are the slow ones.
    fuzz_prekex(iters / 20 ? iters / 20 : 1);
    fuzz_onwire(iters / 100 ? iters / 100 : 1);
    fuzz_server(iters);
    fuzz_client(iters);
    fuzz_keys(iters);
    fuzz_ed25519(iters / 50 ? iters / 50 : 1);
    printf("  ssh fuzz: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
