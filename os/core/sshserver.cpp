#include "sshserver.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// RFC 4254 section 5.1 reason codes.
#define OPEN_ADMIN_PROHIBITED 1
#define OPEN_UNKNOWN_TYPE     3

static SshServer *of(SshConn *c) { return (SshServer *)c->owner; }

static bool streq(const uint8_t *d, uint32_t n, const char *s) {
    return n == strlen(s) && memcmp(d, s, n) == 0;
}

static void auth_failure(SshServer *s) {
    SshW *w = ssh_begin(&s->c, SSH_MSG_USERAUTH_FAILURE);
    sshw_cstr(w, "publickey,password");
    sshw_bool(w, false);
    ssh_end(&s->c);
}

// A refused attempt. Counted, logged by the caller, and then either a pause
// before the answer or, past the limit, the end of the connection.
static void refused(SshServer *s, const char *user, const char *method, bool slow) {
    s->fails++;
    if (s->ops->auth_log) s->ops->auth_log(s->ctx, user, method, false);
    if (s->fails >= SSH_SRV_MAX_FAILS) {
        ssh_disconnect(&s->c, SSH_DISC_NO_MORE_AUTH_METHODS, "too many authentication failures");
        return;
    }
    // A wrong password costs a second, growing to four; a key the server does
    // not know costs nothing, because a client with several keys in an agent
    // offers each in turn and that is not guessing.
    if (slow) s->penalty_ms = 1000u * (s->fails > 4 ? 4 : s->fails);
    auth_failure(s);
}

static void succeeded(SshServer *s, const char *user, const char *method) {
    snprintf(s->user, sizeof(s->user), "%s", user);
    s->authed = true;
    if (s->ops->auth_log) s->ops->auth_log(s->ctx, user, method, true);
    SshW *w = ssh_begin(&s->c, SSH_MSG_USERAUTH_SUCCESS);
    (void)w;
    ssh_end(&s->c);
}

// A name that could be an account: short, printable, no separators that
// would let it climb out of /home/<name>.
static bool name_sane(const char *u) {
    size_t n = strlen(u);
    if (n == 0 || n > 24) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = u[i];
        if (ch <= ' ' || ch >= 127 || ch == '/' || ch == '\\') return false;
    }
    return strcmp(u, ".") != 0 && strcmp(u, "..") != 0;
}

static void on_userauth(SshServer *s, SshR *r) {
    // RFC 4252 section 5.1: requests after success are ignored.
    if (s->authed) return;
    char user[64], service[32], method[32];
    sshr_cstr(r, user, sizeof(user));
    sshr_cstr(r, service, sizeof(service));
    sshr_cstr(r, method, sizeof(method));
    if (r->err) { ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "bad userauth request"); return; }
    if (strcmp(service, "ssh-connection") != 0) {
        ssh_disconnect(&s->c, SSH_DISC_SERVICE_NOT_AVAILABLE, "only ssh-connection");
        return;
    }
    bool user_valid = name_sane(user) && s->ops->user_ok && s->ops->user_ok(s->ctx, user);

    if (!strcmp(method, "none")) { auth_failure(s); return; }

    if (!strcmp(method, "password")) {
        bool change = sshr_bool(r);
        char pw[128];
        sshr_cstr(r, pw, sizeof(pw));
        if (r->err) { ssh_wipe(pw, sizeof(pw)); ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "bad password request"); return; }
        // A password CHANGE is not offered over the wire; passwd on the
        // device is the way to do that.
        bool ok = !change && user_valid && s->ops->password && s->ops->password(s->ctx, user, pw);
        ssh_wipe(pw, sizeof(pw));
        if (ok) succeeded(s, user, "password");
        else    refused(s, user, "password", true);
        return;
    }

    if (!strcmp(method, "publickey")) {
        bool has_sig = sshr_bool(r);
        char alg[32];
        sshr_cstr(r, alg, sizeof(alg));
        const uint8_t *blob; uint32_t bn;
        sshr_string(r, &blob, &bn);
        const uint8_t *sig = nullptr; uint32_t sn = 0;
        if (has_sig) sshr_string(r, &sig, &sn);
        if (r->err || sshr_left(r) != 0) {
            ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "bad publickey request"); return;
        }
        char type[24];
        bool alg_ok = ssh_key_type(blob, bn, type, sizeof(type)) &&
            ((!strcmp(type, "ecdsa-sha2-nistp256") && !strcmp(alg, "ecdsa-sha2-nistp256")) ||
             (!strcmp(type, "ssh-rsa") && (!strcmp(alg, "rsa-sha2-256") || !strcmp(alg, "rsa-sha2-512"))) ||
             (!strcmp(type, "ssh-ed25519") && !strcmp(alg, "ssh-ed25519")));
        bool listed = alg_ok && user_valid && s->ops->pubkey && s->ops->pubkey(s->ctx, user, blob, bn);
        if (!listed) { refused(s, user, "publickey", false); return; }
        if (!has_sig) {
            // RFC 4252 section 7: "this key would do", before the client signs.
            SshW *w = ssh_begin(&s->c, SSH_MSG_USERAUTH_PK_OK);
            sshw_cstr(w, alg);
            sshw_string(w, blob, bn);
            ssh_end(&s->c);
            return;
        }
        // What the client signed: the session identifier, then this very
        // request up to the signature (RFC 4252 section 7).
        uint32_t cap = 32 + bn + 160;
        uint8_t *data = (uint8_t *)malloc(cap);
        if (!data) { ssh_disconnect(&s->c, SSH_DISC_BY_APPLICATION, "out of memory"); return; }
        SshW w; sshw_init(&w, data, cap);
        sshw_string(&w, s->c.session_id, 32);
        sshw_u8(&w, SSH_MSG_USERAUTH_REQUEST);
        sshw_cstr(&w, user);
        sshw_cstr(&w, service);
        sshw_cstr(&w, "publickey");
        sshw_bool(&w, true);
        sshw_cstr(&w, alg);
        sshw_string(&w, blob, bn);
        // An ssh-ed25519 check is the slow one, about half a second on a Pico
        // 2 W (ed25519.cpp), and it runs on the server's task without yielding;
        // the stall guard's first warning is at 3 s. Only a key the account
        // lists gets this far. DEVICE-UNCONFIRMED: the half second is counted
        // on a model of the core, not timed on a board.
        bool ok = !w.err && ssh_sig_verify(blob, bn, sig, sn, data, w.len, alg);
        free(data);
        if (ok) succeeded(s, user, "publickey");
        else    refused(s, user, "publickey", true);
        return;
    }

    // keyboard-interactive, hostbased, gssapi: not offered.
    auth_failure(s);
}

// --- channels ---------------------------------------------------------------------

static void chan_reply(SshServer *s, bool want, bool ok) {
    if (!want) return;
    SshW *w = ssh_begin(&s->c, ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
    sshw_u32(w, s->peer_id);
    ssh_end(&s->c);
}

static void on_open(SshServer *s, SshR *r) {
    const uint8_t *type; uint32_t tn;
    sshr_string(r, &type, &tn);
    uint32_t sender = sshr_u32(r), window = sshr_u32(r), maxpkt = sshr_u32(r);
    if (r->err) { ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "bad channel open"); return; }
    bool session = streq(type, tn, "session");
    if (!session || s->chan) {
        // Forwarding of every kind, and a second session on one connection.
        SshW *w = ssh_begin(&s->c, SSH_MSG_CHANNEL_OPEN_FAILURE);
        sshw_u32(w, sender);
        sshw_u32(w, session ? OPEN_ADMIN_PROHIBITED : OPEN_UNKNOWN_TYPE);
        sshw_cstr(w, session ? "one session per connection" : "not supported");
        sshw_cstr(w, "");
        ssh_end(&s->c);
        return;
    }
    s->chan = true;
    s->peer_id = sender;
    s->peer_window = window;
    s->peer_maxpkt = maxpkt;
    s->local_window = SSH_SRV_WINDOW;
    SshW *w = ssh_begin(&s->c, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
    sshw_u32(w, sender);
    sshw_u32(w, 0);                 // our channel number: there is only one
    sshw_u32(w, SSH_SRV_WINDOW);
    sshw_u32(w, SSH_SRV_MAXPKT);
    ssh_end(&s->c);
}

static void on_request(SshServer *s, SshR *r) {
    char type[40];
    sshr_cstr(r, type, sizeof(type));
    bool want = sshr_bool(r);
    if (r->err) { ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "bad channel request"); return; }

    if (!strcmp(type, "pty-req")) {
        char term[32];
        sshr_cstr(r, term, sizeof(term));
        uint32_t cols = sshr_u32(r), rows = sshr_u32(r);
        sshr_u32(r); sshr_u32(r);
        const uint8_t *modes; uint32_t mn;
        sshr_string(r, &modes, &mn);
        if (r->err || s->mode != SSH_MODE_NONE) { chan_reply(s, want, false); return; }
        s->pty = true;
        snprintf(s->term, sizeof(s->term), "%s", term);
        s->cols = cols; s->rows = rows;
        chan_reply(s, want, true);
        return;
    }
    if (!strcmp(type, "window-change")) {
        uint32_t cols = sshr_u32(r), rows = sshr_u32(r);
        if (!r->err) {
            s->cols = cols; s->rows = rows;
            if (s->ops->on_winch) s->ops->on_winch(s->ctx);
        }
        chan_reply(s, want, !r->err);
        return;
    }
    if (!strcmp(type, "shell") || !strcmp(type, "exec")) {
        bool exec = type[0] == 'e';
        if (exec) sshr_cstr(r, s->exec, sizeof(s->exec));
        if (r->err || s->mode != SSH_MODE_NONE) { chan_reply(s, want, false); return; }
        s->mode = exec ? SSH_MODE_EXEC : SSH_MODE_SHELL;
        chan_reply(s, want, true);
        if (s->ops->on_start) s->ops->on_start(s->ctx);
        return;
    }
    if (!strcmp(type, "signal")) {
        char name[16];
        sshr_cstr(r, name, sizeof(name));
        if (!r->err && s->ops->on_signal) s->ops->on_signal(s->ctx, name);
        return;                     // RFC 4254 6.9: no reply
    }
    // env, subsystem (sftp), x11-req, auth-agent-req, break, and the OpenSSH
    // extensions: refused.
    chan_reply(s, want, false);
}

static bool channel_msg(SshServer *s, uint8_t type, SshR *r) {
    uint32_t id = sshr_u32(r);
    if (r->err || !s->chan || id != 0 || s->close_in) {
        ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "message for a channel that is not open");
        return true;
    }
    switch (type) {
    case SSH_MSG_CHANNEL_WINDOW_ADJUST: {
        uint32_t add = sshr_u32(r);
        // RFC 4254 5.2: the window may not exceed 2^32 - 1 bytes.
        if (r->err || add > 0xFFFFFFFFu - s->peer_window) {
            ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "window overflow"); return true;
        }
        s->peer_window += add;
        return true;
    }
    case SSH_MSG_CHANNEL_DATA:
    case SSH_MSG_CHANNEL_EXTENDED_DATA: {
        if (type == SSH_MSG_CHANNEL_EXTENDED_DATA) sshr_u32(r);
        const uint8_t *d; uint32_t n;
        sshr_string(r, &d, &n);
        if (r->err) { ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "bad channel data"); return true; }
        // More than the window allows is a broken or hostile peer, and the
        // window is all that guarantees the input ring has room.
        if (n > s->local_window || s->eof_in) {
            ssh_disconnect(&s->c, SSH_DISC_PROTOCOL_ERROR, "data beyond the window"); return true;
        }
        s->local_window -= n;
        if (type == SSH_MSG_CHANNEL_DATA && s->ops->on_data) s->ops->on_data(s->ctx, d, n);
        else ssh_srv_consumed(s, n);                // stderr from a client: dropped
        return true;
    }
    case SSH_MSG_CHANNEL_EOF:
        s->eof_in = true;
        if (s->ops->on_eof) s->ops->on_eof(s->ctx);
        return true;
    case SSH_MSG_CHANNEL_CLOSE:
        s->close_in = true;
        if (!s->close_out) {
            SshW *w = ssh_begin(&s->c, SSH_MSG_CHANNEL_CLOSE);
            sshw_u32(w, s->peer_id);
            ssh_end(&s->c);
            s->close_out = true;
        }
        if (s->ops->on_close) s->ops->on_close(s->ctx);
        return true;
    case SSH_MSG_CHANNEL_REQUEST:
        on_request(s, r);
        return true;
    case SSH_MSG_CHANNEL_SUCCESS:
    case SSH_MSG_CHANNEL_FAILURE:
        return true;                // nothing is ever asked of the client
    }
    return false;
}

static bool dispatch(SshConn *c, uint8_t type, SshR *r) {
    SshServer *s = of(c);
    if (type == 0) return true;     // transport ready: the client speaks next
    if (type == SSH_MSG_SERVICE_REQUEST) {
        char name[32];
        sshr_cstr(r, name, sizeof(name));
        if (r->err || strcmp(name, "ssh-userauth") != 0 || s->service_ok) {
            ssh_disconnect(c, SSH_DISC_SERVICE_NOT_AVAILABLE, "service not available");
            return true;
        }
        s->service_ok = true;
        SshW *w = ssh_begin(c, SSH_MSG_SERVICE_ACCEPT);
        sshw_cstr(w, "ssh-userauth");
        ssh_end(c);
        return true;
    }
    if (type == SSH_MSG_EXT_INFO) return true;       // nothing a client says here matters
    if (!s->service_ok) {
        ssh_disconnect(c, SSH_DISC_PROTOCOL_ERROR, "no service requested");
        return true;
    }
    if (type == SSH_MSG_USERAUTH_REQUEST) { on_userauth(s, r); return true; }
    // RFC 4252 section 6: nothing from the connection protocol until the
    // client has authenticated.
    if (!s->authed) {
        if (type >= 50 && type <= 79) return false;   // other userauth numbers: unimplemented
        ssh_disconnect(c, SSH_DISC_PROTOCOL_ERROR, "not authenticated");
        return true;
    }
    if (type == SSH_MSG_GLOBAL_REQUEST) {
        char name[64];
        sshr_cstr(r, name, sizeof(name));
        bool want = sshr_bool(r);
        // tcpip-forward, keepalive@openssh.com, no-more-sessions: none granted.
        if (want) { SshW *w = ssh_begin(c, SSH_MSG_REQUEST_FAILURE); (void)w; ssh_end(c); }
        return true;
    }
    if (type == SSH_MSG_CHANNEL_OPEN) { on_open(s, r); return true; }
    if (type >= SSH_MSG_CHANNEL_WINDOW_ADJUST && type <= SSH_MSG_CHANNEL_FAILURE)
        return channel_msg(s, type, r);
    return false;
}

bool ssh_srv_init(SshServer *s, const SshHostKey *hostkey, const SshServerOps *ops, void *ctx) {
    memset(s, 0, sizeof(*s));
    s->ops = ops;
    s->ctx = ctx;
    if (!ssh_conn_init(&s->c, true, "RPCortex_2")) return false;
    s->c.hostkey = hostkey;
    s->c.dispatch = dispatch;
    s->c.owner = s;
    return true;
}

void ssh_srv_free(SshServer *s) { ssh_conn_free(&s->c); }

uint32_t ssh_srv_send(SshServer *s, const uint8_t *d, uint32_t n) {
    uint32_t sent = 0;
    while (sent < n && s->chan && !s->close_in && !s->eof_out) {
        uint32_t take = n - sent;
        if (take > s->peer_window) take = s->peer_window;
        if (take > s->peer_maxpkt) take = s->peer_maxpkt;
        if (take > SSH_SRV_CHUNK) take = SSH_SRV_CHUNK;
        if (take == 0 || !ssh_can_send(&s->c, take + 16)) break;
        SshW *w = ssh_begin(&s->c, SSH_MSG_CHANNEL_DATA);
        sshw_u32(w, s->peer_id);
        sshw_string(w, d + sent, take);
        if (!ssh_end(&s->c)) break;
        s->peer_window -= take;
        sent += take;
    }
    return sent;
}

void ssh_srv_consumed(SshServer *s, uint32_t n) {
    s->regrant += n;
    if (!s->chan || s->close_in || s->eof_in) return;
    // Half the window at a time, or everything once it has run dry.
    if (s->regrant < SSH_SRV_WINDOW / 2 && s->local_window > 0) return;
    if (!ssh_open(&s->c)) return;
    SshW *w = ssh_begin(&s->c, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    sshw_u32(w, s->peer_id);
    sshw_u32(w, s->regrant);
    if (ssh_end(&s->c)) { s->local_window += s->regrant; s->regrant = 0; }
}

bool ssh_srv_exit(SshServer *s, uint32_t status) {
    if (!s->chan || s->close_out) return true;
    if (!ssh_can_send(&s->c, 128)) return false;
    if (!s->close_in) {
        SshW *w = ssh_begin(&s->c, SSH_MSG_CHANNEL_REQUEST);
        sshw_u32(w, s->peer_id);
        sshw_cstr(w, "exit-status");
        sshw_bool(w, false);
        sshw_u32(w, status);
        ssh_end(&s->c);
        if (!s->eof_out) {
            w = ssh_begin(&s->c, SSH_MSG_CHANNEL_EOF);
            sshw_u32(w, s->peer_id);
            ssh_end(&s->c);
            s->eof_out = true;
        }
    }
    SshW *w = ssh_begin(&s->c, SSH_MSG_CHANNEL_CLOSE);
    sshw_u32(w, s->peer_id);
    ssh_end(&s->c);
    s->close_out = true;
    return true;
}
