// A stream written into a flash region one whole sector at a time. See stagesink.h.
#include "stagesink.h"

#include <string.h>

void stage_begin(StageSink *s, StageSectorFn write, void *ctx, uint8_t *buf,
                 uint32_t sector, uint32_t cap) {
    s->write   = write;
    s->ctx     = ctx;
    s->buf     = buf;
    s->sector  = sector;
    s->cap     = sector ? cap - cap % sector : 0;
    s->held    = 0;
    s->written = 0;
    s->total   = 0;
    s->ok      = write && buf && sector && s->cap;
    sha256_init(&s->sha);
}

static bool flush(StageSink *s) {
    if (!s->held) return s->ok;
    // Never past the region, whatever the stream says: the next thing after the
    // staging area on this flash chip is the filesystem.
    if (s->written + s->sector > s->cap) { s->ok = false; return false; }
    if (s->held < s->sector) memset(s->buf + s->held, 0xFF, s->sector - s->held);
    if (!s->write(s->ctx, s->written, s->buf)) { s->ok = false; return false; }
    s->written += s->sector;
    s->held = 0;
    return true;
}

bool stage_write(StageSink *s, const void *data, uint32_t n) {
    if (!s->ok) return false;
    // Refused before anything of it is taken, so a stream one byte too long does
    // not leave a sector of it in flash.
    if (n > s->cap - s->total) { s->ok = false; return false; }
    const uint8_t *p = (const uint8_t *)data;
    sha256_update(&s->sha, p, n);
    s->total += n;
    while (n) {
        uint32_t room = s->sector - s->held;
        uint32_t take = n < room ? n : room;
        memcpy(s->buf + s->held, p, take);
        s->held += take;
        p += take;
        n -= take;
        if (s->held == s->sector && !flush(s)) return false;
    }
    return true;
}

bool stage_finish(StageSink *s, uint8_t digest[SHA256_DIGEST_LEN], uint32_t *total) {
    if (s->ok) flush(s);
    sha256_final(&s->sha, digest);
    if (total) *total = s->total;
    return s->ok && s->total > 0;
}
