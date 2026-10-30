// Desc: WiGLE-1.4 CSV rows - see novawigle.h.
// File: novawigle.cpp
#include "novawigle.h"

#include <string.h>

namespace nova {
namespace wigle {

using codec::Sink;
using codec::put_chr;
using codec::put_str;

const char *sec_text(unsigned bits) {
    if (bits == 0)                              return "OPEN";
    if ((bits & 4) && (bits & 2))               return "WPA/WPA2-PSK";
    if (bits & 4)                               return "WPA2-PSK";
    if (bits & 2)                               return "WPA-PSK";
    if (bits & 1)                               return "WEP";
    return "WPA?";                              // a bit nobody has named; the oracle's own fallback
}

bool put_header(const Sink &k, const char *model) {
    return put_str(k, "WigleWifi-1.4,appRelease=novad1,model=") && put_str(k, model) &&
           put_str(k, ",release=1.0,device=NovaD1,display=,board=rp2350,brand=NovaLabs\n"
                       "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,"
                       "CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n");
}

bool put_field(const Sink &k, const char *s) {
    bool quote = false;
    for (const char *p = s; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\n') { quote = true; break; }
    if (!quote) return put_str(k, s);
    if (!put_chr(k, '"')) return false;
    for (const char *p = s; *p; p++) {
        if (*p == '"' && !put_chr(k, '"')) return false;
        if (!put_chr(k, *p)) return false;
    }
    return put_chr(k, '"');
}

void bssid_text(const uint8_t *b, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 3]     = hex[b[i] >> 4];
        out[i * 3 + 1] = hex[b[i] & 15];
        out[i * 3 + 2] = i < 5 ? ':' : 0;
    }
}

// Degrees x 100000 as "-12.345670": the six decimals "{:.6f}" gave, the last of them always 0
// because a receiver's coordinate has no more than five.
static bool wg_put_deg(const Sink &k, int32_t e5) {
    uint32_t a = e5 < 0 ? (uint32_t)0 - (uint32_t)e5 : (uint32_t)e5;
    if (e5 < 0 && !put_chr(k, '-')) return false;
    if (!codec::put_u32(k, a / 100000u) || !put_chr(k, '.')) return false;
    char d[6];
    uint32_t f = a % 100000u;
    for (int i = 4; i >= 0; i--) { d[i] = (char)('0' + f % 10); f /= 10; }
    d[5] = '0';
    return k.put(k.ctx, d, 6);
}

// Tenths of a metre as "545.4", which is what str() gave a float altitude.
static bool wg_put_tenths(const Sink &k, int32_t dm) {
    uint32_t a = dm < 0 ? (uint32_t)0 - (uint32_t)dm : (uint32_t)dm;
    if (dm < 0 && !put_chr(k, '-')) return false;
    return codec::put_u32(k, a / 10u) && put_chr(k, '.') && put_chr(k, (char)('0' + a % 10u));
}

bool put_row(const Sink &k, const Row &r) {
    char mac[18];
    bssid_text(r.bssid, mac);
    if (!put_str(k, mac) || !put_chr(k, ',')) return false;
    if (!put_field(k, r.ssid) || !put_chr(k, ',')) return false;
    if (!put_chr(k, '[') || !put_str(k, sec_text(r.sec)) || !put_str(k, "],")) return false;
    if (!put_str(k, r.time) || !put_chr(k, ',')) return false;
    if (!codec::put_i32(k, r.channel) || !put_chr(k, ',')) return false;
    if (!codec::put_i32(k, r.rssi) || !put_chr(k, ',')) return false;
    if (r.has_pos) {
        if (!wg_put_deg(k, r.lat_e5) || !put_chr(k, ',') || !wg_put_deg(k, r.lon_e5)) return false;
    } else if (!put_chr(k, ',')) {
        return false;
    }
    if (!put_chr(k, ',')) return false;
    if (r.has_alt && !wg_put_tenths(k, r.alt_dm)) return false;
    // The accuracy column is empty - a receiver's estimate is not read - and Type is always WIFI.
    return put_str(k, ",,WIFI\n");
}

void seen_reset(Seen *s) { s->n = 0; s->next = 0; }

bool seen_note(Seen *s, const uint8_t *bssid) {
    for (uint16_t i = 0; i < s->n; i++)
        if (memcmp(s->b[i], bssid, 6) == 0) return true;
    if (s->n < WIGLE_SEEN_MAX) {
        memcpy(s->b[s->n++], bssid, 6);
    } else {
        memcpy(s->b[s->next], bssid, 6);
        s->next = (uint16_t)((s->next + 1) % WIGLE_SEEN_MAX);
    }
    return false;
}

}  // namespace wigle
}  // namespace nova
