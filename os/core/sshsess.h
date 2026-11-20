// What sits between an SSH channel and the shell that serves it.
//
// Two tasks share one login: the one that owns the connection (it reads the
// socket, decrypts, encrypts and writes) and the one that runs the shell (the
// line editor and every command typed). They meet here, in two rings and a
// handful of flags, and nowhere else. The shell task never touches the network
// and the network task never runs a command — which is what keeps a command
// holding the filesystem lock from ever waiting on the network lock, the
// ordering that a download already takes the other way round.
//
// Each ring has one writer and one reader, on whichever cores they happen to
// be, so the indices are published with release stores and read with acquire
// loads. On a host those are fences the compiler honours; on a Cortex-M33 they
// are DMBs. Volatile alone would be neither.
//
// Pure, so the host interop test drives the same code the device runs.
#ifndef RPC_SSHSESS_H
#define RPC_SSHSESS_H

#include <stdint.h>

#define SSH_SESS_IN    2048u    // = the channel window: input never has to be refused
#define SSH_SESS_OUT   4096u

struct SshRing {
    uint8_t *buf;
    uint32_t cap;               // a power of two
    uint32_t head;              // written by the producer only
    uint32_t tail;              // written by the consumer only
};

void     ssh_ring_init(SshRing *r, uint8_t *buf, uint32_t cap);
uint32_t ssh_ring_used(const SshRing *r);
uint32_t ssh_ring_free(const SshRing *r);
uint32_t ssh_ring_put(SshRing *r, const uint8_t *d, uint32_t n);
uint32_t ssh_ring_get(SshRing *r, uint8_t *d, uint32_t n);
// The bytes at the front, without taking them: up to two pieces, because the
// ring wraps. Returns the length of the first piece.
uint32_t ssh_ring_peek(const SshRing *r, const uint8_t **p);
void     ssh_ring_drop(SshRing *r, uint32_t n);

struct SshSess {
    SshRing in, out;
    uint8_t inbuf[SSH_SESS_IN];
    uint8_t outbuf[SSH_SESS_OUT];

    bool    pty;                // a terminal: \n goes out as \r\n
    uint8_t last;               // the last byte written, for the \r\n rule

    // Shell side writes, network side reads.
    volatile bool     running;  // a command is executing
    volatile bool     done;     // the shell has finished; exit_status is set
    volatile int      exit_status;
    volatile uint32_t consumed; // input bytes taken, to reopen the window by
    volatile uint32_t lost;     // output dropped because nobody was reading

    // Network side writes, shell side reads.
    volatile bool     intr;     // Ctrl+C, or a "signal" request for INT
    volatile bool     eof;      // the peer will send nothing more
    volatile bool     hangup;   // the connection has gone: stop now
    volatile uint16_t cols, rows;
};

void ssh_sess_init(SshSess *s, bool pty);

// --- the network side ----------------------------------------------------------

// Channel data has arrived. While a command runs a Ctrl+C in it is an
// interrupt for that command rather than a byte, the way a terminal driver
// turns it into SIGINT; at the prompt it is a byte, and the line editor
// abandons the line on it. Returns how many bytes were dropped rather than
// queued, which the caller gives straight back to the window.
uint32_t ssh_sess_input(SshSess *s, const uint8_t *d, uint32_t n);

// --- the shell side --------------------------------------------------------------

// One key, or -1 when none is waiting.
int ssh_sess_getch(SshSess *s);
// Output, with \n turned into \r\n on a terminal. Returns how many of the n
// bytes were taken; fewer when the ring is full. A \n is taken whole or not at
// all, so a line ending is never split into a lone \r.
uint32_t ssh_sess_write(SshSess *s, const char *d, uint32_t n);

#endif  // RPC_SSHSESS_H
