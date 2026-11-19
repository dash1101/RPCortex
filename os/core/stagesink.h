// A stream written into a flash region one whole sector at a time.
//
// An update used to be downloaded to a file and then copied into the staging
// area, so the filesystem had to hold the image AND, afterwards, the copy of the
// running firmware kept for rollback: two images of about 850 KB on a 1.8 MB
// filesystem that also holds packages. It never fit, and rollback quietly did not
// happen (PAR-54). Written straight into the staging area instead, the download
// costs the filesystem nothing and the rollback copy has room.
//
// Flash is erased a sector at a time and can only be programmed where it has been
// erased, so the stream is gathered into one sector's worth of RAM and each full
// sector is erased and programmed in one step. The tail is padded with 0xFF, the
// erased state. A running SHA-256 is kept of exactly the bytes given, so the
// caller can check the download against the manifest without reading anything
// back, and then check what landed in flash against the same digest.
//
// Pure: the sector write comes in as a function, so every way this can go wrong
// (a stream that does not end on a sector, one longer than the region, a write
// that fails half way) is reachable on a host against a model of the flash.
#ifndef RPC_STAGESINK_H
#define RPC_STAGESINK_H

#include <stdint.h>

#include "sha256.h"

// Erase and program one whole sector at byte offset `off` within the region.
// False if it did not happen.
typedef bool (*StageSectorFn)(void *ctx, uint32_t off, const uint8_t *sector);

struct StageSink {
    StageSectorFn write;
    void         *ctx;
    uint8_t      *buf;          // one sector, the caller's
    uint32_t      sector;       // sector size
    uint32_t      cap;          // region size; nothing is written past it
    uint32_t      held;         // bytes gathered, not yet written
    uint32_t      written;      // bytes committed to flash, whole sectors
    uint32_t      total;        // bytes accepted from the stream
    bool          ok;
    Sha256Ctx     sha;
};

// `buf` must be `sector` bytes and outlive the sink. `cap` is rounded down to a
// whole number of sectors.
void stage_begin(StageSink *s, StageSectorFn write, void *ctx, uint8_t *buf,
                 uint32_t sector, uint32_t cap);

// Take `n` more bytes. False once anything has failed or the stream has grown
// past the region; it stays false, and nothing more is written.
bool stage_write(StageSink *s, const void *data, uint32_t n);

// Write the last partial sector, padded with 0xFF, and hand back the digest of
// the stream and its length. False if anything failed along the way, or if the
// stream was empty, which is never an image.
bool stage_finish(StageSink *s, uint8_t digest[SHA256_DIGEST_LEN], uint32_t *total);

#endif  // RPC_STAGESINK_H
