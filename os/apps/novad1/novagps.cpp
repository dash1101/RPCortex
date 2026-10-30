// Desc: NMEA parsing - see novagps.h.
// File: novagps.cpp
#include "novagps.h"

#include <string.h>
#include <stdio.h>

namespace nova {
namespace gps {

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool checksum_ok(const char *s) {
    if (!s || s[0] != '$') return false;
    unsigned x = 0;
    const char *p = s + 1;
    for (; *p && *p != '*'; p++) x ^= (unsigned char)*p;
    if (*p != '*') return false;
    int hi = hexv(p[1]), lo = hexv(p[2]);
    return hi >= 0 && lo >= 0 && (unsigned)((hi << 4) | lo) == x;
}

bool field(const char *s, int n, char *out, unsigned cap) {
    const char *p = s + 1;                          // past '$'
    while (n > 0 && *p && *p != '*') {
        if (*p == ',') n--;
        p++;
    }
    if (cap) out[0] = 0;
    if (n > 0 || !*p || *p == '*') return false;
    unsigned i = 0;
    while (*p && *p != ',' && *p != '*' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return true;
}

// A decimal "12.345" as an integer scaled by 10^decimals, rounding half up on a longer fraction.
// False on anything that is not a plain non-negative decimal.
static bool dec_scaled(const char *s, int decimals, int32_t *out) {
    if (!s || !*s) return false;
    int32_t whole = 0;
    const char *p = s;
    bool any = false;
    while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); p++; any = true; if (whole > 20000000) return false; }
    int32_t frac = 0, scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    int32_t pos = scale;
    bool round_up = false;
    if (*p == '.') {
        p++;
        for (int i = 0; *p >= '0' && *p <= '9'; p++, i++) {
            any = true;
            if (i < decimals) { pos /= 10; frac += (*p - '0') * pos; }
            else if (i == decimals) round_up = *p >= '5';
        }
    }
    if (!any || *p) return false;
    int32_t v = whole * scale + frac + (round_up ? 1 : 0);
    *out = v;
    return true;
}

bool coord_e5(const char *v, const char *hemi, int32_t *out) {
    if (!v || !*v) return false;
    const char *dot = strchr(v, '.');
    int dl = (int)(dot ? (dot - v) : (int)strlen(v)) - 2;       // digits of degrees: all but the last two before the dot
    if (dl < 1 || dl > 3) return false;
    int32_t deg = 0;
    for (int i = 0; i < dl; i++) { if (v[i] < '0' || v[i] > '9') return false; deg = deg * 10 + (v[i] - '0'); }
    int32_t min_e4;
    if (!dec_scaled(v + dl, 4, &min_e4)) return false;           // minutes, four decimals: 07.0380 -> 70380
    if (min_e4 >= 600000) return false;                          // 60 minutes or more is not a coordinate
    // minutes / 60 as degrees x 10^5 is minutes_e4 / 6, rounded.
    int32_t e5 = deg * 100000 + (min_e4 + 3) / 6;
    if (hemi && (hemi[0] == 'S' || hemi[0] == 'W')) e5 = -e5;
    *out = e5;
    return true;
}

void format_deg(int32_t e5, char *out, unsigned cap) {
    unsigned u = (unsigned)(e5 < 0 ? -e5 : e5);
    snprintf(out, cap, "%s%u.%05u", e5 < 0 ? "-" : "", u / 100000u, u % 100000u);
}

void format_tenths(int32_t t, char *out, unsigned cap) {
    unsigned u = (unsigned)(t < 0 ? -t : t);
    snprintf(out, cap, "%s%u.%u", t < 0 ? "-" : "", u / 10u, u % 10u);
}

static int two(const char *s) { return (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') ? (s[0] - '0') * 10 + (s[1] - '0') : -1; }

static void take_time(Fix *f, const char *t) {
    if (strlen(t) < 6) return;
    int h = two(t), m = two(t + 2), s = two(t + 4);
    if (h < 0 || m < 0 || s < 0 || h > 23 || m > 59 || s > 60) return;
    f->hh = h; f->mm = m; f->ss = s > 59 ? 59 : s;
    f->has_time = true;
}

bool line_push(LineBuf *b, char ch, const char **out) {
    if (ch == '\r' || ch == '\n') {
        if (b->n <= 0) return false;
        b->line[b->n] = 0;
        b->n = 0;
        *out = b->line;
        return true;
    }
    if (b->n + 1 < (int)sizeof(b->line)) b->line[b->n++] = ch;
    else b->n = 0;                  // too long to be real NMEA: resync at the next newline
    return false;
}

bool consume(Fix *f, const char *s) {
    if (!checksum_ok(s)) return false;
    f->have = true;
    char t[4] = { 0, 0, 0, 0 };
    if (strlen(s) >= 6) { t[0] = s[3]; t[1] = s[4]; t[2] = s[5]; }
    memcpy(f->last, t, sizeof(f->last));

    char a[24], b[8], c[24], d[8];
    if (!strcmp(t, "GGA")) {
        if (field(s, 1, a, sizeof(a))) take_time(f, a);
        char q[4];
        const bool fix = field(s, 6, q, sizeof(q)) && q[0] && q[0] != '0';
        f->fixed = fix;
        if (fix && field(s, 2, a, sizeof(a)) && field(s, 3, b, sizeof(b)) && field(s, 4, c, sizeof(c)) && field(s, 5, d, sizeof(d))) {
            int32_t la, lo;
            if (coord_e5(a, b, &la) && coord_e5(c, d, &lo)) { f->lat_e5 = la; f->lon_e5 = lo; }
        }
        if (field(s, 7, a, sizeof(a))) { int32_t v; if (dec_scaled(a, 0, &v)) f->sats_used = v; }
        if (field(s, 8, a, sizeof(a))) { int32_t v; if (dec_scaled(a, 1, &v)) f->hdop_x10 = v; }
        if (fix && field(s, 9, a, sizeof(a))) {
            bool neg = a[0] == '-';
            int32_t v;
            if (dec_scaled(a + (neg ? 1 : 0), 1, &v)) f->alt_dm = neg ? -v : v;
        }
        return true;
    }
    if (!strcmp(t, "RMC")) {
        if (field(s, 1, a, sizeof(a))) take_time(f, a);
        char st[4];
        f->fixed = field(s, 2, st, sizeof(st)) && st[0] == 'A';
        if (f->fixed && field(s, 3, a, sizeof(a)) && field(s, 4, b, sizeof(b)) && field(s, 5, c, sizeof(c)) && field(s, 6, d, sizeof(d))) {
            int32_t la, lo;
            if (coord_e5(a, b, &la) && coord_e5(c, d, &lo)) { f->lat_e5 = la; f->lon_e5 = lo; }
        }
        if (field(s, 7, a, sizeof(a))) {
            int32_t kn_e3;
            if (dec_scaled(a, 3, &kn_e3)) f->speed_dkmh = (kn_e3 * 1852 + 50000) / 100000;     // knots -> tenths of km/h; 999.999 kn x 1852 still fits 32 bits
        }
        if (field(s, 9, a, sizeof(a)) && strlen(a) >= 6) {
            int dd = two(a), mo = two(a + 2), yy = two(a + 4);
            if (dd >= 1 && dd <= 31 && mo >= 1 && mo <= 12 && yy >= 0) { f->dd = dd; f->mo = mo; f->yy = yy; f->has_date = true; }
        }
        return true;
    }
    if (!strcmp(t, "GSV")) {
        if (field(s, 3, a, sizeof(a))) { int32_t v; if (dec_scaled(a, 0, &v)) f->sats_view = v; }
        return true;
    }
    return false;
}

}  // namespace gps
}  // namespace nova
