#include "btname.h"

uint32_t bt_name_from_ad(const uint8_t *data, uint32_t len, char *out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!data || len == 0) return 0;

    const uint8_t *best = nullptr;
    uint32_t best_len = 0;
    bool best_is_full = false;

    uint32_t i = 0;
    while (i < len) {
        uint32_t rec = data[i];

        // A zero length ends the list — that is the specification, and without
        // it a run of padding zeroes is an infinite loop.
        if (rec == 0) break;

        // The record claims rec bytes after the length byte. If that runs past
        // the end the packet is malformed and everything after it is
        // unreadable, so it stops rather than reading on.
        if (i + 1 + rec > len) break;

        uint8_t type = data[i + 1];
        const uint8_t *val = &data[i + 2];
        uint32_t vlen = rec - 1;          // the type byte is part of rec

        if (type == BT_AD_NAME_FULL || type == BT_AD_NAME_SHORT) {
            // The complete name wins. A device sending both means the short one
            // as a fallback, and picking whichever came last would be a coin
            // flip on the advertiser's ordering.
            bool is_full = (type == BT_AD_NAME_FULL);
            if (!best || (is_full && !best_is_full)) {
                best = val;
                best_len = vlen;
                best_is_full = is_full;
            }
        }

        i += 1 + rec;
    }

    if (!best || best_len == 0) return 0;

    uint32_t n = best_len < cap - 1 ? best_len : cap - 1;
    uint32_t w = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t c = best[k];
        // Names arrive from strangers and end up in a terminal. A control byte
        // in one is an escape sequence somebody else chose, so anything outside
        // printable ASCII becomes a dot rather than being passed through.
        out[w++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    out[w] = 0;
    return w;
}
