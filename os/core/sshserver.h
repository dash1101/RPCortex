// The SSH server's service layer: user authentication (RFC 4252) and one
// session channel (RFC 4254) on top of sshtrans.
//
// What a session can be is deliberately small. One channel per connection,
// of type "session"; a shell or one exec'd command; a pty request and window
// changes are accepted and remembered; a signal request can carry Ctrl+C.
// There is no port forwarding of any kind, no agent or X11 forwarding, no
// subsystems (so no sftp), and no environment variables — each of those is a
// way to reach further into the device than a shell already does, and none of
// them is needed to administer it.
//
// Authentication is "publickey" (ecdsa-sha2-nistp256, rsa-sha2-256/512,
// ssh-ed25519) and "password". Who may log in, and with what, is decided by the caller through
// SshServerOps; this file only applies the protocol and the attempt limit.
#ifndef RPC_SSHSERVER_H
#define RPC_SSHSERVER_H

#include "sshtrans.h"

// Attempts before the connection is dropped. "none" does not count; every
// refused password, key or signature does. OpenSSH's MaxAuthTries is the same.
#define SSH_SRV_MAX_FAILS   6
// The window and packet size this side offers. Small on purpose: what arrives
// is typing, and every byte the window allows has to have somewhere to go —
// the session's input ring is this big, so data is never refused for want of
// room and the peer is never allowed to send more than fits.
#define SSH_SRV_WINDOW      2048u
#define SSH_SRV_MAXPKT      2048u
// The largest piece of output sent in one packet.
#define SSH_SRV_CHUNK       2048u

enum SshSrvMode { SSH_MODE_NONE = 0, SSH_MODE_SHELL = 1, SSH_MODE_EXEC = 2 };

struct SshServerOps {
    // May this account log in over SSH at all? The caller says no for an
    // account that does not exist and for one with no password.
    bool (*user_ok)(void *ctx, const char *user);
    bool (*password)(void *ctx, const char *user, const char *pw);
    // Is this public key blob listed for the user?
    bool (*pubkey)(void *ctx, const char *user, const uint8_t *blob, uint32_t n);
    // Every attempt that counted, for the log.
    void (*auth_log)(void *ctx, const char *user, const char *method, bool ok);
    void (*on_start)(void *ctx);                    // a shell or a command was asked for
    void (*on_data)(void *ctx, const uint8_t *d, uint32_t n);
    void (*on_signal)(void *ctx, const char *name);
    void (*on_winch)(void *ctx);
    void (*on_eof)(void *ctx);
    void (*on_close)(void *ctx);
};

struct SshServer {
    SshConn c;
    const SshServerOps *ops;
    void *ctx;

    bool     service_ok;        // ssh-userauth was asked for and accepted
    bool     authed;
    char     user[25];
    uint8_t  fails;
    // Set after a refused attempt: the caller holds this connection (no
    // reading, no sending) for this long, so a guessing client gets one answer
    // per pause. Cleared by the caller.
    uint32_t penalty_ms;

    bool     chan;              // a session channel was opened
    uint32_t peer_id;
    uint32_t peer_window;       // what we may still send
    uint32_t peer_maxpkt;
    uint32_t local_window;      // what the peer may still send
    uint32_t regrant;           // consumed and not yet given back
    bool     eof_in, close_in, eof_out, close_out;

    uint8_t  mode;              // SshSrvMode
    bool     pty;
    char     term[32];
    uint32_t cols, rows;
    char     exec[256];
};

bool ssh_srv_init(SshServer *s, const SshHostKey *hostkey, const SshServerOps *ops, void *ctx);
void ssh_srv_free(SshServer *s);

// Send output on the channel. Returns how much was taken now (window, packet
// size, the output buffer and any key exchange all limit it); zero is not an
// error, it means "later".
uint32_t ssh_srv_send(SshServer *s, const uint8_t *d, uint32_t n);
// The caller has used n bytes of channel input; the window is reopened by
// that much (in batches, so typing does not cost a packet per key).
void ssh_srv_consumed(SshServer *s, uint32_t n);
// The session has ended: exit-status, EOF and CLOSE. False if there was no room
// yet; call again.
bool ssh_srv_exit(SshServer *s, uint32_t status);

#endif  // RPC_SSHSERVER_H
