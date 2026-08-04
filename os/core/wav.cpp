#include "wav.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

WavResult wav_parse(const uint8_t *data, uint32_t len, WavInfo *out) {
    if (!data || !out) return WAV_NOT_WAV;
    memset(out, 0, sizeof(*out));

    // RIFF....WAVE
    if (len < 12) return WAV_NOT_WAV;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return WAV_NOT_WAV;

    bool have_fmt = false;
    uint32_t pos = 12;

    // Walk the chunk list rather than assuming an order. Files from editors and
    // phones routinely carry LIST, fact or bext chunks before the data, and a
    // parser that expects fmt-then-data reads those as samples.
    while (pos + 8 <= len) {
        uint32_t id_off = pos;
        uint32_t size   = rd32(data + pos + 4);
        uint32_t body   = pos + 8;

        // A chunk claiming more than the file holds is a truncated or corrupt
        // file. Trusting it means reading past the buffer.
        if (body > len || size > len - body) {
            // The DATA chunk is the exception worth allowing: plenty of files
            // are written with a placeholder size, and the samples are really
            // there. Anything else claiming too much is not trustworthy.
            if (memcmp(data + id_off, "data", 4) == 0 && have_fmt) {
                out->data_offset = body;
                out->data_bytes  = len > body ? len - body : 0;
                return WAV_OK;
            }
            return WAV_TRUNCATED;
        }

        if (memcmp(data + id_off, "fmt ", 4) == 0) {
            if (size < 16) return WAV_TRUNCATED;
            uint16_t format = rd16(data + body);
            out->channels    = rd16(data + body + 2);
            out->sample_rate = rd32(data + body + 4);
            out->bits        = rd16(data + body + 14);

            // 1 is uncompressed PCM. 0xFFFE is "extensible", which is PCM with
            // a longer header; anything else is compressed and would need a
            // decoder this does not have.
            if (format != 1 && format != 0xFFFE) return WAV_UNSUPPORTED;
            if (out->bits != 16) return WAV_UNSUPPORTED;
            if (out->channels < 1 || out->channels > 2) return WAV_UNSUPPORTED;
            if (out->sample_rate == 0) return WAV_UNSUPPORTED;
            have_fmt = true;
        } else if (memcmp(data + id_off, "data", 4) == 0) {
            if (!have_fmt) return WAV_NO_FMT;      // data before fmt is unreadable
            out->data_offset = body;
            out->data_bytes  = size;
            return WAV_OK;
        }

        // Chunks are padded to an even length, and the pad byte is not counted
        // in the size. Missing that walks half a byte out of step for the rest
        // of the file.
        pos = body + size + (size & 1);
    }

    return have_fmt ? WAV_NO_DATA : WAV_NO_FMT;
}

const char *wav_result_str(WavResult r) {
    switch (r) {
        case WAV_OK:          return "ok";
        case WAV_NOT_WAV:     return "not a WAV file";
        case WAV_TRUNCATED:   return "the file is cut short or its header is wrong";
        case WAV_NO_FMT:      return "no format chunk";
        case WAV_NO_DATA:     return "no audio in it";
        case WAV_UNSUPPORTED: return "needs 16-bit uncompressed PCM, mono or stereo";
        default:              return "unknown";
    }
}
