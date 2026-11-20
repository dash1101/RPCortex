// The SSH client's service layer: authenticate, open one session channel,
// ask for a terminal and a shell (or run one command), then move bytes.
//
// Authentication tries, in order, whatever the server lists and this side
// has: the device's own key (publickey, ecdsa-sha2-nistp256) when one is
// given, then "password", then "keyboard-interactive" for servers that only
// do passwords that way. Every secret is asked of the caller through
// SshClientOps at the moment it is needed and never kept.
#ifndef RPC_SSHCLIENT_H
#define RPC_SSHCLIENT_H

#include "sshtrans.h"

#define SSH_CLI_WINDOW   8192u    // what the server may send before we catch up
#define SSH_CLI_MAXPKT   4096u
#define SSH_CLI_CHUNK    1024u    // the most typed input sent in one packet
#define SSH_CLI_PROMPTS  3        // password prompts before giving up

struct SshClientOps {
    // The server's host key, before anything else is believed. True to go on.
    bool (*hostkey)(void *ctx, const uint8_t *blob, uint32_t n);
    // A secret, asked for with this prompt; `echo` says whether what is typed
    // may be shown. False gives up.
    bool (*prompt)(void *ctx, const char *prompt, bool echo, char *out, uint32_t cap);
    void (*banner)(void *ctx, const char *text);
    void (*on_data)(void *ctx, const uint8_t *d, uint32_t n, bool err_stream);
};

enum SshCliStage {
    SSH_CLI_CONNECTING = 0,
    SSH_CLI_AUTH,
    SSH_CLI_OPENING,        // authenticated; channel being set up
    SSH_CLI_RUNNING,        // shell or command started
    SSH_CLI_DONE,           // channel closed, or it never came up
};

struct SshClient {
    SshConn c;
    const SshClientOps *ops;
    void *ctx;

    char     user[33];
    const SshHostKey *idkey;    // optional: the device's own key
    uint8_t  stage;
    bool     tried_key, tried_kbd;
    uint8_t  pw_prompts;
    bool     kbd_active;        // the last request was keyboard-interactive
    char     failed[96];        // why authentication ended, for the message

    // What to ask for once the channel is open.
    bool     want_pty;
    char     term[24];
    uint32_t cols, rows;
    const char *command;        // null for an interactive shell

    uint32_t peer_id, peer_window, peer_maxpkt;
    uint32_t local_window, regrant;
    uint8_t  replies;           // requests sent with want_reply, unanswered
    bool     eof_in, close_in, close_out;
    bool     have_status;
    int      exit_status;
};

bool ssh_cli_init(SshClient *k, const char *user, const SshClientOps *ops, void *ctx);
void ssh_cli_free(SshClient *k);

// Typed input to the remote side; returns how much was taken now.
uint32_t ssh_cli_send(SshClient *k, const uint8_t *d, uint32_t n);
// The terminal changed size.
void ssh_cli_winch(SshClient *k, uint32_t cols, uint32_t rows);
// Our side is finished (Ctrl+] or no more input for a command).
void ssh_cli_close(SshClient *k);

#endif  // RPC_SSHCLIENT_H
