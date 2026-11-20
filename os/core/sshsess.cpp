#include "sshsess.h"

#include <string.h>

static inline uint32_t ld_acq(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void     st_rel(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

void ssh_ring_init(SshRing *r, uint8_t *buf, uint32_t cap) {
    r->buf = buf; r->cap = cap; r->head = 0; r->tail = 0;
}

// head and tail run freely and wrap at 2^32; their difference is the fill,
// which is why the capacity has to be a power of two.
uint32_t ssh_ring_used(const SshRing *r) { return ld_acq(&r->head) - ld_acq(&r->tail); }
uint32_t ssh_ring_free(const SshRing *r) { return r->cap - ssh_ring_used(r); }

uint32_t ssh_ring_put(SshRing *r, const uint8_t *d, uint32_t n) {
    uint32_t head = r->head;                   // ours: no barrier needed to read it
    uint32_t room = r->cap - (head - ld_acq(&r->tail));
    if (n > room) n = room;
    for (uint32_t i = 0; i < n; i++) r->buf[(head + i) & (r->cap - 1)] = d[i];
    st_rel(&r->head, head + n);                // the bytes before the index
    return n;
}

uint32_t ssh_ring_get(SshRing *r, uint8_t *d, uint32_t n) {
    uint32_t tail = r->tail;
    uint32_t have = ld_acq(&r->head) - tail;
    if (n > have) n = have;
    for (uint32_t i = 0; i < n; i++) d[i] = r->buf[(tail + i) & (r->cap - 1)];
    st_rel(&r->tail, tail + n);
    return n;
}

uint32_t ssh_ring_peek(const SshRing *r, const uint8_t **p) {
    uint32_t tail = r->tail;
    uint32_t have = ld_acq(&r->head) - tail;
    uint32_t at = tail & (r->cap - 1);
    uint32_t first = r->cap - at;
    *p = r->buf + at;
    return have < first ? have : first;
}

void ssh_ring_drop(SshRing *r, uint32_t n) {
    uint32_t tail = r->tail;
    uint32_t have = ld_acq(&r->head) - tail;
    if (n > have) n = have;
    st_rel(&r->tail, tail + n);
}

void ssh_sess_init(SshSess *s, bool pty) {
    memset(s, 0, sizeof(*s));
    ssh_ring_init(&s->in, s->inbuf, SSH_SESS_IN);
    ssh_ring_init(&s->out, s->outbuf, SSH_SESS_OUT);
    s->pty = pty;
    s->cols = 80; s->rows = 24;
}

uint32_t ssh_sess_input(SshSess *s, const uint8_t *d, uint32_t n) {
    uint32_t dropped = 0;
    uint32_t i = 0;
    while (i < n) {
        if (s->running) {
            // Find the next Ctrl+C; everything before it is type-ahead.
            uint32_t j = i;
            while (j < n && d[j] != 0x03) j++;
            uint32_t put = ssh_ring_put(&s->in, d + i, j - i);
            dropped += (j - i) - put;        // cannot happen while the window holds
            if (j < n) { s->intr = true; dropped++; j++; }
            i = j;
        } else {
            uint32_t put = ssh_ring_put(&s->in, d + i, n - i);
            dropped += (n - i) - put;
            i = n;
        }
    }
    return dropped;
}

int ssh_sess_getch(SshSess *s) {
    uint8_t c;
    if (ssh_ring_get(&s->in, &c, 1) != 1) return -1;
    __atomic_add_fetch(&s->consumed, 1, __ATOMIC_RELEASE);
    return c;
}

uint32_t ssh_sess_write(SshSess *s, const char *d, uint32_t n) {
    if (!s->pty) return ssh_ring_put(&s->out, (const uint8_t *)d, n);
    uint32_t i = 0;
    while (i < n) {
        // A run with no \n in it goes as it is.
        uint32_t j = i;
        while (j < n && d[j] != '\n') j++;
        if (j > i) {
            uint32_t want = j - i;
            uint32_t put = ssh_ring_put(&s->out, (const uint8_t *)d + i, want);
            if (put) s->last = (uint8_t)d[i + put - 1];
            i += put;
            if (put < want) return i;           // the ring filled part way
        }
        if (i >= n) break;
        // A \n, preceded by \r unless one was just written.
        if (s->last == '\r') {
            if (ssh_ring_put(&s->out, (const uint8_t *)"\n", 1) != 1) return i;
        } else {
            if (ssh_ring_free(&s->out) < 2) return i;
            ssh_ring_put(&s->out, (const uint8_t *)"\r\n", 2);
        }
        s->last = '\n';
        i++;
    }
    return i;
}
