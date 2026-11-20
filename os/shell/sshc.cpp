// `ssh`: the client, and the command the server is driven from.
//
//   ssh [-p port] [-T] user@host [command]   log in to another machine
//   ssh keygen                               this user's own key, for authorized_keys elsewhere
//   ssh pubkey                               print it again
//   ssh server status|on|off|fingerprint|port <n>|rekey     (sshd.cpp)
//
// The protocol is core/sshclient.cpp, tested on the host against a real
// OpenSSH sshd. What is here is the terminal: keys from the console (or from
// the remote terminal, when this is run inside an SSH session) go to the
// channel, what comes back goes to the screen, until the remote side ends or
// Ctrl+] is pressed. DEVICE-UNCONFIRMED: built for a Pico 2 W, not yet run on one.
//
// Trust on first use, as OpenSSH does it: the first time a host is seen its
// key's fingerprint is shown and the connection goes on only after a typed
// "yes"; the key is then kept in ~/.ssh/known_hosts and a different one later
// stops the connection before anything — a password above all — is sent.
#include "command.h"
#include "out.h"
#include "storage.h"
#include "session.h"
#include "fsguard.h"
#include "task.h"
#include "interrupt.h"
#include "logring.h"
#include "nettcp.h"

#include "sshclient.h"
#include "sshkeys.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

bool net_is_connected(void);
int  sshd_command(int argc, char **argv);   // sshd.cpp

#define CTRL_RIGHT_BRACKET 0x1d

struct Cli {
    SshClient k;
    char      host[64];
    uint16_t  port;
    char      kh_path[TASK_USER_MAX + 32];
    SshHostKey idkey;
};

static void user_ssh_dir(char *out, unsigned cap) {
    snprintf(out, cap, "/home/%s/.ssh", session_user());
}

// --- the host key --------------------------------------------------------------------

static bool op_hostkey(void *ctx, const uint8_t *blob, uint32_t n) {
    Cli *c = (Cli *)ctx;
    char fp[64];
    ssh_fingerprint(blob, n, fp, sizeof(fp));
    char type[24];
    ssh_key_type(blob, n, type, sizeof(type));
    const char *kind = !strcmp(type, "ssh-rsa") ? "RSA" : !strcmp(type, "ssh-ed25519") ? "ED25519" : "ECDSA";

    char *text = (char *)malloc(8192);
    uint32_t len = text ? storage_read_file(c->kh_path, (uint8_t *)text, 8192) : 0;
    SshKnown k = text ? ssh_known_hosts_check(text, len, c->host, c->port, blob, n) : SSH_KH_UNKNOWN;
    free(text);

    if (k == SSH_KH_MATCH) return true;
    if (k == SSH_KH_CHANGED) {
        out_err("THE HOST KEY OF %s HAS CHANGED.", c->host);
        out_multi("  Somebody may be in the middle of this connection, or the host was");
        out_multi("  reinstalled. It now presents %s %s.", kind, fp);
        out_multi("  Nothing was sent. If the change is expected, remove the line for");
        out_multi("  %s from %s and connect again.", c->host, c->kh_path);
        log_addf(LOG_K_WARN, "ssh: host key for %s changed - refused", c->host);
        return false;
    }
    out_warn("The authenticity of host '%s' can't be established.", c->host);
    out_multi("  %s key fingerprint is %s.", kind, fp);
    if (!session_confirm("Are you sure you want to continue connecting")) return false;

    // On the heap: an RSA host key's line is over a kilobyte, and this runs
    // inside the protocol, a few frames under the shell's own.
    const uint32_t cap = 1600;
    char *line = (char *)malloc(cap);
    uint32_t ln = line ? ssh_known_hosts_line(c->host, c->port, blob, n, line, cap) : 0;
    char dir[TASK_USER_MAX + 16];
    user_ssh_dir(dir, sizeof(dir));
    // Written as the user would write it, so the guard on other people's ~/.ssh
    // applies here too.
    if (ln && fs_may_change(c->kh_path)) {
        storage_mkdir(dir);
        if (storage_append_file(c->kh_path, (const uint8_t *)line, ln))
            out_info("Added '%s' to the list of known hosts.", c->host);
    }
    free(line);
    return true;
}

static bool op_prompt(void *ctx, const char *prompt, bool echo, char *out, uint32_t cap) {
    Cli *c = (Cli *)ctx;
    char p[128];
    if (!strcmp(prompt, "Password")) snprintf(p, sizeof(p), "%s@%s's password", c->k.user, c->host);
    else                             snprintf(p, sizeof(p), "%s", prompt);
    session_prompt(p, out, cap, !echo);
    // Nothing typed, because the session was interrupted or is ending: give up
    // rather than send an empty password three times.
    if (!out[0] && (intr_pending() || task_should_stop())) return false;
    return true;
}

static void op_banner(void *, const char *text) { out_multi("%s", text); }

static void op_data(void *, const uint8_t *d, uint32_t n, bool) {
    out_write((const char *)d, n);
}

static const SshClientOps kOps = { op_hostkey, op_prompt, op_banner, op_data };

// --- the device's own key ------------------------------------------------------------

static bool load_idkey(SshHostKey *k) {
    char path[TASK_USER_MAX + 32];
    snprintf(path, sizeof(path), "/home/%s/.ssh/id_ecdsa", session_user());
    uint8_t d[32];
    bool ok = storage_read_file(path, d, sizeof(d)) == sizeof(d) && ssh_hostkey_load(k, d);
    ssh_wipe(d, sizeof(d));
    return ok;
}

static void print_pubkey(const SshHostKey *k) {
    uint8_t blob[160];
    uint32_t bn = ssh_hostkey_blob(k, blob, sizeof(blob));
    char comment[64];
    snprintf(comment, sizeof(comment), "%s@rpcortex", session_user());
    char line[300];
    if (ssh_pubkey_line(blob, bn, comment, line, sizeof(line))) out_multi("%s", line);
}

static int cmd_keygen(void) {
    char dir[TASK_USER_MAX + 16], path[TASK_USER_MAX + 32];
    user_ssh_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/id_ecdsa", dir);
    if (!fs_allow_change(path)) return 1;
    bool is_dir = false;
    if (storage_stat(path, &is_dir, nullptr)) {
        out_warn("You already have a key in %s.", path);
        if (!session_confirm("Replace it? Hosts that trust the old one will stop")) { out_info("Kept."); return 1; }
    }
    static SshHostKey k;
    if (!ssh_hostkey_generate(&k)) { out_err("Could not make a key."); return 1; }
    storage_mkdir(dir);
    bool ok = storage_write_file(path, k.d, sizeof(k.d));
    if (ok) {
        out_ok("New ECDSA key in %s. The public half, for authorized_keys elsewhere:", path);
        print_pubkey(&k);
    } else {
        out_err("Could not write %s.", path);
    }
    ssh_wipe(&k, sizeof(k));
    return ok ? 0 : 1;
}

// --- a connection ----------------------------------------------------------------------

static bool send_out(Cli *c, int sock) {
    uint32_t n;
    const uint8_t *p = ssh_out(&c->k.c, &n);
    while (n) {
        int w = net_tcp_write_now(sock, p, n);
        if (w < 0) return false;
        if (w == 0) return true;            // no room yet; next turn
        ssh_out_consume(&c->k.c, (uint32_t)w);
        p = ssh_out(&c->k.c, &n);
    }
    return true;
}

static int connect_and_run(const char *user, const char *host, uint16_t port, const char *command,
                           bool want_pty) {
    if (!net_is_connected()) { out_err("No network. Connect with 'wifi connect <ssid>' first."); return 1; }
    Cli *c = (Cli *)calloc(1, sizeof(Cli));
    if (!c) { out_err("Not enough memory for an SSH connection."); return 1; }
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    snprintf(c->kh_path, sizeof(c->kh_path), "/home/%s/.ssh/known_hosts", session_user());

    out_info("Connecting to %s port %u...", host, (unsigned)port);
    int sock = net_tcp_connect(host, port, 10000);
    if (sock < 0) { out_err("Could not connect to %s:%u.", host, (unsigned)port); free(c); return 1; }
    net_tcp_adopt(sock);

    if (!ssh_cli_init(&c->k, user, &kOps, c)) {
        net_pkg_tcp_close(sock);
        free(c);
        out_err("Not enough memory for an SSH connection.");
        return 1;
    }
    if (load_idkey(&c->idkey)) c->k.idkey = &c->idkey;
    c->k.want_pty = want_pty;
    c->k.command = command;
    snprintf(c->k.term, sizeof(c->k.term), "xterm");
    c->k.cols = 80; c->k.rows = 24;
    // Inside a remote session the terminal says how big it is.
    if (const TaskTerm *t = task_shell()->term) {
        uint16_t cols, rows;
        if (t->size && t->size(t->ctx, &cols, &rows)) { c->k.cols = cols; c->k.rows = rows; }
    }

    // The console's keys are this command's until it ends: without the claim,
    // any task polling for Ctrl+C would take keystrokes meant for the far end.
    // Only the CONSOLE needs claiming: run inside a remote session, the keys come
    // from that session's own ring, and claiming the console from there would
    // stop the person at the serial port from interrupting their own commands.
    bool claimed = !task_shell()->term && intr_input_claim();
    bool shown_hint = false;
    uint8_t keys[64];
    uint32_t nkeys = 0;
    bool quit = false, lost = false;
    for (;;) {
        bool busy = false;
        // Socket in.
        for (int i = 0; i < 4; i++) {
            uint32_t room = c->k.c.in_cap - c->k.c.in_len;
            if (!room) break;
            uint8_t buf[512];
            int n = net_tcp_read_now(sock, buf, room < sizeof(buf) ? room : sizeof(buf));
            if (n < 0) { lost = true; break; }
            if (n == 0) break;
            ssh_feed(&c->k.c, buf, (uint32_t)n);
            busy = true;
        }
        ssh_process(&c->k.c);
        if (c->k.stage == SSH_CLI_RUNNING && !shown_hint && want_pty) {
            shown_hint = true;
            out_info("Connected. Ctrl+] ends the connection from this side.");
        }
        // Keys out. Ctrl+] is ours; everything else, Ctrl+C included, is the far end's.
        if (c->k.stage == SSH_CLI_RUNNING) {
            while (nkeys < sizeof(keys)) {
                int ch = intr_getkey();
                if (ch < 0) break;
                if (ch == CTRL_RIGHT_BRACKET) { quit = true; break; }
                keys[nkeys++] = (uint8_t)ch;
            }
            // A Ctrl+C caught by somebody's intr_check on this task: pass it on.
            if (intr_pending() && nkeys < sizeof(keys)) { keys[nkeys++] = 0x03; intr_clear(); }
            if (nkeys) {
                uint32_t sent = ssh_cli_send(&c->k, keys, nkeys);
                if (sent) { memmove(keys, keys + sent, nkeys - sent); nkeys -= sent; busy = true; }
            }
        } else if (intr_pending() || task_should_stop()) {
            quit = true;                    // Ctrl+C while still connecting
        }
        if (quit) { ssh_cli_close(&c->k); quit = false; ssh_disconnect(&c->k.c, SSH_DISC_BY_APPLICATION, "closed by user"); }
        if (!send_out(c, sock)) lost = true;
        uint32_t left;
        ssh_out(&c->k.c, &left);
        if (lost || (ssh_closed(&c->k.c) && !left)) break;
        if (task_should_stop()) break;
        if (!busy) task_sleep_ms(5);
    }
    // Let the last packet (a DISCONNECT) leave before the socket goes.
    for (int i = 0; i < 50 && !lost && !net_tcp_drained(sock); i++) task_sleep_ms(10);
    net_pkg_tcp_close(sock);
    if (claimed) intr_input_release();

    int rc = c->k.have_status ? c->k.exit_status : 255;
    if (c->k.failed[0])      out_err("%s: %s", host, c->k.failed);
    else if (lost && !c->k.have_status) out_err("Connection to %s lost.", host);
    else if (!c->k.have_status && c->k.c.why[0] && strcmp(c->k.c.why, "closed by user"))
        out_err("%s: %s", host, c->k.c.why);
    else if (want_pty) out_info("Connection to %s closed.", host);
    ssh_cli_free(&c->k);
    ssh_wipe(c, sizeof(*c));
    free(c);
    return rc;
}

static int cmd_ssh(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "server")) return sshd_command(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "keygen")) return cmd_keygen();
    if (argc >= 2 && !strcmp(argv[1], "pubkey")) {
        static SshHostKey k;
        if (!load_idkey(&k)) { out_info("No key yet. 'ssh keygen' makes one."); return 1; }
        print_pubkey(&k);
        ssh_wipe(&k, sizeof(k));
        return 0;
    }

    uint16_t port = 22;
    bool force_no_pty = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            long p = strtol(argv[++i], nullptr, 10);
            if (p < 1 || p > 65535) { out_err("A port is 1 to 65535."); return 1; }
            port = (uint16_t)p;
        } else if (!strcmp(argv[i], "-T")) {
            force_no_pty = true;
        } else {
            out_err("Unknown option %s.", argv[i]);
            return 1;
        }
    }
    const char *target = i < argc ? argv[i] : nullptr;
    const char *at = target ? strchr(target, '@') : nullptr;
    if (!target || !at || at == target || !at[1]) {
        out_multi("Usage: ssh [-p port] [-T] user@host [command]");
        out_multi("       ssh keygen | pubkey");
        out_multi("       ssh server status|on|off|fingerprint|port <n>|rekey");
        out_multi("  Ctrl+] ends a connection from this side.");
        return 1;
    }
    char user[33];
    snprintf(user, sizeof(user), "%.*s", (int)(at - target), target);
    char host[64];
    snprintf(host, sizeof(host), "%s", at + 1);
    // The rest of the line is the command, as OpenSSH joins it.
    char command[256];
    command[0] = 0;
    for (int k = i + 1; k < argc; k++) {
        if (k > i + 1) strncat(command, " ", sizeof(command) - strlen(command) - 1);
        strncat(command, argv[k], sizeof(command) - strlen(command) - 1);
    }
    bool has_cmd = command[0] != 0;
    return connect_and_run(user, host, port, has_cmd ? command : nullptr, !has_cmd && !force_no_pty);
}

void ssh_register(void) {
    static const Command c{"ssh", "log in to another machine; 'ssh server' for this one", cmd_ssh, nullptr};
    cmd_register(&c);
}
