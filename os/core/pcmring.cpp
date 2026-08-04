#include "pcmring.h"

void pcm_ring_init(PcmRing *r, int16_t *buf, uint32_t cap) {
    if (!r) return;
    r->buf = buf;
    // Round down to a power of two. A mask is one instruction where a modulo is
    // a division, and this runs once per sample.
    uint32_t p = 1;
    while (p * 2 <= cap) p *= 2;
    r->cap = buf ? p : 0;
    r->head = r->tail = 0;
    r->dropped = 0;
}

void pcm_ring_clear(PcmRing *r) {
    if (!r) return;
    // Order matters even here. Moving tail up to head empties it in one step;
    // zeroing both would briefly look full to a reader that caught it midway.
    r->tail = r->head;
}

uint32_t pcm_ring_used(const PcmRing *r) {
    if (!r || !r->cap) return 0;
    return (r->head - r->tail) & (r->cap - 1);
}

uint32_t pcm_ring_free(const PcmRing *r) {
    if (!r || !r->cap) return 0;
    // One slot is always kept empty, which is what makes full and empty
    // distinguishable without a separate count that both sides would have to
    // write — and both sides writing anything is what would need a lock.
    return r->cap - 1 - pcm_ring_used(r);
}

uint32_t pcm_ring_write(PcmRing *r, const int16_t *src, uint32_t n) {
    if (!r || !r->cap || !src) return 0;
    uint32_t mask = r->cap - 1;
    uint32_t h = r->head;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (h + 1) & mask;
        if (next == (r->tail & mask)) {
            // Full. Drop the OLDEST rather than refusing the newest: what is in
            // the ring is already late, and keeping it makes everything behind
            // it later still. A click now beats a delay that grows for ever.
            r->tail = (r->tail + 1) & mask;
            r->dropped++;
        }
        r->buf[h] = src[i];
        h = next;
    }

    // The samples are in the buffer before the index says so. Without this the
    // reader can see a head that has moved and read a slot not yet written —
    // which on this part is silence or noise rather than an error.
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->head = h;
    return n;
}

uint32_t pcm_ring_read(PcmRing *r, int16_t *dst, uint32_t n) {
    if (!r || !r->cap || !dst) return 0;
    uint32_t mask = r->cap - 1;
    uint32_t t = r->tail;
    uint32_t avail = (r->head - t) & mask;
    if (avail < n) return 0;        // a partial frame is worse than none

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = r->buf[t];
        t = (t + 1) & mask;
    }
    r->tail = t;
    return n;
}
