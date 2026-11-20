#include "sshclient.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static SshClient *of(SshConn *c) { return (SshClient *)c->owner; }

// Text from the server goes to a terminal, so control characters come out
// (all but newline and tab): a hostile server must not be able to move the
// cursor, retitle the window or rewrite what is on screen through a banner or
// a password prompt. OpenSSH does the same with strnvis.
static void sanitize(char *s) {
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch < 32 && ch != '\n' && ch != '\t') || ch == 127) *s = '?';
    }
}

static void give_up(SshClient *k, uint32_t reason, const char *why) {
    if (!k->failed[0]) snprintf(k->failed, sizeof(k->failed), "%s", why);
    k->stage = SSH_CLI_DONE;
    ssh_disconnect(&k->c, reason, why);
}

static void userauth_head(SshW *w, SshClient *k, const char *method) {
    sshw_cstr(w, k->user);
    sshw_cstr(w, "ssh-connection");
    sshw_cstr(w, method);
}

static bool send_pubkey(SshClient *k) {
    uint8_t blob[160];
    uint32_t bn = ssh_hostkey_blob(k->idkey, blob, sizeof(blob));
    if (!bn) return false;
    // What gets signed: RFC 4252 section 7.
    uint8_t data[400];
    SshW d; sshw_init(&d, data, sizeof(data));
    sshw_string(&d, k->c.session_id, 32);
    sshw_u8(&d, SSH_MSG_USERAUTH_REQUEST);
    userauth_head(&d, k, "publickey");
    sshw_bool(&d, true);
    sshw_cstr(&d, "ecdsa-sha2-nistp256");
    sshw_string(&d, blob, bn);
    uint8_t sig[128];
    uint32_t sn = d.err ? 0 : ssh_hostkey_sign(k->idkey, data, d.len, sig, sizeof(sig));
    if (!sn) return false;
    SshW *w = ssh_begin(&k->c, SSH_MSG_USERAUTH_REQUEST);
    userauth_head(w, k, "publickey");
    sshw_bool(w, true);
    sshw_cstr(w, "ecdsa-sha2-nistp256");
    sshw_string(w, blob, bn);
    sshw_string(w, sig, sn);
    return ssh_end(&k->c);
}

static void next_auth(SshClient *k, const uint8_t *methods, uint32_t mn) {
    k->kbd_active = false;
    if (k->idkey && !k->tried_key && ssh_namelist_has(methods, mn, "publickey")) {
        k->tried_key = true;
        if (send_pubkey(k)) return;
    }
    if (ssh_namelist_has(methods, mn, "password") && k->pw_prompts < SSH_CLI_PROMPTS) {
        char pw[128];
        k->pw_prompts++;
        if (!k->ops->prompt || !k->ops->prompt(k->ctx, "Password", false, pw, sizeof(pw))) {
            ssh_wipe(pw, sizeof(pw));
            give_up(k, SSH_DISC_AUTH_CANCELLED, "authentication cancelled");
            return;
        }
        SshW *w = ssh_begin(&k->c, SSH_MSG_USERAUTH_REQUEST);
        userauth_head(w, k, "password");
        sshw_bool(w, false);
        sshw_cstr(w, pw);
        ssh_wipe(pw, sizeof(pw));
        ssh_end(&k->c);
        return;
    }
    if (ssh_namelist_has(methods, mn, "keyboard-interactive") && k->pw_prompts < SSH_CLI_PROMPTS) {
        k->kbd_active = true;
        SshW *w = ssh_begin(&k->c, SSH_MSG_USERAUTH_REQUEST);
        userauth_head(w, k, "keyboard-interactive");
        sshw_cstr(w, "");       // language
        sshw_cstr(w, "");       // submethods
        ssh_end(&k->c);
        return;
    }
    give_up(k, SSH_DISC_NO_MORE_AUTH_METHODS, "permission denied");
}

// RFC 4256 section 3.2: answer each prompt of an INFO_REQUEST.
static void on_info_request(SshClient *k, SshR *r) {
    char name[64], instr[128];
    sshr_cstr(r, name, sizeof(name));
    sshr_cstr(r, instr, sizeof(instr));
    const uint8_t *lang; uint32_t ln;
    sshr_string(r, &lang, &ln);
    uint32_t n = sshr_u32(r);
    if (r->err || n > 8) { give_up(k, SSH_DISC_PROTOCOL_ERROR, "bad keyboard-interactive request"); return; }
    sanitize(name); sanitize(instr);
    if (k->ops->banner) {
        if (name[0])  k->ops->banner(k->ctx, name);
        if (instr[0]) k->ops->banner(k->ctx, instr);
    }
    if (n) k->pw_prompts++;
    // Answers are collected first, then sent, so a cancelled prompt part way
    // leaves nothing half-written in the output buffer.
    static const uint32_t kMax = 128;
    char *ans = (char *)calloc(n ? n : 1, kMax);
    if (!ans) { give_up(k, SSH_DISC_BY_APPLICATION, "out of memory"); return; }
    bool ok = true;
    for (uint32_t i = 0; i < n && ok; i++) {
        char prompt[96];
        sshr_cstr(r, prompt, sizeof(prompt));
        bool echo = sshr_bool(r);
        if (r->err) { ok = false; break; }
        sanitize(prompt);
        // Prompts usually end in ": ", which the device's own prompt adds.
        size_t pl = strlen(prompt);
        while (pl && (prompt[pl - 1] == ' ' || prompt[pl - 1] == ':')) prompt[--pl] = 0;
        ok = k->ops->prompt && k->ops->prompt(k->ctx, prompt[0] ? prompt : "Password", echo,
                                             ans + i * kMax, kMax);
    }
    if (ok) {
        SshW *w = ssh_begin(&k->c, 61);       // SSH_MSG_USERAUTH_INFO_RESPONSE
        sshw_u32(w, n);
        for (uint32_t i = 0; i < n; i++) sshw_cstr(w, ans + i * kMax);
        ssh_end(&k->c);
    }
    ssh_wipe(ans, (n ? n : 1) * kMax);
    free(ans);
    if (!ok) give_up(k, SSH_DISC_AUTH_CANCELLED, "authentication cancelled");
}

// --- the channel ------------------------------------------------------------------------

// The order replies come back in is the order the requests went: a pty
// first when one was asked for, then the shell or the command.
#define REPLY_PTY   1
#define REPLY_START 2

static void open_session(SshClient *k) {
    k->stage = SSH_CLI_OPENING;
    k->local_window = SSH_CLI_WINDOW;
    SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_OPEN);
    sshw_cstr(w, "session");
    sshw_u32(w, 0);
    sshw_u32(w, SSH_CLI_WINDOW);
    sshw_u32(w, SSH_CLI_MAXPKT);
    ssh_end(&k->c);
}

static void on_confirm(SshClient *k, SshR *r) {
    k->peer_id = sshr_u32(r);
    k->peer_window = sshr_u32(r);
    k->peer_maxpkt = sshr_u32(r);
    if (r->err || k->stage != SSH_CLI_OPENING) { give_up(k, SSH_DISC_PROTOCOL_ERROR, "bad channel confirmation"); return; }
    k->replies = 0;
    if (k->want_pty) {
        SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_REQUEST);
        sshw_u32(w, k->peer_id);
        sshw_cstr(w, "pty-req");
        sshw_bool(w, true);
        sshw_cstr(w, k->term[0] ? k->term : "xterm");
        sshw_u32(w, k->cols ? k->cols : 80);
        sshw_u32(w, k->rows ? k->rows : 24);
        sshw_u32(w, 0); sshw_u32(w, 0);
        static const uint8_t modes[] = { 0 };   // TTY_OP_END: the server's defaults
        sshw_string(w, modes, sizeof(modes));
        ssh_end(&k->c);
        k->replies = REPLY_PTY;
    }
    SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_REQUEST);
    sshw_u32(w, k->peer_id);
    if (k->command) {
        sshw_cstr(w, "exec");
        sshw_bool(w, true);
        sshw_cstr(w, k->command);
    } else {
        sshw_cstr(w, "shell");
        sshw_bool(w, true);
    }
    ssh_end(&k->c);
    k->replies = (uint8_t)(k->replies ? (REPLY_PTY | (REPLY_START << 2)) : REPLY_START);
    k->stage = SSH_CLI_RUNNING;
}

static void on_reply(SshClient *k, bool ok) {
    uint8_t kind = k->replies & 3;
    k->replies >>= 2;
    if (kind == REPLY_PTY && !ok && k->ops->banner)
        k->ops->banner(k->ctx, "The server would not allocate a terminal.");
    if (kind == REPLY_START && !ok) {
        give_up(k, SSH_DISC_BY_APPLICATION, k->command ? "the server refused the command"
                                                       : "the server refused a shell");
    }
}

static void send_close(SshClient *k) {
    if (k->close_out) return;
    SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_CLOSE);
    sshw_u32(w, k->peer_id);
    ssh_end(&k->c);
    k->close_out = true;
}

static bool channel_msg(SshClient *k, uint8_t type, SshR *r) {
    uint32_t id = sshr_u32(r);
    if (r->err || id != 0 || (k->stage != SSH_CLI_OPENING && k->stage != SSH_CLI_RUNNING) ||
        k->close_in) {
        give_up(k, SSH_DISC_PROTOCOL_ERROR, "message for a channel that is not open");
        return true;
    }
    switch (type) {
    case SSH_MSG_CHANNEL_OPEN_CONFIRMATION: on_confirm(k, r); return true;
    case SSH_MSG_CHANNEL_OPEN_FAILURE: {
        sshr_u32(r);
        char why[64]; sshr_cstr(r, why, sizeof(why));
        sanitize(why);
        char msg[96]; snprintf(msg, sizeof(msg), "the server refused a session: %s", why);
        give_up(k, SSH_DISC_BY_APPLICATION, msg);
        return true;
    }
    case SSH_MSG_CHANNEL_WINDOW_ADJUST: {
        uint32_t add = sshr_u32(r);
        if (r->err || add > 0xFFFFFFFFu - k->peer_window) {
            give_up(k, SSH_DISC_PROTOCOL_ERROR, "window overflow"); return true;
        }
        k->peer_window += add;
        return true;
    }
    case SSH_MSG_CHANNEL_DATA:
    case SSH_MSG_CHANNEL_EXTENDED_DATA: {
        bool err_stream = type == SSH_MSG_CHANNEL_EXTENDED_DATA;
        if (err_stream) sshr_u32(r);
        const uint8_t *d; uint32_t n;
        sshr_string(r, &d, &n);
        if (r->err || n > k->local_window) {
            give_up(k, SSH_DISC_PROTOCOL_ERROR, "data beyond the window"); return true;
        }
        k->local_window -= n;
        if (k->ops->on_data) k->ops->on_data(k->ctx, d, n, err_stream);
        // Written out already, so the window reopens at once, half at a time.
        k->regrant += n;
        if (k->regrant >= SSH_CLI_WINDOW / 2 && !k->close_out) {
            SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_WINDOW_ADJUST);
            sshw_u32(w, k->peer_id);
            sshw_u32(w, k->regrant);
            if (ssh_end(&k->c)) { k->local_window += k->regrant; k->regrant = 0; }
        }
        return true;
    }
    case SSH_MSG_CHANNEL_EOF:
        k->eof_in = true;
        return true;
    case SSH_MSG_CHANNEL_CLOSE:
        k->close_in = true;
        send_close(k);
        k->stage = SSH_CLI_DONE;
        ssh_disconnect(&k->c, SSH_DISC_BY_APPLICATION, "session finished");
        return true;
    case SSH_MSG_CHANNEL_SUCCESS: on_reply(k, true);  return true;
    case SSH_MSG_CHANNEL_FAILURE: on_reply(k, false); return true;
    case SSH_MSG_CHANNEL_REQUEST: {
        char name[40];
        sshr_cstr(r, name, sizeof(name));
        bool want = sshr_bool(r);
        if (!strcmp(name, "exit-status")) {
            uint32_t st = sshr_u32(r);
            if (!r->err) { k->exit_status = (int)st; k->have_status = true; }
        } else if (!strcmp(name, "exit-signal")) {
            k->exit_status = 255;
            k->have_status = true;
        } else if (want) {
            SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_FAILURE);
            sshw_u32(w, k->peer_id);
            ssh_end(&k->c);
        }
        return true;
    }
    }
    return false;
}

static bool dispatch(SshConn *c, uint8_t type, SshR *r) {
    SshClient *k = of(c);
    switch (type) {
    case 0: {
        k->stage = SSH_CLI_AUTH;
        SshW *w = ssh_begin(c, SSH_MSG_SERVICE_REQUEST);
        sshw_cstr(w, "ssh-userauth");
        ssh_end(c);
        return true;
    }
    case SSH_MSG_EXT_INFO: return true;
    case SSH_MSG_SERVICE_ACCEPT: {
        // "none" first: the answer is the list of methods the server takes.
        SshW *w = ssh_begin(c, SSH_MSG_USERAUTH_REQUEST);
        userauth_head(w, k, "none");
        ssh_end(c);
        return true;
    }
    case SSH_MSG_USERAUTH_BANNER: {
        char text[512];
        sshr_cstr(r, text, sizeof(text));
        if (!r->err && k->ops->banner) { sanitize(text); k->ops->banner(k->ctx, text); }
        return true;
    }
    case SSH_MSG_USERAUTH_FAILURE: {
        if (k->stage != SSH_CLI_AUTH) { give_up(k, SSH_DISC_PROTOCOL_ERROR, "unexpected auth failure"); return true; }
        const uint8_t *m; uint32_t mn;
        sshr_string(r, &m, &mn);
        if (r->err) { give_up(k, SSH_DISC_PROTOCOL_ERROR, "bad auth failure"); return true; }
        next_auth(k, m, mn);
        return true;
    }
    case SSH_MSG_USERAUTH_SUCCESS:
        if (k->stage != SSH_CLI_AUTH) { give_up(k, SSH_DISC_PROTOCOL_ERROR, "unexpected auth success"); return true; }
        open_session(k);
        return true;
    case 60:                     // INFO_REQUEST, or PK_OK (never asked for)
        if (k->kbd_active && k->stage == SSH_CLI_AUTH) { on_info_request(k, r); return true; }
        give_up(k, SSH_DISC_PROTOCOL_ERROR, "unexpected message 60");
        return true;
    case SSH_MSG_GLOBAL_REQUEST: {
        char name[64];
        sshr_cstr(r, name, sizeof(name));
        bool want = sshr_bool(r);
        // hostkeys-00@openssh.com, keepalives: answered no when an answer is wanted.
        if (want) { SshW *w = ssh_begin(c, SSH_MSG_REQUEST_FAILURE); (void)w; ssh_end(c); }
        return true;
    }
    case SSH_MSG_REQUEST_SUCCESS: case SSH_MSG_REQUEST_FAILURE:
        return true;
    case SSH_MSG_CHANNEL_OPEN: {
        // The server opening a channel to us (forwarding): never.
        const uint8_t *t; uint32_t tn;
        sshr_string(r, &t, &tn);
        uint32_t sender = sshr_u32(r);
        SshW *w = ssh_begin(c, SSH_MSG_CHANNEL_OPEN_FAILURE);
        sshw_u32(w, sender);
        sshw_u32(w, 1);
        sshw_cstr(w, "not supported");
        sshw_cstr(w, "");
        ssh_end(c);
        return true;
    }
    }
    if (type >= SSH_MSG_CHANNEL_OPEN_CONFIRMATION && type <= SSH_MSG_CHANNEL_FAILURE)
        return channel_msg(k, type, r);
    return false;
}

// The host key check is the transport's callback; it hands it on to ours.
static bool hostkey_trampoline(SshConn *c, const uint8_t *blob, uint32_t n) {
    SshClient *k = of(c);
    return k->ops->hostkey && k->ops->hostkey(k->ctx, blob, n);
}

bool ssh_cli_init(SshClient *k, const char *user, const SshClientOps *ops, void *ctx) {
    memset(k, 0, sizeof(*k));
    k->ops = ops;
    k->ctx = ctx;
    snprintf(k->user, sizeof(k->user), "%s", user);
    if (!ssh_conn_init(&k->c, false, "RPCortex_2")) return false;
    k->c.hostkey_check = hostkey_trampoline;
    k->c.dispatch = dispatch;
    k->c.owner = k;
    return true;
}

void ssh_cli_free(SshClient *k) { ssh_conn_free(&k->c); }

uint32_t ssh_cli_send(SshClient *k, const uint8_t *d, uint32_t n) {
    uint32_t sent = 0;
    while (sent < n && k->stage == SSH_CLI_RUNNING && !k->close_out) {
        uint32_t take = n - sent;
        if (take > k->peer_window) take = k->peer_window;
        if (take > k->peer_maxpkt) take = k->peer_maxpkt;
        if (take > SSH_CLI_CHUNK) take = SSH_CLI_CHUNK;
        if (take == 0 || !ssh_can_send(&k->c, take + 16)) break;
        SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_DATA);
        sshw_u32(w, k->peer_id);
        sshw_string(w, d + sent, take);
        if (!ssh_end(&k->c)) break;
        k->peer_window -= take;
        sent += take;
    }
    return sent;
}

void ssh_cli_winch(SshClient *k, uint32_t cols, uint32_t rows) {
    k->cols = cols; k->rows = rows;
    if (k->stage != SSH_CLI_RUNNING || k->close_out || !k->want_pty) return;
    SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_REQUEST);
    sshw_u32(w, k->peer_id);
    sshw_cstr(w, "window-change");
    sshw_bool(w, false);
    sshw_u32(w, cols); sshw_u32(w, rows); sshw_u32(w, 0); sshw_u32(w, 0);
    ssh_end(&k->c);
}

void ssh_cli_close(SshClient *k) {
    if (k->stage == SSH_CLI_RUNNING && !k->close_out) {
        SshW *w = ssh_begin(&k->c, SSH_MSG_CHANNEL_EOF);
        sshw_u32(w, k->peer_id);
        ssh_end(&k->c);
        send_close(k);
    } else if (k->stage != SSH_CLI_RUNNING) {
        give_up(k, SSH_DISC_BY_APPLICATION, "closed");
    }
}
