// The SSH transport layer (RFC 4253): the version lines, the binary packets,
// algorithm negotiation, the key exchange in both roles, and re-keying.
//
// Transport-agnostic: bytes in through ssh_feed, bytes out from ssh_out, and
// everything above the transport (authentication, channels) reached through
// one dispatch callback. No sockets, no tasks and no clock, so the whole thing
// runs on a host with a real OpenSSH on the far end of a loopback socket, which
// is how it is tested.
//
// What is offered, and why only this:
//
//   kex        curve25519-sha256, curve25519-sha256@libssh.org, ecdh-sha2-nistp256
//   host key   ecdsa-sha2-nistp256 (server); the client also accepts
//              rsa-sha2-512 and rsa-sha2-256 host keys
//   ciphers    aes128-gcm@openssh.com, aes256-gcm@openssh.com, aes128-ctr, aes256-ctr
//   MACs       hmac-sha2-256-etm@openssh.com, hmac-sha2-256 (CTR only)
//   other      no compression; strict KEX (the Terrapin countermeasure);
//              ext-info (server-sig-algs)
//
// Every one of those is something mbedTLS already carries for HTTPS, apart
// from the deterministic-ECDSA switch, so SSH costs code and not a second
// crypto library. Ed25519 and ChaCha20-Poly1305 are what is missing; mbedTLS
// 3.6 has neither.
#ifndef RPC_SSHTRANS_H
#define RPC_SSHTRANS_H

#include <stdint.h>
#include "sshwire.h"
#include "sshcrypto.h"

// Message numbers, RFC 4250 section 4.1.
enum {
    SSH_MSG_DISCONNECT = 1, SSH_MSG_IGNORE = 2, SSH_MSG_UNIMPLEMENTED = 3, SSH_MSG_DEBUG = 4,
    SSH_MSG_SERVICE_REQUEST = 5, SSH_MSG_SERVICE_ACCEPT = 6, SSH_MSG_EXT_INFO = 7,
    SSH_MSG_KEXINIT = 20, SSH_MSG_NEWKEYS = 21,
    SSH_MSG_KEX_ECDH_INIT = 30, SSH_MSG_KEX_ECDH_REPLY = 31,
    SSH_MSG_USERAUTH_REQUEST = 50, SSH_MSG_USERAUTH_FAILURE = 51, SSH_MSG_USERAUTH_SUCCESS = 52,
    SSH_MSG_USERAUTH_BANNER = 53, SSH_MSG_USERAUTH_PK_OK = 60,
    SSH_MSG_GLOBAL_REQUEST = 80, SSH_MSG_REQUEST_SUCCESS = 81, SSH_MSG_REQUEST_FAILURE = 82,
    SSH_MSG_CHANNEL_OPEN = 90, SSH_MSG_CHANNEL_OPEN_CONFIRMATION = 91,
    SSH_MSG_CHANNEL_OPEN_FAILURE = 92, SSH_MSG_CHANNEL_WINDOW_ADJUST = 93,
    SSH_MSG_CHANNEL_DATA = 94, SSH_MSG_CHANNEL_EXTENDED_DATA = 95, SSH_MSG_CHANNEL_EOF = 96,
    SSH_MSG_CHANNEL_CLOSE = 97, SSH_MSG_CHANNEL_REQUEST = 98, SSH_MSG_CHANNEL_SUCCESS = 99,
    SSH_MSG_CHANNEL_FAILURE = 100,
};

// Disconnect reasons, RFC 4250 section 4.2.2.
enum {
    SSH_DISC_PROTOCOL_ERROR = 2, SSH_DISC_KEX_FAILED = 3, SSH_DISC_MAC_ERROR = 5,
    SSH_DISC_SERVICE_NOT_AVAILABLE = 7, SSH_DISC_VERSION_NOT_SUPPORTED = 8,
    SSH_DISC_HOST_KEY_NOT_VERIFIABLE = 9, SSH_DISC_CONNECTION_LOST = 10,
    SSH_DISC_BY_APPLICATION = 11, SSH_DISC_TOO_MANY_CONNECTIONS = 12,
    SSH_DISC_AUTH_CANCELLED = 13, SSH_DISC_NO_MORE_AUTH_METHODS = 14,
};

// The largest packet accepted. RFC 4253 section 6.1 says an implementation
// MUST handle 35000 bytes, and this does — the input buffer grows to it for the
// one packet that needs it and the connection is refused if the memory is not
// there. Everyday packets are small because the channel window and packet size
// this side advertises are small.
#define SSH_PKT_MAX        35000u
#define SSH_IN_START       4096u     // input buffer to begin with
#define SSH_OUT_CAP        4096u     // encrypted bytes waiting for the socket
// What has to be free in the output buffer before another input packet is
// handled: the largest reply any one packet can provoke (a key exchange reply
// with its signature, or our KEXINIT plus a NEWKEYS) with room to spare.
#define SSH_OUT_RESERVE    1536u
// Service messages produced while a key exchange is running wait here until it
// finishes (RFC 4253 section 7.1 forbids sending them in between).
#define SSH_DEFER_CAP      768u

// Rekey after this many bytes either way, well inside RFC 4344's limit for a
// 128-bit block cipher and RFC 4253 section 9's one gigabyte.
#define SSH_REKEY_BYTES    (1u << 30)
#define SSH_REKEY_PACKETS  (1u << 28)

enum SshState {
    SSH_ST_VERSION = 0,     // waiting for the peer's version line
    SSH_ST_KEX,             // the first key exchange
    SSH_ST_OPEN,            // encrypted; the service layer is running
    SSH_ST_CLOSED,          // finished; nothing more will be read
};

struct SshConn;
// The service layer's handler. `type` is the message number; `r` is
// positioned just after it and covers the rest of the payload. Returns false
// for a message it does not know, which is answered with UNIMPLEMENTED.
//
// Type 0 is not a message: it is called once, with an empty reader, when the
// first key exchange has finished and the service layer may start.
typedef bool (*SshDispatchFn)(SshConn *c, uint8_t type, SshR *r);
// Client only: is this host key the one expected? Return true to continue.
typedef bool (*SshHostKeyFn)(SshConn *c, const uint8_t *blob, uint32_t n);

struct SshDir {          // one direction of the packet stream
    SshCipher cipher;
    uint32_t  seq;
    uint32_t  packets;   // since the last key exchange
    uint32_t  bytes;
};

struct SshKeys {         // what NEWKEYS will take into use
    int     enc;
    int     mac;
    uint8_t key[32];
    uint8_t iv[16];
    uint8_t mac_key[32];
};

struct SshConn {
    bool     server;
    uint8_t  state;              // SshState
    uint8_t  disc_reason;        // set when the connection ended
    char     why[80];            // what ended it, for a log line

    // The version lines without CR LF, kept because both go into the hash.
    char     v_mine[64];
    char     v_peer[256];
    uint8_t  vline_len;          // bytes of a version line collected so far
    uint16_t pre_lines;          // lines before the version (server banners)

    uint8_t *in;  uint32_t in_cap, in_len;
    uint8_t *out; uint32_t out_len;
    bool     len_known;          // the current packet's first block is decrypted
    uint8_t  defer[SSH_DEFER_CAP];
    uint32_t defer_len;

    SshDir   rx, tx;

    // --- key exchange ---
    bool     kex_running;        // a KEXINIT has been sent or received
    bool     kexinit_sent, kexinit_recv;
    bool     newkeys_sent, newkeys_recv;
    bool     ecdh_done;          // the one INIT/REPLY of this exchange happened
    bool     ignore_next;        // the peer guessed wrong (first_kex_packet_follows)
    bool     strict;             // strict KEX agreed
    bool     first_kex_done;
    bool     peer_ext_info;      // client offered ext-info-c
    bool     peer_strict_offer;
    int      kex_alg;
    char     hostkey_alg[24];    // negotiated host key algorithm
    uint8_t *i_mine; uint32_t i_mine_n;   // our KEXINIT payload, for the hash
    uint8_t *i_peer; uint32_t i_peer_n;
    SshEcdh  ecdh;
    uint8_t  q_mine[65]; uint32_t q_mine_n;   // client: its ephemeral public
    SshKeys  next_tx, next_rx;
    uint8_t  session_id[32];
    bool     have_session_id;
    int      enc_cs, enc_sc, mac_cs, mac_sc;

    // --- role specific ---
    const SshHostKey *hostkey;   // server
    SshHostKeyFn hostkey_check;  // client
    uint8_t  peer_hostkey[600];  // client: the key the server presented
    uint32_t peer_hostkey_n;

    // Client: the host key algorithms to offer, most wanted first. Null for
    // the default; set to put the type a known_hosts entry has first, so a
    // host that has several keys presents the one already recorded.
    const char *hostkey_algs;

    // The packet being built between ssh_begin and ssh_end.
    SshW     w;
    uint32_t w_start;
    bool     w_defer;
    bool     w_open;

    SshDispatchFn dispatch;
    void    *owner;              // the service layer's own state
};

// Set up a connection. Allocates the two buffers (about 10 KB); false if the
// memory is not there. `ident` is the software part of our version line.
bool ssh_conn_init(SshConn *c, bool server, const char *ident);
void ssh_conn_free(SshConn *c);

// Bytes from the socket. Returns how many were taken: fewer than offered when
// the input buffer is full, in which case the rest should be offered again
// after ssh_process has made room.
uint32_t ssh_feed(SshConn *c, const uint8_t *data, uint32_t n);
// Handle every complete packet that is waiting, while there is room to answer.
void ssh_process(SshConn *c);

// Encrypted bytes waiting for the socket, and how many were sent.
const uint8_t *ssh_out(const SshConn *c, uint32_t *n);
void           ssh_out_consume(SshConn *c, uint32_t n);
uint32_t       ssh_out_room(const SshConn *c);

// Building a packet to send. The payload is written straight into the output
// buffer between begin and end, so nothing is copied. ssh_end returns false
// when the payload did not fit, which ends the connection.
SshW *ssh_begin(SshConn *c, uint8_t type);
bool  ssh_end(SshConn *c);

// May the service layer send bulk data now? False during a key exchange this
// side has joined, when RFC 4253 7.1 forbids it, and when the output buffer is
// short of room.
bool ssh_can_send(const SshConn *c, uint32_t need);

// End the connection: a DISCONNECT goes out (when the transport is far enough
// along to send one) and nothing further is read.
void ssh_disconnect(SshConn *c, uint32_t reason, const char *why);

// Start a key exchange this side wants (re-keying). No effect if one is running.
void ssh_rekey(SshConn *c);

bool ssh_open(const SshConn *c);     // encrypted and running
bool ssh_closed(const SshConn *c);

#endif  // RPC_SSHTRANS_H
