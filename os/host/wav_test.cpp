// Reading a WAV header.
//
// The file comes off the device's own filesystem, so this is not hostile input
// in the way a Bluetooth advertisement is — but it is input written by whatever
// tool made the file, and those disagree. Chunk order varies, LIST and fact
// chunks turn up in the middle, and plenty of writers leave the data size as a
// placeholder because they did not know it when they wrote the header.
//
// Getting any of that wrong plays noise, which sounds like a broken audio path
// rather than a broken parser.
#include "../core/wav.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

// --- building test files ----------------------------------------------------

static uint8_t buf[512];
static uint32_t len;

static void put(const void *p, uint32_t n) { memcpy(buf + len, p, n); len += n; }
static void put32(uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(b, 4);
}
static void put16(uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put(b, 2);
}
static void chunk_fmt(uint16_t format, uint16_t ch, uint32_t rate, uint16_t bits) {
    put("fmt ", 4); put32(16);
    put16(format); put16(ch); put32(rate);
    put32(rate * ch * bits / 8);        // byte rate
    put16((uint16_t)(ch * bits / 8));   // block align
    put16(bits);
}
static void begin(void) { len = 0; put("RIFF", 4); put32(0); put("WAVE", 4); }

int main(void) {
    printf("wav_test - reading a WAV header\n");
    WavInfo w;

    // --- the ordinary file --------------------------------------------------
    begin();
    chunk_fmt(1, 2, 44100, 16);
    put("data", 4); put32(8);
    put32(0); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_OK, "a plain 44.1 kHz stereo file parses");
    ck(w.sample_rate == 44100, "with the right rate");
    ck(w.channels == 2, "the right channel count");
    ck(w.bits == 16, "and the right depth");
    ck(w.data_bytes == 8, "and knows how much audio there is");
    ck(buf[w.data_offset - 8] == 'd', "with the offset pointing past the chunk header");

    begin();
    chunk_fmt(1, 1, 22050, 16);
    put("data", 4); put32(4); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_OK && w.channels == 1, "mono is fine too");

    // --- chunk order, which is where real files differ ------------------------
    //
    // A LIST chunk before the data is what almost every editor writes. Skipping
    // it wrongly means reading its text as samples.
    begin();
    chunk_fmt(1, 2, 44100, 16);
    put("LIST", 4); put32(10); put("INFOhello", 9); put("\0", 1);
    put("data", 4); put32(4); put32(0x11223344);
    ck(wav_parse(buf, len, &w) == WAV_OK, "a LIST chunk between fmt and data is skipped");
    ck(w.data_bytes == 4, "and the data chunk is still found");
    ck(buf[w.data_offset] == 0x44, "pointing at the samples, not at the LIST text");

    // An ODD-length chunk is padded to even, and the pad is not in the size.
    // Missing it walks every later chunk half a byte out of step.
    begin();
    chunk_fmt(1, 2, 44100, 16);
    put("fact", 4); put32(3); put("abc", 3); put("\0", 1);     // 3 bytes + 1 pad
    put("data", 4); put32(4); put32(0xAABBCCDD);
    ck(wav_parse(buf, len, &w) == WAV_OK, "an odd-length chunk is padded to even");
    ck(w.data_bytes == 4 && buf[w.data_offset] == 0xDD,
       "and the data after it is still found in the right place");

    // --- placeholder sizes ----------------------------------------------------
    //
    // Streaming writers do not know the length when they write the header, so
    // they leave something enormous there. The samples really are present, and
    // refusing the file would be refusing a file that plays fine everywhere else.
    begin();
    chunk_fmt(1, 2, 44100, 16);
    put("data", 4); put32(0xFFFFFFFF);
    put32(0); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_OK,
       "a data chunk with a placeholder size is accepted");
    ck(w.data_bytes == 8, "using what is actually in the file");

    // But a NON-data chunk claiming too much is a corrupt file, not a
    // convention, and trusting it would read past the buffer.
    begin();
    chunk_fmt(1, 2, 44100, 16);
    put("LIST", 4); put32(0xFFFFFF00);
    ck(wav_parse(buf, len, &w) == WAV_TRUNCATED,
       "any other chunk claiming more than the file holds is refused");

    // --- what cannot be played ------------------------------------------------
    begin();
    chunk_fmt(2, 2, 44100, 16);       // 2 = ADPCM
    put("data", 4); put32(4); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_UNSUPPORTED, "a compressed file is refused");

    begin();
    chunk_fmt(1, 2, 44100, 24);
    put("data", 4); put32(4); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_UNSUPPORTED, "24-bit is refused");

    begin();
    chunk_fmt(1, 6, 48000, 16);
    put("data", 4); put32(4); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_UNSUPPORTED, "and so is 5.1");

    begin();
    chunk_fmt(1, 2, 0, 16);
    put("data", 4); put32(4); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_UNSUPPORTED, "a zero sample rate is refused");

    // --- malformed ------------------------------------------------------------
    ck(wav_parse(nullptr, 0, &w) == WAV_NOT_WAV, "no data at all");
    ck(wav_parse(buf, 4, &w) == WAV_NOT_WAV, "a file too short to hold a header");

    len = 0; put("RIFX", 4); put32(0); put("WAVE", 4);
    ck(wav_parse(buf, len, &w) == WAV_NOT_WAV, "big-endian RIFX is not this format");

    len = 0; put("RIFF", 4); put32(0); put("AVI ", 4);
    ck(wav_parse(buf, len, &w) == WAV_NOT_WAV, "a RIFF file that is not WAVE");

    begin();
    put("data", 4); put32(4); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_NO_FMT, "data before any format chunk is unreadable");

    begin();
    chunk_fmt(1, 2, 44100, 16);
    ck(wav_parse(buf, len, &w) == WAV_NO_DATA, "a header with no audio after it");

    begin();
    put("fmt ", 4); put32(4); put32(0);      // fmt too short to hold a format
    put("data", 4); put32(4); put32(0);
    ck(wav_parse(buf, len, &w) == WAV_TRUNCATED, "a format chunk too short to be one");

    // Every reason says something different, or the message helps nobody.
    ck(wav_result_str(WAV_UNSUPPORTED) != wav_result_str(WAV_TRUNCATED),
       "each refusal explains itself differently");

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
