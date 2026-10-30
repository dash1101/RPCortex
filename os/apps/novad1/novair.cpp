// Desc: Flipper .ir codec - see novair.h.
// File: novair.cpp
#include "novair.h"

namespace nova {
namespace ir {

using namespace codec;

// --- encoders ------------------------------------------------------------------------------

struct IrOut {
    uint32_t *t;
    unsigned  cap, n;
    bool      over;
    void push(uint32_t v) { if (n < cap) t[n++] = v; else over = true; }
};

// Pulse-distance, the NEC family: bytes least-significant bit first.
static void pulsedist(IrOut &o, const uint8_t *bytes, unsigned nb, uint32_t hdr_m,
                      uint32_t hdr_s, uint32_t bit, uint32_t one, uint32_t zero) {
    o.push(hdr_m);
    o.push(hdr_s);
    for (unsigned b = 0; b < nb; b++)
        for (int i = 0; i < 8; i++) {
            o.push(bit);
            o.push(((bytes[b] >> i) & 1) ? one : zero);
        }
    o.push(bit);
}

// A level list (1 mark, 0 space) merged into durations: neighbours at the same
// level join, then a leading space and a trailing space go, so the list starts and
// ends on a mark, which is what a transmitter expects.
struct Level { uint8_t lv; uint32_t d; };

static void merge_levels(IrOut &o, const Level *lv, unsigned n) {
    Level m[96];
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (k && m[k - 1].lv == lv[i].lv) m[k - 1].d += lv[i].d;
        else if (k < 96)                  m[k++] = lv[i];
    }
    unsigned s = 0;
    if (k && m[0].lv == 0) s = 1;
    if (k > s && m[k - 1].lv == 0) k--;
    for (unsigned i = s; i < k; i++) o.push(m[i].d);
}

// The hex-byte tokens of an address or command, as Python's _hexbytes read them:
// each token a hex number, tokens that are not hex skipped. Up to 8 are kept.
static unsigned hexbytes(const char *s, uint32_t *out, unsigned max) {
    unsigned n = 0;
    const char *p = s ? s : "";
    const char *e = p + strlen(p);
    const char *ts, *te;
    while (next_token(&p, e, &ts, &te)) {
        uint64_t v;
        if (parse_hex(ts, te, &v) && n < max) out[n++] = (uint32_t)v;
    }
    return n;
}

static bool is_proto(const char *p, const char *e, const char *upper_word) {
    // Protocol names compare in upper case, as .upper() did.
    while (*upper_word) {
        if (p >= e) return false;
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != *upper_word++) return false;
    }
    return p == e;
}

unsigned encode(const char *protocol, const char *address, const char *command,
                uint32_t *freq_hz, uint32_t *out, unsigned cap) {
    // Not trimmed: the MicroPython encoder upper-cased the name and compared it as
    // given, so " NEC " was an unknown protocol there and is one here.
    const char *p = protocol ? protocol : "";
    const char *e = p + strlen(p);

    uint32_t a[8], c[8];
    unsigned na = hexbytes(address, a, 8), nc = hexbytes(command, c, 8);
    if (!na) { a[0] = 0; na = 1; }
    if (!nc) { c[0] = 0; nc = 1; }

    IrOut o = { out, cap, 0, false };
    uint32_t f = 38000;

    if (is_proto(p, e, "NEC")) {
        const uint8_t b[4] = { (uint8_t)a[0], (uint8_t)(a[0] ^ 0xFF), (uint8_t)c[0], (uint8_t)(c[0] ^ 0xFF) };
        pulsedist(o, b, 4, 9000, 4500, 560, 1690, 560);
    } else if (is_proto(p, e, "NECEXT") || is_proto(p, e, "NEC_EXT") ||
               is_proto(p, e, "NEC42") || is_proto(p, e, "NEC42EXT")) {
        uint32_t a1 = na > 1 ? a[1] : 0;
        uint32_t c1 = nc > 1 ? c[1] : (c[0] ^ 0xFF);
        const uint8_t b[4] = { (uint8_t)a[0], (uint8_t)a1, (uint8_t)c[0], (uint8_t)c1 };
        pulsedist(o, b, 4, 9000, 4500, 560, 1690, 560);
    } else if (is_proto(p, e, "SAMSUNG32") || is_proto(p, e, "SAMSUNG")) {
        const uint8_t b[4] = { (uint8_t)a[0], (uint8_t)a[0], (uint8_t)c[0], (uint8_t)(c[0] ^ 0xFF) };
        pulsedist(o, b, 4, 4500, 4500, 560, 1690, 560);
    } else if (is_proto(p, e, "PIONEER")) {
        // Timings are the Flipper firmware's. The byte layout is inferred: DEVICE-UNCONFIRMED.
        f = 40000;
        const uint8_t b[4] = { (uint8_t)a[0], (uint8_t)(a[0] ^ 0xFF), (uint8_t)c[0], (uint8_t)(c[0] ^ 0xFF) };
        pulsedist(o, b, 4, 8500, 4225, 500, 1500, 500);
    } else if (is_proto(p, e, "SIRC") || is_proto(p, e, "SONY") ||
               is_proto(p, e, "SIRC15") || is_proto(p, e, "SIRC20")) {
        unsigned nbits = is_proto(p, e, "SIRC15") ? 15 : (is_proto(p, e, "SIRC20") ? 20 : 12);
        uint32_t val;
        if (nbits == 12)      val = (c[0] & 0x7F) | ((a[0] & 0x1F) << 7);
        else if (nbits == 15) val = (c[0] & 0x7F) | ((a[0] & 0xFF) << 7);
        else                  val = (c[0] & 0x7F) | ((a[0] & 0x1F) << 7) | (((na > 1 ? a[1] : 0) & 0xFF) << 12);
        f = 40000;
        o.push(2400);
        o.push(600);
        for (unsigned i = 0; i < nbits; i++) {
            o.push(((val >> i) & 1) ? 1200 : 600);
            o.push(600);
        }
    } else if (is_proto(p, e, "RC5") || is_proto(p, e, "RC5X")) {
        // Philips RC5: 14 bi-phase bits at 889 us a half, 36 kHz. Order: start,
        // field (RC5X: the inverted 7th command bit), toggle, 5 address, 6 command.
        f = 36000;
        uint32_t addr = a[0] & 0x1F, cmd = c[0];
        uint32_t s2 = 1;
        if (is_proto(p, e, "RC5X")) s2 = (cmd & 0x40) ? 0 : 1;
        cmd &= 0x3F;
        uint8_t bits[14];
        unsigned k = 0;
        bits[k++] = 1; bits[k++] = (uint8_t)s2; bits[k++] = 0;
        for (int i = 4; i >= 0; i--) bits[k++] = (uint8_t)((addr >> i) & 1);
        for (int i = 5; i >= 0; i--) bits[k++] = (uint8_t)((cmd >> i) & 1);
        Level lv[28];
        unsigned n = 0;
        for (unsigned i = 0; i < k; i++) {
            if (bits[i] == 1) { lv[n++] = { 0, 889 }; lv[n++] = { 1, 889 }; }   // a one: space, then mark
            else              { lv[n++] = { 1, 889 }; lv[n++] = { 0, 889 }; }
        }
        merge_levels(o, lv, n);
    } else if (is_proto(p, e, "RC6") || is_proto(p, e, "RC6-MODE0") || is_proto(p, e, "RC6_MODE0")) {
        // Philips RC6 mode 0: a 2666/889 leader, then bi-phase bits at 444 us a half
        // (a one is mark then space), 36 kHz. The toggle bit is double width.
        f = 36000;
        uint32_t addr = a[0] & 0xFF, cmd = c[0] & 0xFF;
        uint8_t bits[21];
        unsigned k = 0;
        bits[k++] = 1; bits[k++] = 0; bits[k++] = 0; bits[k++] = 0; bits[k++] = 0;
        for (int i = 7; i >= 0; i--) bits[k++] = (uint8_t)((addr >> i) & 1);
        for (int i = 7; i >= 0; i--) bits[k++] = (uint8_t)((cmd >> i) & 1);
        Level lv[44];
        unsigned n = 0;
        lv[n++] = { 1, 2666 };
        lv[n++] = { 0, 889 };
        for (unsigned i = 0; i < k; i++) {
            uint32_t half = (i == 4) ? 888 : 444;
            if (bits[i] == 1) { lv[n++] = { 1, half }; lv[n++] = { 0, half }; }
            else              { lv[n++] = { 0, half }; lv[n++] = { 1, half }; }
        }
        merge_levels(o, lv, n);
    } else {
        return 0;
    }
    if (o.over) return 0;
    if (freq_hz) *freq_hz = f;
    return o.n;
}

// --- reading -------------------------------------------------------------------------------

// A decimal like "0.330000" in millionths. Plain digits with one dot; anything else
// is "not a number" and the default stays, which is what float() raising did.
static bool parse_ppm(const char *s, const char *e, uint32_t *out) {
    if (s >= e) return false;
    uint32_t whole = 0, frac = 0, scale = 1000000;
    const char *q = s;
    bool any = false;
    for (; q < e && *q >= '0' && *q <= '9'; q++) { whole = whole * 10 + (uint32_t)(*q - '0'); any = true; if (whole > 4000) return false; }
    if (q < e && *q == '.') {
        q++;
        for (; q < e && *q >= '0' && *q <= '9'; q++) {
            any = true;
            if (scale > 1) { scale /= 10; frac += (uint32_t)(*q - '0') * scale; }
        }
    }
    if (!any || q != e) return false;
    *out = whole * 1000000u + frac;
    return true;
}

namespace {

struct Block {
    bool     has_name;
    char     name[IR_NAME_MAX];
    uint8_t  type_kind;                 // 0 raw (the default), 1 parsed, 2 anything else
    char     protocol[16];
    char     address[24], command[24];
    bool     has_proto;
    uint32_t freq;
    uint32_t duty;
    bool     has_data;
    unsigned n;                         // durations collected so far
    bool     truncated;
};

void block_reset(Block &b) {
    b.has_name = false; b.name[0] = 0;
    b.type_kind = 0;
    b.protocol[0] = 0; b.address[0] = 0; b.command[0] = 0;
    b.has_proto = false;
    b.freq = 38000; b.duty = IR_DUTY_DEFAULT_PPM;
    b.has_data = false; b.n = 0; b.truncated = false;
}

void copy_span(char *dst, unsigned cap, const char *s, const char *e) {
    unsigned n = 0;
    while (s < e && n + 1 < cap) dst[n++] = *s++;
    dst[n] = 0;
}

void copy_str(char *dst, unsigned cap, const char *src) {
    copy_span(dst, cap, src, src + strlen(src));
}

// Read the integers of a data line into times[], from position b.n. A token that
// is not a plain (optionally negative) integer is skipped, as the isdigit filter
// did. `comma` selects the legacy bare list, whose separator is a comma and which
// admits no sign.
void read_durations(Block &b, const char *s, const char *e, uint32_t *times, unsigned cap, bool comma) {
    b.n = 0;
    b.truncated = false;
    if (comma) {
        const char *q = s;
        while (q <= e) {
            const char *t = q;
            while (q < e && *q != ',') q++;
            const char *ts = t, *te = q;
            trim(&ts, &te);
            int32_t v;
            if (ts < te && *ts != '-' && *ts != '+' && parse_int(ts, te, &v)) {
                if (b.n < cap) times[b.n++] = (uint32_t)v; else b.truncated = true;
            }
            q++;
        }
    } else {
        const char *q = s, *ts, *te;
        while (next_token(&q, e, &ts, &te)) {
            int32_t v;
            if (*ts == '+' || !parse_int(ts, te, &v)) continue;      // isdigit refused a plus sign
            if (b.n < cap) times[b.n++] = (uint32_t)(v < 0 ? -v : v); else b.truncated = true;
        }
    }
}

// Finish a block: is there a signal in it, and if so fill sig (and times, for a
// parsed one, which encode() writes).
bool flush(Block &b, Signal *sig, uint32_t *times, unsigned cap) {
    if (!b.has_name && !b.has_data) return false;
    const char *name = b.has_name ? b.name : "signal";
    if (b.type_kind == 0 && b.has_data && b.n) {
        copy_str(sig->name, sizeof(sig->name), name);
        sig->freq_hz = b.freq;
        sig->duty_ppm = b.duty;
        sig->n = b.n;
        sig->truncated = b.truncated;
        return true;
    }
    if (b.type_kind == 1 && b.has_proto) {
        uint32_t f = 38000;
        unsigned n = encode(b.protocol, b.address, b.command, &f, times, cap);
        if (!n) return false;
        copy_str(sig->name, sizeof(sig->name), name);
        sig->freq_hz = f;
        sig->duty_ppm = IR_DUTY_DEFAULT_PPM;
        sig->n = n;
        sig->truncated = false;
        return true;
    }
    return false;
}

}  // namespace

bool next(const char **cursor, Signal *sig, uint32_t *times, unsigned cap) {
    const char *p = *cursor;
    Block cur;
    block_reset(cur);

    while (*p) {
        const char *line_start = p;
        const char *e = eol(p);
        const char *nextp = *e ? e + 1 : e;
        const char *s = p;
        trim(&s, &e);

        if (starts(s, e, "name:")) {
            if (flush(cur, sig, times, cap)) { *cursor = line_start; return true; }
            block_reset(cur);
            cur.has_name = true;
            const char *vs = s + 5, *ve = e;
            trim(&vs, &ve);
            copy_span(cur.name, sizeof(cur.name), vs, ve);
        } else if (starts(s, e, "type:")) {
            const char *vs = s + 5, *ve = e;
            trim(&vs, &ve);
            // Anything but "raw" and "parsed" makes the block neither: the MicroPython
            // codec kept the text and matched on equality, so an unknown type emitted nothing.
            cur.type_kind = eq_span(vs, ve, "raw") ? 0 : (eq_span(vs, ve, "parsed") ? 1 : 2);
        } else if (starts(s, e, "protocol:")) {
            const char *vs = s + 9, *ve = e;
            trim(&vs, &ve);
            copy_span(cur.protocol, sizeof(cur.protocol), vs, ve);
            cur.has_proto = (ve > vs);
        } else if (starts(s, e, "address:")) {
            const char *vs = s + 8, *ve = e;
            trim(&vs, &ve);
            copy_span(cur.address, sizeof(cur.address), vs, ve);
        } else if (starts(s, e, "command:")) {
            const char *vs = s + 8, *ve = e;
            trim(&vs, &ve);
            copy_span(cur.command, sizeof(cur.command), vs, ve);
        } else if (starts(s, e, "frequency:")) {
            const char *vs = s + 10, *ve = e;
            trim(&vs, &ve);
            int32_t v;
            if (parse_int(vs, ve, &v) && v > 0) cur.freq = (uint32_t)v;
        } else if (starts(s, e, "duty_cycle:")) {
            const char *vs = s + 11, *ve = e;
            trim(&vs, &ve);
            uint32_t d;
            if (parse_ppm(vs, ve, &d)) cur.duty = d;
        } else if (starts(s, e, "data:")) {
            read_durations(cur, s + 5, e, times, cap, false);
            cur.has_data = true;
        } else if (s < e && !cur.has_data && !cur.has_name) {
            // The legacy form: a bare comma-separated list with no name above it.
            const char *q = s;
            while (q < e && *q != ',') q++;
            if (q < e) {
                block_reset(cur);
                cur.has_name = true;
                copy_str(cur.name, sizeof(cur.name), "signal");
                read_durations(cur, s, e, times, cap, true);
                cur.has_data = true;
            }
        }
        p = nextp;
    }
    bool got = flush(cur, sig, times, cap);
    *cursor = p;
    return got;
}

// --- writing -------------------------------------------------------------------------------

bool write_header(const Sink &k) {
    return put_str(k, "Filetype: IR signals file\nVersion: 1\n");
}

bool write_signal(const Sink &k, const char *name, const uint32_t *times, unsigned n,
                  uint32_t freq_hz, uint32_t duty_ppm) {
    if (!put_str(k, "#\nname: ") || !put_str(k, name)) return false;
    if (!put_str(k, "\ntype: raw\nfrequency: ") || !put_u32(k, freq_hz)) return false;
    if (!put_str(k, "\nduty_cycle: ") || !put_fixed6(k, duty_ppm)) return false;
    if (!put_str(k, "\ndata:")) return false;
    for (unsigned i = 0; i < n; i++)
        if (!put_chr(k, ' ') || !put_u32(k, times[i])) return false;
    return put_chr(k, '\n');
}

}  // namespace ir
}  // namespace nova
