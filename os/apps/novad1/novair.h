// Desc: Flipper Zero .ir files, both ways, and the remote protocols they can name.
// File: novair.h
//
// Ported from novair.py (parse_flipper, to_flipper, append_signal, encode), the
// oracle the host test compares against. A signal is a list of microsecond
// durations, mark first, plus a carrier frequency; a .ir file holds many named
// signals, either RAW (the list itself) or PARSED (a protocol, address and
// command, which encode() turns into the list).
//
// Only the protocols novair.py encoded are here. Pioneer's byte layout is INFERRED
// (its timings are the Flipper firmware's, the address/command layout is the NEC
// one by analogy) and no remote has confirmed it: DEVICE-UNCONFIRMED.
#ifndef NOVA_IR_H
#define NOVA_IR_H

#include "novacodec.h"

namespace nova {
namespace ir {

#define IR_NAME_MAX 24
#define IR_DUTY_DEFAULT_PPM 330000u      // ~33%, the standard IR duty cycle

struct Signal {
    char     name[IR_NAME_MAX];
    uint32_t freq_hz;
    uint32_t duty_ppm;       // millionths: 330000 is 0.33
    unsigned n;              // how many durations are in the array it was given
    bool     truncated;      // the file had more than the array could hold
};

// Encode a parsed signal to durations. `address` and `command` are the hex-byte
// text a .ir writes ("04 00 00 00"). Returns the count, 0 when the protocol is not
// one of NEC, NECext, Samsung32, Pioneer, SIRC (12/15/20), RC5, RC5X, RC6. *freq_hz
// is set to the carrier the protocol uses.
unsigned encode(const char *protocol, const char *address, const char *command,
                uint32_t *freq_hz, uint32_t *out, unsigned cap);

// The next signal in a .ir file's text, starting at *cursor, which is advanced so
// the next call carries on. Returns false at the end. Durations go in times[0..cap).
bool next(const char **cursor, Signal *sig, uint32_t *times, unsigned cap);

// "Filetype: IR signals file\nVersion: 1\n" - the top of a new file.
bool write_header(const codec::Sink &k);

// One RAW signal block, written after the header or appended to an existing file
// (which must already end in a newline).
bool write_signal(const codec::Sink &k, const char *name, const uint32_t *times,
                  unsigned n, uint32_t freq_hz, uint32_t duty_ppm);

}  // namespace ir
}  // namespace nova

#endif  // NOVA_IR_H
