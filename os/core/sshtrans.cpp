#include "sshtrans.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// --- what this side offers ---------------------------------------------------------
//
// The strict-KEX and ext-info names are pseudo-algorithms: they only mean
// something in the FIRST KEXINIT, and the strict-KEX draft says their presence
// later MUST be ignored. Both the standard and the pre-standard strict names
// are offered, as the draft recommends; OpenSSH knows the pre-standard one.
static const char kKexServerFirst[] =
    "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,"
    "kex-strict-s-v00@openssh.com,kex-strict-s";
static const char kKexClientFirst[] =
    "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,"
    "ext-info-c,kex-strict-c-v00@openssh.com,kex-strict-c";
static const char kKexLater[] =
    "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256";
static const char kHostKeyServer[] = "ecdsa-sha2-nistp256";
static const char kHostKeyClient[] = "ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256";
static const char kCiphers[] =
    "aes128-gcm@openssh.com,aes256-gcm@openssh.com,aes128-ctr,aes256-ctr";
static const char kMacs[] = "hmac-sha2-256-etm@openssh.com,hmac-sha2-256";

static int kex_of(const char *n) {
    if (!strcmp(n, "curve25519-sha256") || !strcmp(n, "curve25519-sha256@libssh.org"))
        return SSH_KEX_C25519;
    if (!strcmp(n, "ecdh-sha2-nistp256")) return SSH_KEX_P256;
    return SSH_KEX_NONE;
}
static int cipher_of(const char *n) {
    if (!strcmp(n, "aes128-gcm@openssh.com")) return SSH_AES128_GCM;
    if (!strcmp(n, "aes256-gcm@openssh.com")) return SSH_AES256_GCM;
    if (!strcmp(n, "aes128-ctr")) return SSH_AES128_CTR;
    if (!strcmp(n, "aes256-ctr")) return SSH_AES256_CTR;
    return -1;
}
static int mac_of(const char *n) {
    if (!strcmp(n, "hmac-sha2-256-etm@openssh.com")) return SSH_MAC_HMAC256_ETM;
    if (!strcmp(n, "hmac-sha2-256")) return SSH_MAC_HMAC256;
    return -1;
}
static bool is_aead(int alg) { return alg == SSH_AES128_GCM || alg == SSH_AES256_GCM; }

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// --- lifecycle ----------------------------------------------------------------------

static void send_kexinit(SshConn *c);

bool ssh_conn_init(SshConn *c, bool server, const char *ident) {
    memset(c, 0, sizeof(*c));
    c->server = server;
    ssh_cipher_init(&c->rx.cipher);
    ssh_cipher_init(&c->tx.cipher);
    c->in  = (uint8_t *)malloc(SSH_IN_START);
    c->out = (uint8_t *)malloc(SSH_OUT_CAP);
    if (!c->in || !c->out) { free(c->in); free(c->out); c->in = c->out = nullptr; return false; }
    c->in_cap = SSH_IN_START;
    snprintf(c->v_mine, sizeof(c->v_mine), "SSH-2.0-%s", ident);
    // The version line, then our KEXINIT at once rather than after the peer's
    // version: one round trip fewer, and strict KEX wants KEXINIT to be the
    // first packet either way, which it is.
    uint32_t n = (uint32_t)strlen(c->v_mine);
    memcpy(c->out, c->v_mine, n);
    c->out[n] = '\r'; c->out[n + 1] = '\n';
    c->out_len = n + 2;
    c->state = SSH_ST_VERSION;
    send_kexinit(c);
    return c->state != SSH_ST_CLOSED;
}

void ssh_conn_free(SshConn *c) {
    ssh_ecdh_free(&c->ecdh);
    ssh_cipher_free(&c->rx.cipher);
    ssh_cipher_free(&c->tx.cipher);
    free(c->i_mine); free(c->i_peer);
    c->i_mine = c->i_peer = nullptr;
    if (c->in)  { ssh_wipe(c->in, c->in_cap); free(c->in); }
    if (c->out) { ssh_wipe(c->out, SSH_OUT_CAP); free(c->out); }
    c->in = c->out = nullptr;
    ssh_wipe(&c->next_tx, sizeof(c->next_tx));
    ssh_wipe(&c->next_rx, sizeof(c->next_rx));
    c->state = SSH_ST_CLOSED;
}

bool ssh_open(const SshConn *c)   { return c->state == SSH_ST_OPEN; }
bool ssh_closed(const SshConn *c) { return c->state == SSH_ST_CLOSED; }

static void fail(SshConn *c, uint32_t reason, const char *why) { ssh_disconnect(c, reason, why); }

// --- output -------------------------------------------------------------------------

const uint8_t *ssh_out(const SshConn *c, uint32_t *n) { *n = c->out ? c->out_len : 0; return c->out; }

void ssh_out_consume(SshConn *c, uint32_t n) {
    if (n >= c->out_len) { c->out_len = 0; return; }
    memmove(c->out, c->out + n, c->out_len - n);
    c->out_len -= n;
}

uint32_t ssh_out_room(const SshConn *c) {
    return c->out ? SSH_OUT_CAP - c->out_len : 0;
}

// Room a packet needs beyond its payload: length, padding-length byte, up to
// 4 + 15 bytes of padding, and the tag or MAC.
#define PKT_OVERHEAD (4 + 1 + 19 + 32)

// Between our KEXINIT and our NEWKEYS only transport and key exchange messages
// may go out (RFC 4253 section 7.1). Anything the service layer sends then
// waits in `defer`, as its bare payload, and is sealed once NEWKEYS is out.
static bool must_defer(const SshConn *c, uint8_t type) {
    if (!c->kexinit_sent || c->newkeys_sent) return false;
    return type >= SSH_MSG_SERVICE_REQUEST && !(type >= 20 && type <= 49);
}

static SshW g_dead_w;   // handed out after the connection has closed: err set, writes nothing

SshW *ssh_begin(SshConn *c, uint8_t type) {
    if (c->state == SSH_ST_CLOSED || !c->out || c->w_open) {
        sshw_init(&g_dead_w, nullptr, 0);
        g_dead_w.err = true;
        return &g_dead_w;
    }
    c->w_open = true;
    c->w_defer = must_defer(c, type);
    if (c->w_defer) {
        c->w_start = c->defer_len;
        uint32_t room = SSH_DEFER_CAP - c->defer_len;
        sshw_init(&c->w, c->defer + c->defer_len + 4, room > 4 ? room - 4 : 0);
        if (room <= 4) c->w.err = true;
    } else {
        c->w_start = c->out_len;
        uint32_t room = SSH_OUT_CAP - c->out_len;
        sshw_init(&c->w, c->out + c->out_len + 5, room > PKT_OVERHEAD ? room - PKT_OVERHEAD : 0);
        if (room <= PKT_OVERHEAD) c->w.err = true;
    }
    sshw_u8(&c->w, type);
    return &c->w;
}

// Pad, length, seal and count the payload sitting at out + start + 5.
static void seal(SshConn *c, uint32_t start, uint32_t plen) {
    SshDir *d = &c->tx;
    int alg = d->cipher.alg, mac = d->cipher.mac;
    uint32_t block = ssh_cipher_block(alg);
    // With the length in the clear (GCM, ETM) only the encrypted part has to be
    // a whole number of blocks; otherwise the length is counted too.
    uint32_t counted = ssh_cipher_len_plain(alg, mac) && alg != SSH_CIPHER_NONE
                       ? 1 + plen : 4 + 1 + plen;
    uint32_t pad = block - (counted % block);
    if (pad < 4) pad += block;
    uint8_t *p = c->out + start;
    uint32_t len = 1 + plen + pad;
    put32(p, len);
    p[4] = (uint8_t)pad;
    if (!ssh_random(p + 5 + plen, pad)) memset(p + 5 + plen, 0, pad);
    uint32_t tag = ssh_cipher_seal(&d->cipher, d->seq, p, len);
    c->out_len = start + 4 + len + tag;
    d->seq++;
    d->packets++;
    d->bytes += 4 + len + tag;
}

static bool flush_deferred(SshConn *c) {
    uint32_t at = 0;
    while (at + 4 <= c->defer_len) {
        uint32_t n = be32(c->defer + at);
        if (SSH_OUT_CAP - c->out_len < n + PKT_OVERHEAD + 5) return false;
        memcpy(c->out + c->out_len + 5, c->defer + at + 4, n);
        seal(c, c->out_len, n);
        at += 4 + n;
    }
    c->defer_len = 0;
    return true;
}

bool ssh_end(SshConn *c) {
    if (!c->w_open) return false;
    c->w_open = false;
    if (c->w.err) {
        fail(c, SSH_DISC_BY_APPLICATION, "a reply did not fit the output buffer");
        return false;
    }
    if (c->w_defer) {
        put32(c->defer + c->w_start, c->w.len);
        c->defer_len += 4 + c->w.len;
        return true;
    }
    seal(c, c->w_start, c->w.len);
    if (c->state == SSH_ST_OPEN && !c->kex_running &&
        (c->tx.bytes > SSH_REKEY_BYTES || c->tx.packets > SSH_REKEY_PACKETS))
        ssh_rekey(c);
    return true;
}

bool ssh_can_send(const SshConn *c, uint32_t need) {
    if (c->state != SSH_ST_OPEN) return false;
    if (c->kexinit_sent && !c->newkeys_sent) return false;
    return ssh_out_room(c) >= need + PKT_OVERHEAD + SSH_OUT_RESERVE / 2;
}

void ssh_disconnect(SshConn *c, uint32_t reason, const char *why) {
    if (c->state == SSH_ST_CLOSED) return;
    if (!c->disc_reason) c->disc_reason = (uint8_t)reason;
    if (!c->why[0]) snprintf(c->why, sizeof(c->why), "%s", why ? why : "");
    // Our version line and KEXINIT went out at the start, so the peer is owed
    // packets whatever state it reached. Never in the middle of building
    // another packet, though: a half-built one cannot be abandoned cleanly.
    if (c->out && !c->w_open && SSH_OUT_CAP - c->out_len > 128 + PKT_OVERHEAD) {
        uint32_t start = c->out_len;
        SshW w; sshw_init(&w, c->out + start + 5, 128);
        sshw_u8(&w, SSH_MSG_DISCONNECT);
        sshw_u32(&w, reason);
        sshw_string(&w, why ? why : "", why ? (uint32_t)(strlen(why) > 64 ? 64 : strlen(why)) : 0);
        sshw_cstr(&w, "");
        if (!w.err) seal(c, start, w.len);
    }
    c->state = SSH_ST_CLOSED;
}

// --- KEXINIT ---------------------------------------------------------------------

static void send_kexinit(SshConn *c) {
    if (c->kexinit_sent) return;
    uint8_t buf[640];
    SshW w; sshw_init(&w, buf, sizeof(buf));
    sshw_u8(&w, SSH_MSG_KEXINIT);
    uint8_t cookie[16];
    if (!ssh_random(cookie, sizeof(cookie))) { fail(c, SSH_DISC_KEX_FAILED, "no randomness"); return; }
    sshw_raw(&w, cookie, sizeof(cookie));
    const char *kex = c->first_kex_done ? kKexLater : (c->server ? kKexServerFirst : kKexClientFirst);
    sshw_cstr(&w, kex);
    sshw_cstr(&w, c->server ? kHostKeyServer : (c->hostkey_algs ? c->hostkey_algs : kHostKeyClient));
    sshw_cstr(&w, kCiphers); sshw_cstr(&w, kCiphers);
    sshw_cstr(&w, kMacs);    sshw_cstr(&w, kMacs);
    sshw_cstr(&w, "none");   sshw_cstr(&w, "none");
    sshw_cstr(&w, "");       sshw_cstr(&w, "");
    sshw_bool(&w, false);    // no guessed packet follows
    sshw_u32(&w, 0);
    if (w.err) { fail(c, SSH_DISC_KEX_FAILED, "KEXINIT too large"); return; }

    free(c->i_mine);
    c->i_mine = (uint8_t *)malloc(w.len);
    if (!c->i_mine) { fail(c, SSH_DISC_KEX_FAILED, "out of memory"); return; }
    memcpy(c->i_mine, buf, w.len);
    c->i_mine_n = w.len;

    SshW *o = ssh_begin(c, SSH_MSG_KEXINIT);
    sshw_raw(o, buf + 1, w.len - 1);
    if (!ssh_end(c)) return;
    c->kexinit_sent = true;
    c->kex_running = true;
}

void ssh_rekey(SshConn *c) {
    if (c->state != SSH_ST_OPEN || c->kex_running) return;
    send_kexinit(c);
}

struct KexLists {
    const uint8_t *l[10];
    uint32_t       n[10];
    bool           follows;
};

static bool parse_kexinit(const uint8_t *p, uint32_t n, KexLists *k) {
    SshR r; sshr_init(&r, p, n);
    sshr_u8(&r);                         // the message number
    for (int i = 0; i < 16; i++) sshr_u8(&r);
    for (int i = 0; i < 10; i++) sshr_string(&r, &k->l[i], &k->n[i]);
    k->follows = sshr_bool(&r);
    sshr_u32(&r);
    return !r.err;
}

static void ecdh_client_start(SshConn *c);

static void on_kexinit(SshConn *c, const uint8_t *p, uint32_t n, uint32_t seq) {
    if (c->kexinit_recv) { fail(c, SSH_DISC_PROTOCOL_ERROR, "second KEXINIT"); return; }
    KexLists peer, mine;
    if (!parse_kexinit(p, n, &peer)) { fail(c, SSH_DISC_PROTOCOL_ERROR, "bad KEXINIT"); return; }

    // The pseudo-algorithms, read from the first exchange only.
    if (!c->first_kex_done) {
        const uint8_t *kl = peer.l[0]; uint32_t kn = peer.n[0];
        if (c->server) {
            c->strict = ssh_namelist_has(kl, kn, "kex-strict-c-v00@openssh.com") ||
                        ssh_namelist_has(kl, kn, "kex-strict-c");
            c->peer_ext_info = ssh_namelist_has(kl, kn, "ext-info-c");
        } else {
            c->strict = ssh_namelist_has(kl, kn, "kex-strict-s-v00@openssh.com") ||
                        ssh_namelist_has(kl, kn, "kex-strict-s");
        }
        // Strict KEX: the KEXINIT has to have been the very first packet. An
        // IGNORE slipped in ahead of it is exactly the Terrapin injection.
        if (c->strict && seq != 0) {
            fail(c, SSH_DISC_KEX_FAILED, "strict KEX: KEXINIT was not the first packet");
            return;
        }
    }

    free(c->i_peer);
    c->i_peer = (uint8_t *)malloc(n);
    if (!c->i_peer) { fail(c, SSH_DISC_KEX_FAILED, "out of memory"); return; }
    memcpy(c->i_peer, p, n);
    c->i_peer_n = n;
    c->kexinit_recv = true;
    c->kex_running = true;
    c->ecdh_done = false;

    if (!c->kexinit_sent) send_kexinit(c);
    if (c->state == SSH_ST_CLOSED) return;
    if (!parse_kexinit(c->i_mine, c->i_mine_n, &mine)) { fail(c, SSH_DISC_KEX_FAILED, "own KEXINIT"); return; }

    const KexLists &cl = c->server ? peer : mine;
    const KexLists &sl = c->server ? mine : peer;
    char name[64];

    if (!ssh_negotiate(cl.l[0], cl.n[0], sl.l[0], sl.n[0], name, sizeof(name)) ||
        (c->kex_alg = kex_of(name)) == SSH_KEX_NONE) {
        fail(c, SSH_DISC_KEX_FAILED, "no key exchange in common"); return;
    }
    if (!ssh_negotiate(cl.l[1], cl.n[1], sl.l[1], sl.n[1], c->hostkey_alg, sizeof(c->hostkey_alg))) {
        fail(c, SSH_DISC_KEX_FAILED, "no host key type in common"); return;
    }
    int enc[2] = { -1, -1 }, mac[2] = { SSH_MAC_NONE, SSH_MAC_NONE };
    for (int dir = 0; dir < 2; dir++) {
        if (!ssh_negotiate(cl.l[2 + dir], cl.n[2 + dir], sl.l[2 + dir], sl.n[2 + dir], name, sizeof(name)) ||
            (enc[dir] = cipher_of(name)) < 0) {
            fail(c, SSH_DISC_KEX_FAILED, "no cipher in common"); return;
        }
        // An AEAD cipher brings its own integrity, and the MAC lists are not
        // negotiated at all — failing to agree on one must not fail the
        // connection (draft-miller-sshm-aes-gcm section 2).
        if (!is_aead(enc[dir])) {
            if (!ssh_negotiate(cl.l[4 + dir], cl.n[4 + dir], sl.l[4 + dir], sl.n[4 + dir], name, sizeof(name)) ||
                (mac[dir] = mac_of(name)) < 0) {
                fail(c, SSH_DISC_KEX_FAILED, "no MAC in common"); return;
            }
        }
        if (!ssh_namelist_has(cl.l[6 + dir], cl.n[6 + dir], "none") ||
            !ssh_namelist_has(sl.l[6 + dir], sl.n[6 + dir], "none")) {
            fail(c, SSH_DISC_KEX_FAILED, "compression is not supported"); return;
        }
    }
    c->enc_cs = enc[0]; c->enc_sc = enc[1];
    c->mac_cs = mac[0]; c->mac_sc = mac[1];

    // A guessed packet follows and the guess was wrong: it is dropped unread
    // (RFC 4253 section 7.1). The guess is right only when both first choices
    // of kex and host key algorithm agree.
    if (peer.follows) {
        char a[64], b[64], e[64], f[64];
        bool right = ssh_namelist_first(peer.l[0], peer.n[0], a, sizeof(a)) &&
                     ssh_namelist_first(mine.l[0], mine.n[0], b, sizeof(b)) &&
                     ssh_namelist_first(peer.l[1], peer.n[1], e, sizeof(e)) &&
                     ssh_namelist_first(mine.l[1], mine.n[1], f, sizeof(f)) &&
                     !strcmp(a, b) && !strcmp(e, f);
        c->ignore_next = !right;
    }

    if (!c->server) ecdh_client_start(c);
}

// --- the exchange hash and keys -------------------------------------------------------

static void exchange_hash(SshConn *c, const uint8_t *ks, uint32_t ksn,
                          const uint8_t *qc, uint32_t qcn, const uint8_t *qs, uint32_t qsn,
                          const uint8_t *k, uint32_t kn, uint8_t h[32]) {
    SshHash s;
    ssh_hash_init(&s);
    const char *vc = c->server ? c->v_peer : c->v_mine;
    const char *vs = c->server ? c->v_mine : c->v_peer;
    ssh_hash_string(&s, vc, (uint32_t)strlen(vc));
    ssh_hash_string(&s, vs, (uint32_t)strlen(vs));
    if (c->server) {
        ssh_hash_string(&s, c->i_peer, c->i_peer_n);
        ssh_hash_string(&s, c->i_mine, c->i_mine_n);
    } else {
        ssh_hash_string(&s, c->i_mine, c->i_mine_n);
        ssh_hash_string(&s, c->i_peer, c->i_peer_n);
    }
    ssh_hash_string(&s, ks, ksn);
    ssh_hash_string(&s, qc, qcn);
    ssh_hash_string(&s, qs, qsn);
    ssh_hash_mpint(&s, k, kn);
    ssh_hash_final(&s, h);
}

static void derive(SshConn *c, const uint8_t *k, uint32_t kn, const uint8_t h[32]) {
    if (!c->have_session_id) {
        // The first exchange hash is the session identifier for the life of
        // the connection; re-keying does not change it (RFC 4253 7.2).
        memcpy(c->session_id, h, 32);
        c->have_session_id = true;
    }
    // c->s is "A","C","E"; s->c is "B","D","F".
    SshKeys cs, sc;
    memset(&cs, 0, sizeof(cs)); memset(&sc, 0, sizeof(sc));
    cs.enc = c->enc_cs; cs.mac = c->mac_cs;
    sc.enc = c->enc_sc; sc.mac = c->mac_sc;
    ssh_kdf(k, kn, h, 'A', c->session_id, cs.iv, ssh_cipher_ivlen(cs.enc));
    ssh_kdf(k, kn, h, 'B', c->session_id, sc.iv, ssh_cipher_ivlen(sc.enc));
    ssh_kdf(k, kn, h, 'C', c->session_id, cs.key, ssh_cipher_keylen(cs.enc));
    ssh_kdf(k, kn, h, 'D', c->session_id, sc.key, ssh_cipher_keylen(sc.enc));
    if (cs.mac) ssh_kdf(k, kn, h, 'E', c->session_id, cs.mac_key, 32);
    if (sc.mac) ssh_kdf(k, kn, h, 'F', c->session_id, sc.mac_key, 32);
    c->next_tx = c->server ? sc : cs;
    c->next_rx = c->server ? cs : sc;
    ssh_wipe(&cs, sizeof(cs)); ssh_wipe(&sc, sizeof(sc));
}

static void send_ext_info(SshConn *c) {
    // RFC 8308: tells the client which signature algorithms the publickey
    // method accepts. Without it OpenSSH falls back to guessing for an RSA key.
    SshW *w = ssh_begin(c, SSH_MSG_EXT_INFO);
    sshw_u32(w, 1);
    sshw_cstr(w, "server-sig-algs");
    sshw_cstr(w, "ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256");
    ssh_end(c);
}

static void send_newkeys(SshConn *c) {
    SshW *w = ssh_begin(c, SSH_MSG_NEWKEYS);
    (void)w;
    if (!ssh_end(c)) return;
    // The new keys from the NEXT packet on, which is the one after NEWKEYS.
    if (!ssh_cipher_set(&c->tx.cipher, c->next_tx.enc, c->next_tx.mac, c->next_tx.key,
                        c->next_tx.iv, c->next_tx.mac_key)) {
        fail(c, SSH_DISC_KEX_FAILED, "could not take the new keys into use");
        return;
    }
    ssh_wipe(&c->next_tx, sizeof(c->next_tx));
    // Strict KEX: the sending sequence number starts again at zero after every
    // NEWKEYS this side sends (draft-miller-sshm-strict-kex section 3.3).
    if (c->strict) c->tx.seq = 0;
    c->tx.packets = 0; c->tx.bytes = 0;
    c->newkeys_sent = true;
    // EXT_INFO, if wanted, is the very next packet after the first NEWKEYS.
    if (c->server && !c->first_kex_done && c->peer_ext_info) send_ext_info(c);
    if (!flush_deferred(c)) fail(c, SSH_DISC_BY_APPLICATION, "deferred replies did not fit");
}

static void kex_finish(SshConn *c) {
    ssh_ecdh_free(&c->ecdh);
    free(c->i_mine); free(c->i_peer);
    c->i_mine = c->i_peer = nullptr;
    c->kex_running = false;
    c->kexinit_sent = c->kexinit_recv = false;
    c->newkeys_sent = c->newkeys_recv = false;
    c->ecdh_done = false;
    if (!c->first_kex_done) {
        c->first_kex_done = true;
        c->state = SSH_ST_OPEN;
        SshR empty; sshr_init(&empty, nullptr, 0);
        if (c->dispatch) c->dispatch(c, 0, &empty);
    }
}

// --- the ECDH messages -------------------------------------------------------------------

// Server: SSH_MSG_KEX_ECDH_INIT carries Q_C; answer with K_S, Q_S and a
// signature over the exchange hash, then NEWKEYS.
static void on_ecdh_init(SshConn *c, SshR *r) {
    if (!c->server || !c->kexinit_recv || !c->kexinit_sent || c->ecdh_done) {
        fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected KEX_ECDH_INIT"); return;
    }
    // Accepted once per exchange: the strict-KEX draft requires it and there is
    // no reason ever to allow a second.
    c->ecdh_done = true;
    const uint8_t *qc; uint32_t qcn;
    if (!sshr_string(r, &qc, &qcn) || sshr_left(r) != 0) {
        fail(c, SSH_DISC_PROTOCOL_ERROR, "bad KEX_ECDH_INIT"); return;
    }
    if (!c->hostkey || !c->hostkey->ok) { fail(c, SSH_DISC_KEX_FAILED, "no host key"); return; }

    uint8_t qs[65];
    uint32_t qsn = ssh_ecdh_start(&c->ecdh, c->kex_alg, qs, sizeof(qs));
    uint8_t k[32];
    uint32_t kn = qsn ? ssh_ecdh_finish(&c->ecdh, qc, qcn, k) : 0;
    ssh_ecdh_free(&c->ecdh);
    if (!kn) { fail(c, SSH_DISC_KEX_FAILED, "key agreement failed"); return; }

    uint8_t ks[160];
    uint32_t ksn = ssh_hostkey_blob(c->hostkey, ks, sizeof(ks));
    uint8_t h[32];
    exchange_hash(c, ks, ksn, qc, qcn, qs, qsn, k, kn, h);
    uint8_t sig[128];
    uint32_t sn = ssh_hostkey_sign(c->hostkey, h, 32, sig, sizeof(sig));
    if (!ksn || !sn) { ssh_wipe(k, sizeof(k)); fail(c, SSH_DISC_KEX_FAILED, "could not sign"); return; }
    derive(c, k, kn, h);
    ssh_wipe(k, sizeof(k));

    SshW *w = ssh_begin(c, SSH_MSG_KEX_ECDH_REPLY);
    sshw_string(w, ks, ksn);
    sshw_string(w, qs, qsn);
    sshw_string(w, sig, sn);
    if (!ssh_end(c)) return;
    send_newkeys(c);
}

static void ecdh_client_start(SshConn *c) {
    c->q_mine_n = ssh_ecdh_start(&c->ecdh, c->kex_alg, c->q_mine, sizeof(c->q_mine));
    if (!c->q_mine_n) { fail(c, SSH_DISC_KEX_FAILED, "could not make a key"); return; }
    SshW *w = ssh_begin(c, SSH_MSG_KEX_ECDH_INIT);
    sshw_string(w, c->q_mine, c->q_mine_n);
    ssh_end(c);
}

// The host key algorithm negotiated has to match the key the server sent.
static bool key_fits_alg(const uint8_t *ks, uint32_t ksn, const char *alg) {
    char type[24];
    if (!ssh_key_type(ks, ksn, type, sizeof(type))) return false;
    if (!strcmp(type, "ssh-rsa")) return !strcmp(alg, "rsa-sha2-256") || !strcmp(alg, "rsa-sha2-512");
    return !strcmp(type, alg);
}

static void on_ecdh_reply(SshConn *c, SshR *r) {
    if (c->server || !c->kexinit_recv || c->ecdh_done || !c->q_mine_n) {
        fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected KEX_ECDH_REPLY"); return;
    }
    c->ecdh_done = true;
    const uint8_t *ks, *qs, *sig; uint32_t ksn, qsn, sn;
    if (!sshr_string(r, &ks, &ksn) || !sshr_string(r, &qs, &qsn) || !sshr_string(r, &sig, &sn) ||
        sshr_left(r) != 0) {
        fail(c, SSH_DISC_PROTOCOL_ERROR, "bad KEX_ECDH_REPLY"); return;
    }
    if (!key_fits_alg(ks, ksn, c->hostkey_alg)) {
        fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "host key does not match its algorithm"); return;
    }
    if (!c->first_kex_done) {
        // Who the server is, asked of the caller before anything is believed.
        if (ksn > sizeof(c->peer_hostkey)) { fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "host key too large"); return; }
        memcpy(c->peer_hostkey, ks, ksn);
        c->peer_hostkey_n = ksn;
        if (!c->hostkey_check || !c->hostkey_check(c, ks, ksn)) {
            fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "host key refused"); return;
        }
    } else if (ksn != c->peer_hostkey_n || memcmp(ks, c->peer_hostkey, ksn) != 0) {
        // A re-key must be signed by the key the connection was opened with.
        fail(c, SSH_DISC_HOST_KEY_NOT_VERIFIABLE, "host key changed during re-key"); return;
    }
    uint8_t k[32];
    uint32_t kn = ssh_ecdh_finish(&c->ecdh, qs, qsn, k);
    ssh_ecdh_free(&c->ecdh);
    if (!kn) { fail(c, SSH_DISC_KEX_FAILED, "key agreement failed"); return; }
    uint8_t h[32];
    exchange_hash(c, ks, ksn, c->q_mine, c->q_mine_n, qs, qsn, k, kn, h);
    if (!ssh_sig_verify(ks, ksn, sig, sn, h, 32, c->hostkey_alg)) {
        ssh_wipe(k, sizeof(k));
        fail(c, SSH_DISC_KEX_FAILED, "the server's signature does not verify"); return;
    }
    derive(c, k, kn, h);
    ssh_wipe(k, sizeof(k));
    c->q_mine_n = 0;
    send_newkeys(c);
}

static void on_newkeys(SshConn *c) {
    if (!c->kexinit_recv || !c->ecdh_done || c->newkeys_recv) {
        fail(c, SSH_DISC_PROTOCOL_ERROR, "unexpected NEWKEYS"); return;
    }
    if (!ssh_cipher_set(&c->rx.cipher, c->next_rx.enc, c->next_rx.mac, c->next_rx.key,
                        c->next_rx.iv, c->next_rx.mac_key)) {
        fail(c, SSH_DISC_KEX_FAILED, "could not take the new keys into use"); return;
    }
    ssh_wipe(&c->next_rx, sizeof(c->next_rx));
    if (c->strict) c->rx.seq = 0;
    c->rx.packets = 0; c->rx.bytes = 0;
    c->newkeys_recv = true;
    if (c->newkeys_sent) kex_finish(c);
}

// --- reading packets -----------------------------------------------------------------------

static void handle(SshConn *c, const uint8_t *p, uint32_t n, uint32_t seq) {
    uint8_t type = p[0];
    if (c->ignore_next) { c->ignore_next = false; return; }

    bool kex_msg = type == SSH_MSG_KEXINIT || type == SSH_MSG_NEWKEYS ||
                   type == SSH_MSG_KEX_ECDH_INIT || type == SSH_MSG_KEX_ECDH_REPLY;
    // Strict KEX: nothing but key exchange messages until the first exchange is
    // done, not even IGNORE or DEBUG, which are the Terrapin attack's tools. A
    // peer that has not agreed to strict KEX may still send those (RFC 4253
    // allows it, and its KEXINIT then fails the seq != 0 test if it offers
    // strict KEX after all), but nothing from the service layer: accepting
    // that before encryption is the bug the strict-KEX draft cites.
    if (!c->first_kex_done && !kex_msg && type != SSH_MSG_DISCONNECT) {
        bool chatter = type == SSH_MSG_IGNORE || type == SSH_MSG_DEBUG ||
                       type == SSH_MSG_UNIMPLEMENTED;
        if (c->strict || !chatter) {
            fail(c, SSH_DISC_PROTOCOL_ERROR, "non-KEX message during key exchange");
            return;
        }
    }

    SshR r; sshr_init(&r, p + 1, n - 1);
    switch (type) {
    case SSH_MSG_DISCONNECT: {
        uint32_t reason = sshr_u32(&r);
        char why[48]; sshr_cstr(&r, why, sizeof(why));
        c->disc_reason = (uint8_t)reason;
        snprintf(c->why, sizeof(c->why), "peer disconnected: %s", r.err ? "" : why);
        c->state = SSH_ST_CLOSED;
        return;
    }
    case SSH_MSG_IGNORE: case SSH_MSG_DEBUG: case SSH_MSG_UNIMPLEMENTED:
        return;
    case SSH_MSG_KEXINIT:    on_kexinit(c, p, n, seq); return;
    case SSH_MSG_NEWKEYS:    on_newkeys(c); return;
    case SSH_MSG_KEX_ECDH_INIT:  on_ecdh_init(c, &r); return;
    case SSH_MSG_KEX_ECDH_REPLY: on_ecdh_reply(c, &r); return;
    default: break;
    }
    if (type >= 20 && type <= 49) { fail(c, SSH_DISC_PROTOCOL_ERROR, "unknown key exchange message"); return; }
    // Service messages: only once encrypted, and never from a peer that has
    // started a key exchange and not finished it — after its KEXINIT it may
    // send nothing else until its NEWKEYS.
    if (c->state != SSH_ST_OPEN || (c->kexinit_recv && !c->newkeys_recv)) {
        fail(c, SSH_DISC_PROTOCOL_ERROR, "message out of order"); return;
    }
    bool known = c->dispatch && c->dispatch(c, type, &r);
    if (!known && c->state != SSH_ST_CLOSED) {
        SshW *w = ssh_begin(c, SSH_MSG_UNIMPLEMENTED);
        sshw_u32(w, seq);
        ssh_end(c);
    }
}

static bool grow_in(SshConn *c, uint32_t need) {
    if (need <= c->in_cap) return true;
    uint32_t cap = c->in_cap;
    while (cap < need) cap *= 2;
    uint32_t limit = SSH_PKT_MAX + 4 + 64;
    if (cap > limit) cap = limit;
    if (cap < need) return false;
    uint8_t *p = (uint8_t *)realloc(c->in, cap);
    if (!p) return false;
    c->in = p;
    c->in_cap = cap;
    return true;
}

// One packet, if a whole one is waiting. False when there is not.
static bool one_packet(SshConn *c) {
    SshDir *d = &c->rx;
    int alg = d->cipher.alg, mac = d->cipher.mac;
    bool plain_len = ssh_cipher_len_plain(alg, mac);
    if (!plain_len) {
        if (c->in_len < 16) return false;
        if (!c->len_known) { ssh_cipher_open_first(&d->cipher, c->in); c->len_known = true; }
    } else if (c->in_len < 4) {
        return false;
    }
    uint32_t len = be32(c->in);
    uint32_t block = ssh_cipher_block(alg);
    uint32_t counted = (plain_len && alg != SSH_CIPHER_NONE) ? len : 4 + len;
    // The length has not been authenticated yet (except under none, where
    // nothing is), so it is checked for sanity before a byte is waited for:
    // big enough for the padding rules, small enough to buffer, and on a block
    // boundary. A bad one ends the connection rather than leaving it waiting
    // for a packet that will never complete.
    if (len < 12 || len > SSH_PKT_MAX || counted % block != 0) {
        fail(c, SSH_DISC_PROTOCOL_ERROR, "bad packet length"); return false;
    }
    uint32_t tag = ssh_cipher_taglen(alg, mac);
    uint32_t total = 4 + len + tag;
    if (!grow_in(c, total)) { fail(c, SSH_DISC_BY_APPLICATION, "packet too large for memory"); return false; }
    if (c->in_len < total) return false;

    uint32_t seq = d->seq;
    if (!ssh_cipher_open(&d->cipher, seq, c->in, len)) {
        fail(c, SSH_DISC_MAC_ERROR, "packet failed authentication"); return false;
    }
    c->len_known = false;
    d->seq++;
    // Strict KEX: the sequence number may not wrap before the first exchange
    // has finished. An attacker cannot send four billion packets in the clear
    // without being noticed, but the draft asks for the check and it is free.
    if (d->seq == 0 && !c->first_kex_done) { fail(c, SSH_DISC_PROTOCOL_ERROR, "sequence wrapped"); return false; }
    d->packets++;
    d->bytes += total;

    uint8_t pad = c->in[4];
    if (pad < 4 || (uint32_t)pad + 1 >= len) { fail(c, SSH_DISC_PROTOCOL_ERROR, "bad padding"); return false; }
    uint32_t plen = len - pad - 1;
    handle(c, c->in + 5, plen, seq);

    if (c->in_len > total) memmove(c->in, c->in + total, c->in_len - total);
    c->in_len -= total;
    if (c->state == SSH_ST_OPEN && !c->kex_running &&
        (d->bytes > SSH_REKEY_BYTES || d->packets > SSH_REKEY_PACKETS))
        ssh_rekey(c);
    return true;
}

// The peer's version line. RFC 4253 section 4.2: at most 255 bytes including
// CR LF; a server may send other lines first, a client may not. A lone LF is
// accepted as the end of a line, as OpenSSH does for old peers.
static bool version_line(SshConn *c) {
    for (uint32_t i = 0; i < c->in_len; i++) {
        uint8_t ch = c->in[i];
        if (ch == '\n') {
            uint32_t n = c->vline_len;
            if (n && c->v_peer[n - 1] == '\r') n--;
            c->v_peer[n] = 0;
            memmove(c->in, c->in + i + 1, c->in_len - i - 1);
            c->in_len -= i + 1;
            c->vline_len = 0;
            if (strncmp(c->v_peer, "SSH-", 4) != 0) {
                if (c->server || ++c->pre_lines > 50) {
                    fail(c, SSH_DISC_PROTOCOL_ERROR, "no SSH version line");
                    return false;
                }
                return true;      // a banner line from a server: read on
            }
            if (strncmp(c->v_peer, "SSH-2.0-", 8) != 0 && strncmp(c->v_peer, "SSH-1.99-", 9) != 0) {
                fail(c, SSH_DISC_VERSION_NOT_SUPPORTED, "not SSH protocol 2");
                return false;
            }
            c->state = SSH_ST_KEX;
            return true;
        }
        if (c->vline_len >= 254 || ch == 0) {
            fail(c, SSH_DISC_PROTOCOL_ERROR, "version line too long");
            return false;
        }
        c->v_peer[c->vline_len++] = (char)ch;
    }
    c->in_len = 0;      // all of it is part of a line still being read
    return false;
}

uint32_t ssh_feed(SshConn *c, const uint8_t *data, uint32_t n) {
    if (c->state == SSH_ST_CLOSED || !c->in) return n;    // swallowed; nobody is listening
    uint32_t room = c->in_cap - c->in_len;
    uint32_t take = n < room ? n : room;
    memcpy(c->in + c->in_len, data, take);
    c->in_len += take;
    return take;
}

void ssh_process(SshConn *c) {
    while (c->state != SSH_ST_CLOSED) {
        if (c->state == SSH_ST_VERSION) {
            if (!version_line(c)) return;
            continue;
        }
        if (ssh_out_room(c) < SSH_OUT_RESERVE) return;    // let the socket drain first
        if (!one_packet(c)) return;
    }
}
