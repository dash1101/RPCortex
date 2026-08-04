// The audio ring.
//
// It sits between an interrupt and a timer with no lock between them, which is
// safe only because there is exactly one writer and one reader. Everything that
// makes that true is arithmetic, so it is all testable here — and worth it,
// because the failure mode on hardware is "the music sounds slightly wrong",
// which is close to undiagnosable.
#include "../core/pcmring.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

int main(void) {
    printf("pcmring_test - the audio ring\n");

    static int16_t mem[16];
    PcmRing r;
    int16_t out[16];

    // --- capacity -----------------------------------------------------------
    pcm_ring_init(&r, mem, 16);
    ck(pcm_ring_used(&r) == 0, "a new ring is empty");
    // One slot is always kept empty, which is what tells full from empty
    // without a counter both sides would have to write.
    ck(pcm_ring_free(&r) == 15, "and holds one less than its size");

    // A size that is not a power of two rounds DOWN, because the wrap is a mask.
    // Rounding up would index past the buffer it was given.
    pcm_ring_init(&r, mem, 12);
    ck(pcm_ring_free(&r) == 7, "a non-power-of-two size rounds down, not up");

    // --- the ordinary path --------------------------------------------------
    pcm_ring_init(&r, mem, 16);
    int16_t in[4] = { 100, 200, 300, 400 };
    ck(pcm_ring_write(&r, in, 4) == 4, "four samples go in");
    ck(pcm_ring_used(&r) == 4, "and are counted");
    ck(pcm_ring_read(&r, out, 4) == 4, "and come back out");
    ck(memcmp(in, out, sizeof(in)) == 0, "unchanged and in order");
    ck(pcm_ring_used(&r) == 0, "leaving it empty");

    // --- underrun -----------------------------------------------------------
    //
    // Reading more than is there returns NOTHING, not a partial frame. Half a
    // stereo pair would swap the channels for every sample after it.
    pcm_ring_init(&r, mem, 16);
    pcm_ring_write(&r, in, 1);
    ck(pcm_ring_read(&r, out, 2) == 0, "a partial frame is refused, not half read");
    ck(pcm_ring_used(&r) == 1, "and the sample stays for when its pair arrives");
    ck(pcm_ring_read(&r, out, 0) == 0, "asking for nothing gets nothing");

    // --- wrapping -----------------------------------------------------------
    //
    // Where an off-by-one lives. Filling and draining several times over walks
    // the indices past the buffer end repeatedly.
    pcm_ring_init(&r, mem, 16);
    for (int round = 0; round < 10; round++) {
        int16_t v[5];
        for (int i = 0; i < 5; i++) v[i] = (int16_t)(round * 10 + i);
        ck(pcm_ring_write(&r, v, 5) == 5, "writing across the wrap works");
        int16_t g[5];
        ck(pcm_ring_read(&r, g, 5) == 5, "and reading back across it");
        ck(memcmp(v, g, sizeof(v)) == 0, "with the samples intact");
    }

    // --- full -----------------------------------------------------------------
    //
    // The deliberate choice: drop the OLDEST. What is already in the ring is
    // late, and keeping it makes everything behind it later still.
    pcm_ring_init(&r, mem, 16);
    int16_t many[20];
    for (int i = 0; i < 20; i++) many[i] = (int16_t)i;
    pcm_ring_write(&r, many, 20);
    ck(pcm_ring_used(&r) == 15, "an over-full write fills the ring");
    ck(r.dropped > 0, "and records what it had to drop");

    // What survived must be the NEWEST samples. If the oldest survived instead,
    // the output would be permanently behind the input and never catch up.
    int16_t got[15];
    pcm_ring_read(&r, got, 15);
    ck(got[14] == 19, "the newest sample survives");
    ck(got[0] == 5,   "and the oldest were the ones dropped");

    // --- clear ----------------------------------------------------------------
    pcm_ring_init(&r, mem, 16);
    pcm_ring_write(&r, in, 4);
    pcm_ring_clear(&r);
    ck(pcm_ring_used(&r) == 0, "clearing empties it");
    ck(pcm_ring_free(&r) == 15, "and gives the space back");
    ck(pcm_ring_write(&r, in, 4) == 4, "and it works afterwards");

    // Clearing a full ring must empty it rather than leave it looking full,
    // which is the state a pause would leave behind.
    pcm_ring_init(&r, mem, 16);
    pcm_ring_write(&r, many, 20);
    pcm_ring_clear(&r);
    ck(pcm_ring_used(&r) == 0, "clearing a full ring empties it too");

    // --- the shape a stereo stream actually has -------------------------------
    //
    // Written in bursts of many samples, read two at a time. The sizes never
    // divide evenly, which is exactly when an index mistake shows up.
    pcm_ring_init(&r, mem, 16);
    int total_read = 0;
    for (int burst = 0; burst < 30; burst++) {
        int16_t chunk[6];
        for (int i = 0; i < 6; i++) chunk[i] = (int16_t)(burst * 6 + i);
        pcm_ring_write(&r, chunk, 6);
        int16_t pair[2];
        while (pcm_ring_read(&r, pair, 2) == 2) total_read += 2;
    }
    ck(total_read > 150, "a bursty writer and a paired reader keep up");
    ck(pcm_ring_used(&r) < 2, "and nothing is left stranded");

    // --- nothing to work with -------------------------------------------------
    pcm_ring_init(&r, nullptr, 16);
    ck(pcm_ring_write(&r, in, 4) == 0, "a ring with no buffer accepts nothing");
    ck(pcm_ring_read(&r, out, 4) == 0, "and returns nothing");
    ck(pcm_ring_used(&r) == 0, "and is empty");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
