// A firmware image kept deflated in flash and inflated as it is read.
// See fwstream.h for why each window is checked before it is handed out.
#include "fwstream.h"

#include <stdlib.h>

#include "uzlib.h"

struct FwStream {
    uzlib_uncomp_t d;
    const FwImage *img;
    uint32_t base;      // image offset of win[0]
    uint32_t fill;      // bytes of the image in win now
    uint32_t pos;       // bytes handed out so far
    int      err;       // the first failure; every read after it fails too
    // Where uzlib writes its output and also its dictionary: the same bytes.
    uint8_t  win[FWSTREAM_WINDOW];
};

uint32_t fwstream_crc32(const void *data, uint32_t len) {
    return uzlib_crc32(data, len, 0xffffffffu) ^ 0xffffffffu;
}

uint32_t fwstream_footprint(void) { return (uint32_t)sizeof(FwStream); }

FwStream *fwstream_open(const FwImage *img) {
    // calloc rather than malloc: a damaged stream can refer back to parts of the
    // window not yet written, and those should read as zeros, not as whatever
    // the heap held before. The CRC refuses the window either way.
    FwStream *s = (FwStream *)calloc(1, sizeof(FwStream));
    if (!s) return nullptr;
    s->img = img;
    uzlib_uncompress_init(&s->d, s->win, FWSTREAM_WINDOW);
    s->d.source = img->z;
    s->d.source_limit = img->z + img->z_len;
    return s;
}

// Inflate the next window into win and check it. Called only once the previous
// window has been handed out in full.
//
// The output pointer and uzlib's dictionary index move together through win,
// because every window but the last is exactly FWSTREAM_WINDOW long: each one
// starts at win[0] with the index back at 0, writes the same byte to the same
// place twice, and reads its back-references from the window before it, which
// is still there until it is overwritten one byte at a time.
static int fill(FwStream *s) {
    const FwImage *img = s->img;
    uzlib_uncomp_t *d = &s->d;
    uint32_t start = s->base + s->fill;
    uint32_t n = img->len - start;
    if (n > FWSTREAM_WINDOW) n = FWSTREAM_WINDOW;
    bool last = start + n == img->len;

    if (d->dict_idx != 0) return FWS_CORRUPT;     // cannot happen: windows are whole
    // n is never 0 here, which matters: uzlib's loop writes one byte before it
    // looks at the limit.
    d->dest_start = d->dest = s->win;
    d->dest_limit = s->win + n;
    int r = uzlib_uncompress(d);
    if (r < 0) return FWS_CORRUPT;
    // uzlib reads zeros past the end of its input and says so only in eof; a
    // stored block would otherwise inflate a truncated stream into zeros.
    if (d->eof || d->dest != d->dest_limit) return FWS_SHORT;
    if (r == UZLIB_DONE && !last) return FWS_SHORT;
    if (fwstream_crc32(s->win, n) != img->crc[start / FWSTREAM_WINDOW]) return FWS_CRC;

    if (last) {
        // The image is complete; the stream has to be too. uzlib stops when the
        // output is full, before the end-of-block that follows, so one more byte
        // is asked for: a whole stream answers DONE and gives nothing. (One that
        // needs input it does not have for that answers with an error, since
        // uzlib checks for running out after every code.) Then the input has to
        // be used up exactly, or there is something after the stream.
        if (r != UZLIB_DONE) {
            uint8_t probe;
            d->dest_start = d->dest = &probe;
            d->dest_limit = &probe + 1;
            r = uzlib_uncompress(d);
            if (r != UZLIB_DONE || d->dest != &probe) return FWS_TRAILING;
        }
        if (d->source != d->source_limit) return FWS_TRAILING;
    }
    s->base = start;
    s->fill = n;
    return FWS_OK;
}

const uint8_t *fwstream_read(FwStream *s, uint32_t n) {
    if (!s || s->err) return nullptr;
    if (n == 0 || n > s->img->len - s->pos) { s->err = FWS_BAD_READ; return nullptr; }
    if (s->pos == s->base + s->fill) {
        int e = fill(s);
        if (e) { s->err = e; return nullptr; }
    }
    uint32_t off = s->pos - s->base;
    if (n > s->fill - off) { s->err = FWS_BAD_READ; return nullptr; }
    s->pos += n;
    return s->win + off;
}

int fwstream_close(FwStream *s, uint32_t *at) {
    if (!s) return FWS_BAD_READ;
    int e = s->err;
    if (!e && s->pos != s->img->len) e = FWS_UNREAD;
    if (at) *at = s->pos;
    free(s);
    return e;
}

const char *fwstream_name(const FwStream *s) {
    return s && s->img->name ? s->img->name : "image";
}

const char *fwstream_strerror(int err) {
    switch (err) {
        case FWS_OK:       return "ok";
        case FWS_CORRUPT:  return "does not decode";
        case FWS_SHORT:    return "ends early";
        case FWS_CRC:      return "wrong CRC";
        case FWS_TRAILING: return "runs on";
        case FWS_BAD_READ: return "bad read";
        case FWS_UNREAD:   return "not all read";
    }
    return "?";
}
