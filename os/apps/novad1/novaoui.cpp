// Desc: MAC prefix to vendor - see novaoui.h.
// File: novaoui.cpp
#include "novaoui.h"

#include <string.h>

namespace nova {
namespace oui {

#include "novaoui_data.inc"

static const char *nth(const char *blob, unsigned n) {
    while (n--) blob += strlen(blob) + 1;
    return blob;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Read a text MAC the way novaoui.py did: drop ':' and '-', and ask for six
// characters at least. The first octet is read on its own, because a randomised
// address is recognised from it before the rest is looked at; a prefix with a
// character that is not hex in it cannot be in the table, so it finds nothing.
enum Parse { P_SHORT, P_BAD_FIRST, P_OK };

static Parse prefix_of(const char *s, uint32_t *key, unsigned *first, bool *key_ok) {
    unsigned total = 0, n = 0, f = 0;
    uint32_t v = 0;
    bool ok = true;
    for (; s && *s; s++) {
        if (*s == ':' || *s == '-') continue;
        total++;
        if (n < 6) {
            int d = hexval(*s);
            if (d < 0) ok = false; else v = (v << 4) | (uint32_t)d;
            if (n < 2) { if (d < 0) f = 0x100; else if (f < 0x100) f = (f << 4) | (unsigned)d; }
            n++;
        }
    }
    if (total < 6) return P_SHORT;
    if (f >= 0x100) return P_BAD_FIRST;
    *key = v; *first = f; *key_ok = ok;
    return P_OK;
}

static int find(uint32_t key) {
    int lo = 0, hi = (int)OUI_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *r = kOuiTable + mid * 4;
        uint32_t k = ((uint32_t)r[0] << 16) | ((uint32_t)r[1] << 8) | r[2];
        if (k == key) return r[3];
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static Result by_prefix(uint32_t key, unsigned first) {
    Result r = { nullptr, nullptr };
    if (first & 0x02) { r.klass = "random"; return r; }
    int v = find(key);
    if (v < 0 || v >= (int)OUI_VENDORS_N) return r;
    r.vendor = nth(kOuiVendors, (unsigned)v);
    r.klass  = nth(kOuiClassNames, kOuiVendorClass[v]);
    return r;
}

Result lookup(const char *mac_text) {
    uint32_t key = 0; unsigned first = 0; bool key_ok = false;
    Result none = { nullptr, nullptr };
    if (prefix_of(mac_text, &key, &first, &key_ok) != P_OK) return none;
    if (first & 0x02) return by_prefix(0, first);          // randomised: no table lookup at all
    if (!key_ok) return none;
    return by_prefix(key, first);
}

Result lookup_bytes(const uint8_t *mac, unsigned n) {
    Result none = { nullptr, nullptr };
    if (!mac || n < 3) return none;
    return by_prefix(((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2], mac[0]);
}

void prefix(const char *mac_text, char *out, unsigned cap) {
    if (cap == 0) return;
    char digits[7];
    unsigned n = 0;
    for (const char *s = mac_text; s && *s && n < 6; s++) {
        if (*s == ':' || *s == '-') continue;
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        digits[n++] = c;
    }
    if (n < 6 || cap < 9) { out[0] = '?'; if (cap > 1) out[1] = 0; return; }
    out[0] = digits[0]; out[1] = digits[1]; out[2] = ':';
    out[3] = digits[2]; out[4] = digits[3]; out[5] = ':';
    out[6] = digits[4]; out[7] = digits[5]; out[8] = 0;
}

unsigned table_size(void) { return OUI_N; }

}  // namespace oui
}  // namespace nova
