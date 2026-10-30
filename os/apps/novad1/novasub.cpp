// Desc: Flipper .sub codec - see novasub.h.
// File: novasub.cpp
#include "novasub.h"

namespace nova {
namespace sub {

using namespace codec;

static void sub_copy_span(char *dst, unsigned cap, const char *s, const char *e) {
    unsigned n = 0;
    while (s < e && n + 1 < cap) dst[n++] = *s++;
    dst[n] = 0;
}

void parse(const char *text, File *out, uint32_t *raw, unsigned raw_cap,
           unsigned *raw_n, unsigned *raw_dropped) {
    out->freq_hz = 0;
    sub_copy_span(out->preset, sizeof(out->preset), SUB_PRESET_DEFAULT,
              SUB_PRESET_DEFAULT + sizeof(SUB_PRESET_DEFAULT) - 1);
    out->protocol[0] = 0;
    out->has_key = false;
    out->key[0] = 0;
    out->bits = -1;
    out->te = -1;
    unsigned n = 0, dropped = 0;

    for (const char *p = text; *p; ) {
        const char *e = eol(p);
        const char *next = *e ? e + 1 : e;
        const char *s = p;
        trim(&s, &e);
        // A blank line, a comment, or a line with no colon says nothing.
        const char *colon = s;
        while (colon < e && *colon != ':') colon++;
        if (s >= e || *s == '#' || colon >= e) { p = next; continue; }

        const char *ks = s, *ke = colon;
        const char *vs = colon + 1, *ve = e;
        trim(&ks, &ke);
        trim(&vs, &ve);

        if (eq_span(ks, ke, "Frequency")) {
            int32_t v;
            if (parse_int(vs, ve, &v) && v >= 0) out->freq_hz = (uint32_t)v;
        } else if (eq_span(ks, ke, "Preset")) {
            sub_copy_span(out->preset, sizeof(out->preset), vs, ve);
        } else if (eq_span(ks, ke, "Protocol")) {
            sub_copy_span(out->protocol, sizeof(out->protocol), vs, ve);
        } else if (eq_span(ks, ke, "RAW_Data")) {
            const char *q = vs, *ts, *te;
            while (next_token(&q, ve, &ts, &te)) {
                int32_t v;
                if (!parse_int(ts, te, &v)) continue;          // Python skipped what int() refused
                if (v < 0) v = -v;
                if (n < raw_cap) raw[n++] = (uint32_t)v; else dropped++;
            }
        } else if (eq_span(ks, ke, "Key")) {
            out->has_key = true;
            sub_copy_span(out->key, sizeof(out->key), vs, ve);
        } else if (eq_span(ks, ke, "Bit")) {
            int32_t v;
            if (parse_int(vs, ve, &v)) out->bits = (int32_t)v;
        } else if (eq_span(ks, ke, "TE")) {
            int32_t v;
            if (parse_int(vs, ve, &v)) out->te = (int32_t)v;
        }
        p = next;
    }
    *raw_n = n;
    if (raw_dropped) *raw_dropped = dropped;
}

bool write_raw(const Sink &k, const uint32_t *times, unsigned n, uint32_t freq_hz,
               const char *preset) {
    if (!freq_hz) freq_hz = SUB_FREQ_DEFAULT_HZ;
    if (!preset || !preset[0]) preset = SUB_PRESET_DEFAULT;
    if (!put_str(k, "Filetype: Flipper SubGhz RAW File\nVersion: 1\nFrequency: ")) return false;
    if (!put_u32(k, freq_hz)) return false;
    if (!put_str(k, "\nPreset: ") || !put_str(k, preset)) return false;
    if (!put_str(k, "\nProtocol: RAW")) return false;
    for (unsigned i = 0; i < n; i += 512) {
        if (!put_str(k, "\nRAW_Data:")) return false;
        for (unsigned j = i; j < n && j < i + 512; j++) {
            if (!put_chr(k, ' ')) return false;
            // Even index is a mark and keeps its sign; odd is a space and loses it.
            if (j & 1) { if (!put_chr(k, '-')) return false; }
            if (!put_u32(k, times[j])) return false;
        }
    }
    return put_chr(k, '\n');
}

// --- decoded protocols ---------------------------------------------------------------------

// The key bytes as one number, the way _key_to_int shifted them in: each token is
// a hex value and moves what came before up a byte. Tokens that are not hex are
// skipped, as they were. 64 bits is as far as a key goes here.
static uint64_t key_to_int(const char *key_hex) {
    uint64_t val = 0;
    const char *p = key_hex ? key_hex : "";
    const char *e = p + strlen(p);
    const char *ts, *te;
    while (next_token(&p, e, &ts, &te)) {
        uint64_t b;
        if (parse_hex(ts, te, &b)) val = (val << 8) | b;
    }
    return val;
}

// A short list that stops being written to when it is full; the caller checks.
struct SubOut {
    uint32_t *t;
    unsigned  cap, n;
    bool      over;
    void push(uint32_t v) { if (n < cap) t[n++] = v; else over = true; }
};

static void princeton(SubOut &o, uint64_t val, int bits, uint32_t te) {
    for (int i = bits - 1; i >= 0; i--) {
        if ((val >> i) & 1) { o.push(te * 3); o.push(te); }
        else                { o.push(te);     o.push(te * 3); }
    }
    o.push(te);             // the stop pulse
    o.push(te * 30);        // and the guard
}

static uint32_t came_header_te(int bits) {
    switch (bits) {
        case 24: case 42: return 76;
        case 12: case 18: return 47;
        case 25:          return 36;
        default:          return 16;
    }
}

static void came_like(SubOut &o, uint64_t val, int bits, uint32_t te, uint32_t header_te,
                      bool bit1_long_low) {
    const uint32_t te_long = te * 2;
    o.push(te);                                       // the start pulse
    for (int i = bits - 1; i >= 0; i--) {
        bool one = (val >> i) & 1;
        bool long_low = bit1_long_low ? one : !one;
        if (long_low) { o.push(te_long); o.push(te); }
        else          { o.push(te);      o.push(te_long); }
    }
    o.push(te * header_te);                           // trailing: the next repeat's preamble
}

static void linear(SubOut &o, uint64_t val, int bits, uint32_t te) {
    int last = 0;
    for (int i = bits - 1; i >= 0; i--) {
        last = (int)((val >> i) & 1);
        if (last) { o.push(te * 3); o.push(te); }
        else      { o.push(te);     o.push(te * 3); }
    }
    // The inter-frame guard replaces the last bit's low.
    if (o.n) o.t[o.n - 1] = te * (last == 1 ? 42u : 44u);
}

unsigned encode_decoded(const char *protocol, const char *key_hex, int bits, int te,
                        uint32_t *out, unsigned cap) {
    if (te <= 0 || bits <= 0 || bits > 64) return 0;
    uint64_t val = key_to_int(key_hex);
    const char *p = protocol ? protocol : "";
    const char *e = p + strlen(p);
    trim(&p, &e);

    SubOut o = { out, cap, 0, false };
    const uint32_t t = (uint32_t)te;
    if (ieq_span(p, e, "princeton"))                                         princeton(o, val, bits, t);
    else if (ieq_span(p, e, "came"))                                         came_like(o, val, bits, t, came_header_te(bits), true);
    else if (ieq_span(p, e, "nice flo") || ieq_span(p, e, "nice_flo") ||
             ieq_span(p, e, "niceflo"))                                      came_like(o, val, bits, t, 36, true);
    else if (e - p >= 6 && ieq_span(p, p + 6, "holtek"))                     came_like(o, val, bits, t, 36, true);
    else if (ieq_span(p, e, "ansonic"))                                      came_like(o, val, bits, t, 35, false);
    else if (ieq_span(p, e, "linear"))                                       linear(o, val, bits, t);
    else return 0;
    return o.over ? 0 : o.n;
}

}  // namespace sub
}  // namespace nova
