// The staging sink, against a model of the flash it writes.
//
// The model is strict in the one way that matters on the real part: a byte can
// only be programmed where its sector has been erased since it was last
// programmed. Writing a sector twice without an erase, or writing outside the
// region, is recorded as a fault rather than quietly accepted, so a sink that
// gets the order wrong fails here instead of producing an image that is mostly
// right.
#include "../core/stagesink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", what); fails++; }
}

#define SECTOR 4096u
#define REGION (16u * SECTOR)

struct Flash {
    uint8_t  mem[REGION];
    int      writes;
    int      faults;        // programmed past the region, or off a sector boundary
    int      fail_at;       // the Nth sector write fails (1-based), 0 never
};

static bool sector_write(void *ctx, uint32_t off, const uint8_t *sector) {
    Flash *f = (Flash *)ctx;
    f->writes++;
    if (f->fail_at && f->writes == f->fail_at) return false;
    if (off % SECTOR || off + SECTOR > REGION) { f->faults++; return false; }
    // Erase then program, as storage_usb_write_block does.
    memset(f->mem + off, 0xFF, SECTOR);
    memcpy(f->mem + off, sector, SECTOR);
    return true;
}

static void fill(uint8_t *p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; p[i] = (uint8_t)(seed >> 16); }
}

// Stream `len` bytes in chunks of `chunk` (0 = random sizes) and check what landed.
static void stream(uint32_t len, uint32_t chunk, const char *what) {
    static Flash f;
    memset(&f, 0xA5, sizeof(f.mem));        // not erased: a byte the sink never wrote stays visibly wrong
    f.writes = f.faults = f.fail_at = 0;
    uint8_t *src = (uint8_t *)malloc(len ? len : 1);
    fill(src, len, len * 7 + chunk);

    uint8_t buf[SECTOR];
    StageSink s;
    stage_begin(&s, sector_write, &f, buf, SECTOR, REGION);
    uint32_t at = 0, seed = len;
    bool ok = true;
    while (at < len && ok) {
        uint32_t n = chunk;
        if (!n) { seed = seed * 1103515245u + 12345u; n = 1 + (seed >> 8) % 1500; }
        if (n > len - at) n = len - at;
        ok = stage_write(&s, src + at, n);
        at += n;
    }
    uint8_t digest[32], want[32];
    uint32_t total = 0;
    bool fin = stage_finish(&s, digest, &total);
    sha256(src, len, want);

    char m[160];
    snprintf(m, sizeof(m), "%s: accepted and finished", what);
    ck(ok && fin, m);
    snprintf(m, sizeof(m), "%s: the stream's length comes back", what);
    ck(total == len, m);
    snprintf(m, sizeof(m), "%s: the digest is the stream's", what);
    ck(!memcmp(digest, want, 32), m);
    snprintf(m, sizeof(m), "%s: every byte landed where it belongs", what);
    ck(!memcmp(f.mem, src, len), m);
    uint32_t sectors = (len + SECTOR - 1) / SECTOR;
    bool pad = true;
    for (uint32_t i = len; i < sectors * SECTOR; i++) if (f.mem[i] != 0xFF) { pad = false; break; }
    snprintf(m, sizeof(m), "%s: the tail of the last sector is erased (0xFF)", what);
    ck(pad, m);
    bool untouched = true;
    for (uint32_t i = sectors * SECTOR; i < REGION; i++) if (f.mem[i] != 0xA5) { untouched = false; break; }
    snprintf(m, sizeof(m), "%s: nothing past the last sector was touched", what);
    ck(untouched, m);
    snprintf(m, sizeof(m), "%s: one write per sector, no faults", what);
    ck(f.writes == (int)sectors && f.faults == 0, m);
    free(src);
}

int main(void) {
    printf("stagesink_test - an update streamed into the staging area\n");

    stream(1, 1, "one byte");
    stream(SECTOR - 1, 100, "a sector less one");
    stream(SECTOR, SECTOR, "exactly a sector, in one write");
    stream(SECTOR + 1, 7, "a sector and one byte, in sevens");
    stream(5 * SECTOR + 123, 0, "five sectors and a bit, in random pieces");
    stream(REGION, 512, "the whole region, exactly");
    stream(REGION - 1, 0, "the whole region less one");

    // --- longer than the region: refused before any of it is written past the end.
    {
        static Flash f; memset(&f, 0, sizeof(f));
        uint8_t buf[SECTOR];
        StageSink s;
        stage_begin(&s, sector_write, &f, buf, SECTOR, REGION);
        static uint8_t big[REGION + 1];
        fill(big, sizeof(big), 3);
        ck(stage_write(&s, big, REGION), "a stream the size of the region is taken");
        ck(!stage_write(&s, big + REGION, 1), "one byte more is refused");
        ck(!stage_write(&s, big, 1), "and so is anything after that");
        uint8_t d[32]; uint32_t t = 0;
        ck(!stage_finish(&s, d, &t), "and the stream does not finish as an image");
        ck(f.faults == 0, "and nothing was programmed outside the region");

        StageSink s2; static Flash f2; memset(&f2, 0, sizeof(f2));
        stage_begin(&s2, sector_write, &f2, buf, SECTOR, REGION);
        ck(!stage_write(&s2, big, REGION + 1), "a single write longer than the region is refused whole");
        ck(f2.writes == 0, "without writing any of it");
    }

    // --- a sector write that fails part way: everything stops and says so.
    {
        static Flash f; memset(&f, 0, sizeof(f));
        f.fail_at = 3;
        uint8_t buf[SECTOR];
        StageSink s;
        stage_begin(&s, sector_write, &f, buf, SECTOR, REGION);
        static uint8_t data[6 * SECTOR];
        fill(data, sizeof(data), 9);
        bool ok = true;
        for (uint32_t at = 0; at < sizeof(data) && ok; at += 1000)
            ok = stage_write(&s, data + at, at + 1000 <= sizeof(data) ? 1000 : sizeof(data) - at);
        ck(!ok, "a failed sector write stops the stream");
        int writes = f.writes;
        ck(!stage_write(&s, data, 10), "and every write after it is refused");
        uint8_t d[32]; uint32_t t = 0;
        ck(!stage_finish(&s, d, &t), "and it does not finish as an image");
        ck(f.writes == writes, "with nothing more written after the failure");
    }

    // --- the tail write failing is caught at finish.
    {
        static Flash f; memset(&f, 0, sizeof(f));
        f.fail_at = 2;
        uint8_t buf[SECTOR];
        StageSink s;
        stage_begin(&s, sector_write, &f, buf, SECTOR, REGION);
        static uint8_t data[SECTOR + 10];
        ck(stage_write(&s, data, sizeof(data)), "a sector and a bit are taken");
        uint8_t d[32]; uint32_t t = 0;
        ck(!stage_finish(&s, d, &t), "a failure writing the last partial sector fails the finish");
    }

    // --- nothing, or nothing to write into.
    {
        static Flash f; memset(&f, 0, sizeof(f));
        uint8_t buf[SECTOR];
        StageSink s;
        stage_begin(&s, sector_write, &f, buf, SECTOR, REGION);
        uint8_t d[32]; uint32_t t = 99;
        ck(!stage_finish(&s, d, &t) && t == 0, "an empty stream is not an image");
        ck(f.writes == 0, "and writes nothing");

        StageSink z;
        stage_begin(&z, sector_write, &f, buf, SECTOR, SECTOR - 1);
        ck(!stage_write(&z, buf, 1), "a region smaller than a sector holds nothing");
        StageSink n;
        stage_begin(&n, nullptr, &f, buf, SECTOR, REGION);
        ck(!stage_write(&n, buf, 1), "and no writer refuses cleanly");
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
