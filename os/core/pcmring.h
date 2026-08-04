// A lock-free ring of audio samples.
//
// One side is an interrupt and the other is a timer, so a lock is not an option
// here: taking one in the radio's IRQ would stall the link, and taking one in
// the sample timer would miss samples. What makes that safe is that there is
// exactly ONE writer and exactly ONE reader, which is the case a ring can
// handle with no locking at all.
//
// The rule is that each side only ever writes its own index. The writer moves
// `head` and reads `tail`; the reader moves `tail` and reads `head`. Neither
// ever sees a half-updated value, because a 32-bit aligned load or store is
// atomic on both chips.
//
// Full is treated as "drop the oldest", not "block". Audio that arrived while
// the output was behind is audio that is already late, and holding it up makes
// everything after it late too — a click is better than a growing delay.
#ifndef RPC_PCMRING_H
#define RPC_PCMRING_H

#include <stdint.h>

struct PcmRing {
    int16_t         *buf;
    uint32_t         cap;        // in samples, and a power of two
    volatile uint32_t head;      // written by the producer only
    volatile uint32_t tail;      // written by the consumer only
    uint32_t         dropped;    // samples lost to a full ring, for diagnosis
};

// `cap` must be a power of two — the wrap is a mask rather than a modulo, which
// matters when this runs 44100 times a second.
void     pcm_ring_init(PcmRing *r, int16_t *buf, uint32_t cap);
void     pcm_ring_clear(PcmRing *r);

// Returns how many were written. Less than `n` means the ring was full and the
// oldest were dropped to make room.
uint32_t pcm_ring_write(PcmRing *r, const int16_t *src, uint32_t n);

// Returns how many were read, which is 0 when there is nothing — that is an
// underrun, and the caller decides what silence sounds like.
uint32_t pcm_ring_read(PcmRing *r, int16_t *dst, uint32_t n);

uint32_t pcm_ring_used(const PcmRing *r);
uint32_t pcm_ring_free(const PcmRing *r);

#endif  // RPC_PCMRING_H
