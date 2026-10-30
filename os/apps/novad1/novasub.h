// Desc: Flipper Zero .sub files, both ways, and the fixed-code protocols they can carry.
// File: novasub.h
//
// Ported from novacc.py (parse_flipper, to_flipper, encode_decoded), which wrote
// and read these files first and is the oracle the host test compares against.
// A capture is a list of microsecond durations, mark first, the form the
// firmware's `subghz` command speaks; a .sub RAW file carries the same list with
// the sign marking mark (+) and space (-).
//
// Rolling codes are not here, on purpose: replaying one does nothing useful, and
// the firmware refuses them too.
#ifndef NOVA_SUB_H
#define NOVA_SUB_H

#include "novacodec.h"

namespace nova {
namespace sub {

#define SUB_PRESET_DEFAULT "FuriHalSubGhzPresetOok650Async"
#define SUB_FREQ_DEFAULT_HZ 915000000u

// A parsed file. Durations go into the caller's array rather than in here.
struct File {
    uint32_t freq_hz;        // 0 when the file has no Frequency line
    char     preset[40];
    char     protocol[24];   // "RAW", "Princeton", ...
    bool     has_key;
    char     key[40];        // "AA BB CC" as written
    int32_t  bits;           // -1 when absent
    int32_t  te;             // -1 when absent
};

// Parse text. Durations are written to raw[0..*raw_n) as absolute microseconds
// (the sign is dropped: parity says which is mark, which is space) and stop at
// raw_cap - the count of values that did not fit is in *raw_dropped so a caller can
// say so rather than silently truncating a capture.
void parse(const char *text, File *out, uint32_t *raw, unsigned raw_cap,
           unsigned *raw_n, unsigned *raw_dropped);

// A RAW .sub from a duration list, in the layout a Flipper expects: sign restored
// (mark +, space -), at most 512 values a line, frequency in Hz.
bool write_raw(const codec::Sink &k, const uint32_t *times, unsigned n,
               uint32_t freq_hz, const char *preset);

// Turn a DECODED file (Protocol, Key, Bit, TE) into durations so it can be
// replayed. Returns the count written, 0 when the protocol is not one of the
// fixed-code families below or the key does not fit. Supported: Princeton / PT2262,
// CAME, NICE FLO, Holtek HT12X, Ansonic, Linear MegaCode.
unsigned encode_decoded(const char *protocol, const char *key_hex, int bits, int te,
                        uint32_t *out, unsigned cap);

}  // namespace sub
}  // namespace nova

#endif  // NOVA_SUB_H
