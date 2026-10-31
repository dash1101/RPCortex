// Desc: Flipper .nfc codec - see novanfc.h.
// File: novanfc.cpp
#include "novanfc.h"

namespace nova {
namespace nfc {

using namespace codec;

unsigned unhex(const char *s, uint8_t *out, unsigned cap) {
    // Spaces and colons are separators; what is left is read two digits at a time.
    char digits[2];
    unsigned have = 0, n = 0;
    for (; s && *s; s++) {
        if (*s == ' ' || *s == ':') continue;
        digits[have++] = *s;
        if (have == 2) {
            have = 0;
            if (n >= cap) break;
            uint64_t v;
            if (digits[0] == '?' || digits[1] == '?') v = 0;
            else if (!parse_hex(digits, digits + 2, &v)) v = 0;
            out[n++] = (uint8_t)v;
        }
    }
    return n;
}

// Does a line (already trimmed, not blank, not a comment) have the key? On a match
// the value span is set.
static bool kv_line(const char *s, const char *e, const char **ks, const char **ke,
                    const char **vs, const char **ve) {
    if (s >= e || *s == '#') return false;
    const char *colon = s;
    while (colon < e && *colon != ':') colon++;
    if (colon >= e) return false;
    *ks = s; *ke = colon;
    *vs = colon + 1; *ve = e;
    trim(ks, ke);
    trim(vs, ve);
    return true;
}

bool get(const char *text, const char *key, char *out, unsigned cap) {
    if (cap) out[0] = 0;
    for (const char *p = text; *p; ) {
        const char *e = eol(p);
        const char *nextp = *e ? e + 1 : e;
        const char *s = p, *ks, *ke, *vs, *ve;
        trim(&s, &e);
        if (kv_line(s, e, &ks, &ke, &vs, &ve) && eq_span(ks, ke, key)) {
            unsigned n = 0;
            while (vs < ve && n + 1 < cap) out[n++] = *vs++;
            if (cap) out[n] = 0;
            return true;
        }
        p = nextp;
    }
    return false;
}

bool read_info(const char *text, Info *out) {
    memset(out, 0, sizeof(*out));
    char v[48];
    if (get(text, "Device type", v, sizeof(v))) {
        unsigned n = 0;
        while (v[n] && n + 1 < sizeof(out->device_type)) { out->device_type[n] = v[n]; n++; }
        out->device_type[n] = 0;
    }
    if (get(text, "UID", v, sizeof(v)))  out->uid_len = unhex(v, out->uid, sizeof(out->uid));
    if (get(text, "ATQA", v, sizeof(v))) out->atqa_len = unhex(v, out->atqa, sizeof(out->atqa));
    if (get(text, "SAK", v, sizeof(v))) { uint8_t b[1]; if (unhex(v, b, 1)) out->sak = b[0]; }

    for (const char *p = text; *p; ) {
        const char *e = eol(p);
        const char *nextp = *e ? e + 1 : e;
        const char *s = p, *ks, *ke, *vs, *ve;
        trim(&s, &e);
        if (kv_line(s, e, &ks, &ke, &vs, &ve) &&
            (starts(ks, ke, "Page ") || starts(ks, ke, "Block ")))
            out->memory_lines++;
        p = nextp;
    }
    return out->uid_len > 0;
}

// --- writing -------------------------------------------------------------------------------

static bool put_bytes(const Sink &k, const uint8_t *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (i && !put_chr(k, ' ')) return false;
        if (!put_hex2(k, b[i])) return false;
    }
    return true;
}

static bool header(const Sink &k, const char *device_type, const uint8_t *uid,
                   unsigned uid_len, const uint8_t *atqa, uint8_t sak) {
    return put_str(k, "Filetype: Flipper NFC device\nVersion: 4\nDevice type: ") &&
           put_str(k, device_type) &&
           put_str(k, "\nUID: ") && put_bytes(k, uid, uid_len) &&
           put_str(k, "\nATQA: ") && put_bytes(k, atqa, 2) &&
           put_str(k, "\nSAK: ") && put_hex2(k, sak) &&
           put_chr(k, '\n');
}

// "Page 3: AA BB CC DD\n", or ?? for what could not be read.
static bool memory_line(const Sink &k, const char *label, unsigned index, unsigned width,
                        Getter get_fn, void *ctx) {
    if (!put_str(k, label) || !put_u32(k, index) || !put_str(k, ": ")) return false;
    uint8_t b[16];
    uint16_t known = 0xFFFF;
    bool ok = get_fn && get_fn(ctx, index, b, &known);
    for (unsigned i = 0; i < width; i++) {
        if (i && !put_chr(k, ' ')) return false;
        if (ok && (known & (1u << i))) { if (!put_hex2(k, b[i])) return false; }
        else                           { if (!put_str(k, "??")) return false; }
    }
    return put_chr(k, '\n');
}

bool write_iso14443a(const Sink &k, const uint8_t *uid, unsigned uid_len,
                     const uint8_t *atqa, uint8_t sak) {
    return header(k, NFC_DT_ISO3A, uid, uid_len, atqa, sak);
}

bool write_ultralight(const Sink &k, const uint8_t *uid, unsigned uid_len,
                      const uint8_t *atqa, uint8_t sak, const char *ul_type,
                      unsigned pages, Getter page, void *ctx,
                      const UltralightExtras *x) {
    if (!header(k, NFC_DT_ULTRALIGHT, uid, uid_len, atqa, sak)) return false;
    if (!put_str(k, "Data format version: 2\nNTAG/Ultralight type: ") || !put_str(k, ul_type)) return false;

    if (!put_str(k, "\nSignature: ")) return false;
    static const uint8_t zeros[32] = { 0 };
    if (!put_bytes(k, (x && x->signature) ? x->signature : zeros, 32)) return false;

    static const uint8_t default_version[8] = { 0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03 };
    if (!put_str(k, "\nMifare version: ")) return false;
    if (!put_bytes(k, (x && x->mifare_version) ? x->mifare_version : default_version, 8)) return false;
    if (!put_chr(k, '\n')) return false;

    for (unsigned i = 0; i < 3; i++) {
        if (!put_str(k, "Counter ") || !put_u32(k, i) || !put_str(k, ": ") ||
            !put_u32(k, x ? x->counters[i] : 0) || !put_chr(k, '\n')) return false;
        if (!put_str(k, "Tearing ") || !put_u32(k, i) || !put_str(k, ": ") ||
            !put_hex2(k, x ? x->tearing[i] : 0) || !put_chr(k, '\n')) return false;
    }
    if (!put_str(k, "Pages total: ") || !put_u32(k, pages) || !put_chr(k, '\n')) return false;
    unsigned read = (x && x->pages_read >= 0) ? (unsigned)x->pages_read : pages;
    if (!put_str(k, "Pages read: ") || !put_u32(k, read) || !put_chr(k, '\n')) return false;
    for (unsigned i = 0; i < pages; i++)
        if (!memory_line(k, "Page ", i, 4, page, ctx)) return false;
    return put_str(k, "Failed authentication attempts: 0\n");
}

bool write_classic(const Sink &k, const uint8_t *uid, unsigned uid_len,
                   const uint8_t *atqa, uint8_t sak, const char *mc_type,
                   unsigned blocks, Getter block, void *ctx) {
    if (!header(k, NFC_DT_CLASSIC, uid, uid_len, atqa, sak)) return false;
    if (!put_str(k, "Mifare Classic type: ") || !put_str(k, mc_type) ||
        !put_str(k, "\nData format version: 2\n")) return false;
    for (unsigned i = 0; i < blocks; i++)
        if (!memory_line(k, "Block ", i, 16, block, ctx)) return false;
    return true;
}

// --- naming --------------------------------------------------------------------------------

static const char *ntag_by_pages(unsigned pages) {
    switch (pages) {
        case 45:  return "NTAG213";
        case 135: return "NTAG215";
        case 231: return "NTAG216";
        case 20:  return "Mifare Ultralight 11";
        case 41:  return "Mifare Ultralight 21";
        case 16:  return "Mifare Ultralight";
        default:  return "NTAG215";         // what the MicroPython table answered for a count it did not know
    }
}

Kind identify(uint8_t sak, const uint8_t *atqa, unsigned atqa_len, unsigned mem_pages) {
    switch (sak) {
        case 0x08: case 0x88: return { NFC_DT_CLASSIC, "1K" };
        case 0x18: case 0x98: return { NFC_DT_CLASSIC, "4K" };
        case 0x09:            return { NFC_DT_CLASSIC, "Mini" };
        default: break;
    }
    if (sak == 0x00) {
        for (unsigned i = 0; i < atqa_len; i++)
            if (atqa[i] == 0x44) return { NFC_DT_ULTRALIGHT, ntag_by_pages(mem_pages) };
    }
    return { NFC_DT_ISO3A, nullptr };
}

unsigned classic_block_count(const char *mc_type) {
    if (strchr(mc_type, '4')) return 256;
    if (strstr(mc_type, "ini")) return 20;
    return 64;
}

}  // namespace nfc
}  // namespace nova
