// Reading the front of a WAV file.
//
// A2DP sends SBC, and SBC is encoded from PCM, so something has to turn a file
// into samples. WAV is the format that needs no decoder — the samples are
// already PCM and the header just says how to read them, which on a device with
// no room for an MP3 decoder is the difference between playing music and not.
//
// The header is a chunk list, and the chunks are not in a guaranteed order. A
// parser that assumes "fmt then data" works on most files and fails on the ones
// a phone or an editor produced, which is the worst possible split.
#ifndef RPC_WAV_H
#define RPC_WAV_H

#include <stdint.h>

struct WavInfo {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_offset;    // where the samples start
    uint32_t data_bytes;     // how many there are
};

enum WavResult {
    WAV_OK = 0,
    WAV_NOT_WAV,             // no RIFF/WAVE marker
    WAV_TRUNCATED,           // a chunk claims more than the file holds
    WAV_NO_FMT,
    WAV_NO_DATA,
    WAV_UNSUPPORTED,         // compressed, or a bit depth this cannot play
};

// Parse `len` bytes from the start of a file. The whole header rarely exceeds a
// hundred bytes, so a caller reads a small block rather than the file.
WavResult   wav_parse(const uint8_t *data, uint32_t len, WavInfo *out);
const char *wav_result_str(WavResult r);

#endif  // RPC_WAV_H
