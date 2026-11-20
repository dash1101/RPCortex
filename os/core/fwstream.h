// A firmware image kept deflated in flash and inflated as it is read.
//
// Written for the CYW43 radio's firmware (cyw43_fw.cpp), which was the largest
// single object in a wireless image: 227 KB stored as it is, about 148 KB
// deflated. Nothing in here knows about radios. It is the reader and its checks,
// and it is host-tested against the real firmware (host/fwstream_test.cpp).
//
// THE CHECK COMES BEFORE THE BYTES. The image is inflated one window at a time,
// and each window's CRC-32, which the generator (tools_cyw43fw.py) took from the
// original, is compared before any byte of that window is handed out. A damaged
// stream, or a decompressor gone wrong, stops the reader at the first bad window
// having handed out only bytes that matched. A single check over the whole image
// at the end would come too late: by then the bytes would have been sent, and
// sending them is the thing that must not happen.
//
// The window is also the deflate window. The generator compresses with a 2 KB
// window, so no back-reference reaches further than this buffer holds; the
// output lands in the buffer directly, and the buffer is the dictionary the next
// window is inflated from. The whole reader is that buffer plus uzlib's state,
// taken from the heap at open and given back at close (fwstream_footprint).
//
// Reads are sequential and never span two windows. A caller reading in blocks
// that divide the window (the CYW43 bus moves 64 bytes at a time) never notices;
// one that does not is refused rather than handed a short block.
//
// The inflater is uzlib, as MicroPython ships it in lib/uzlib (zlib licence; by
// Joergen Ibsen, Paul Sokolovsky and Jim Mussared), kept unmodified in os/uzlib/.
#ifndef RPC_FWSTREAM_H
#define RPC_FWSTREAM_H

#include <stdint.h>

#define FWSTREAM_WINDOW 2048    // must match WINDOW_BITS in tools_cyw43fw.py

// One stored image, as the generator writes it.
struct FwImage {
    const char     *name;       // for messages: "Wi-Fi firmware"
    const uint8_t  *z;          // raw deflate, no zlib or gzip header
    uint32_t        z_len;
    uint32_t        len;        // bytes it inflates to
    const uint32_t *crc;        // CRC-32 of each FWSTREAM_WINDOW bytes; the last may cover fewer
};

enum FwStreamError {
    FWS_OK = 0,
    FWS_CORRUPT,        // the deflate stream did not decode
    FWS_SHORT,          // it ended, or its input ran out, before the image did
    FWS_CRC,            // a window inflated to the wrong bytes
    FWS_TRAILING,       // it went on after the image should have ended
    FWS_BAD_READ,       // asked for nothing, past the end, or across two windows
    FWS_UNREAD,         // closed before every byte was read
};

struct FwStream;

// NULL when the heap cannot spare fwstream_footprint() bytes. Nothing is
// inflated yet; the first read does that.
FwStream *fwstream_open(const FwImage *img);

// The next n bytes of the image, checked. The pointer is good until the next
// read or the close. NULL on any failure, and every read after one fails too.
const uint8_t *fwstream_read(FwStream *s, uint32_t n);

// Gives the memory back. FWS_OK only if every byte was read and every check
// passed. `at`, when given, is how far the reader had got: for a failed window,
// where that window starts.
int fwstream_close(FwStream *s, uint32_t *at);

const char *fwstream_name(const FwStream *s);
const char *fwstream_strerror(int err);

// Heap taken by one open stream. One is open at a time.
uint32_t fwstream_footprint(void);

// CRC-32 as zlib, zip and PNG compute it.
uint32_t fwstream_crc32(const void *data, uint32_t len);

#endif  // RPC_FWSTREAM_H
