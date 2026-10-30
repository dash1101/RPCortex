#include "scanlist.h"

#include <string.h>

void scanlist_init(ScanList *l, ScanRec *store, uint32_t cap) {
    l->rec  = store;
    l->cap  = cap;
    l->n    = 0;
    l->lost = 0;
}

// The name as a C string. Cut at the first NUL because everything downstream
// holds names that way; a beacon whose name begins with a NUL is the usual way a
// hidden network advertises, and is hidden here too.
static uint32_t name_copy(char *out, const uint8_t *ssid, unsigned len) {
    if (len > SCANLIST_SSID_MAX) len = SCANLIST_SSID_MAX;
    uint32_t n = 0;
    while (n < len && ssid && ssid[n]) { out[n] = (char)ssid[n]; n++; }
    out[n] = 0;
    return n;
}

static void fill(ScanRec *r, const uint8_t bssid[6], const char *name, uint32_t name_n,
                 int rssi, int channel, unsigned sec) {
    memcpy(r->bssid, bssid, 6);
    r->rssi    = (int16_t)rssi;
    r->channel = (uint16_t)channel;
    r->sec     = (uint8_t)sec;
    r->hidden  = name_n == 0;
    memcpy(r->ssid, name, name_n + 1);
}

int scanlist_add(ScanList *l, const uint8_t bssid[6], const uint8_t *ssid,
                 unsigned ssid_len, int rssi, int channel, unsigned sec) {
    char name[SCANLIST_SSID_MAX + 1];
    uint32_t name_n = name_copy(name, ssid, ssid_len);

    for (uint32_t i = 0; i < l->n; i++) {
        ScanRec *r = &l->rec[i];
        if (memcmp(r->bssid, bssid, 6) != 0) continue;
        // A name heard late is kept even when this sighting is the weaker one;
        // it is the only time the network said what it is called.
        bool had_name = r->ssid[0] != 0;
        if (rssi > r->rssi) {
            r->rssi    = (int16_t)rssi;
            r->channel = (uint16_t)channel;
            r->sec     = (uint8_t)sec;
        }
        if (name_n && !had_name) {
            memcpy(r->ssid, name, name_n + 1);
            r->hidden = 0;
        }
        return SCANLIST_MERGED;
    }

    if (l->n < l->cap) {
        fill(&l->rec[l->n++], bssid, name, name_n, rssi, channel, sec);
        return SCANLIST_NEW;
    }

    // Full. Find the faintest held radio; the earliest wins a tie so the answer
    // does not depend on anything but the order of arrival.
    uint32_t weak = 0;
    for (uint32_t i = 1; i < l->n; i++)
        if (l->rec[i].rssi < l->rec[weak].rssi) weak = i;
    l->lost++;
    if (l->n == 0 || rssi <= l->rec[weak].rssi) return SCANLIST_FULL;
    fill(&l->rec[weak], bssid, name, name_n, rssi, channel, sec);
    return SCANLIST_NEW;
}

void scanlist_sort(ScanList *l) {
    // Insertion sort: stable, and at most a few dozen entries.
    for (uint32_t i = 1; i < l->n; i++) {
        ScanRec key = l->rec[i];
        uint32_t j = i;
        while (j > 0 && l->rec[j - 1].rssi < key.rssi) { l->rec[j] = l->rec[j - 1]; j--; }
        l->rec[j] = key;
    }
}

void scanlist_bssid_text(const uint8_t b[6], char out[18]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 3]     = hex[b[i] >> 4];
        out[i * 3 + 1] = hex[b[i] & 15];
        out[i * 3 + 2] = i < 5 ? ':' : 0;
    }
}
