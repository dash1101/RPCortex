// Desc: NMEA sentences from a GPS module, parsed into a position, a time and a speed.
// File: novagps.h
//
// Pure: a sentence in, numbers out, no fw_* call, so it is checked on a host against the sentences
// in the NMEA 0183 documentation and against the MicroPython screen's own coordinate conversion.
//
// NO FLOATING POINT. A package has none, and does not need it: a coordinate is degrees times
// 100000 (a metre is about 9 of them), a speed is tenths of a kilometre an hour, an altitude is
// tenths of a metre. They print as the decimals a person reads.
#ifndef NOVA_GPS_H
#define NOVA_GPS_H

#include <stdint.h>

namespace nova {
namespace gps {

struct Fix {
    bool    have;           // a sentence whose checksum added up has been read
    bool    fixed;          // and the receiver says it has a position
    int32_t lat_e5;         // degrees x 100000, south negative
    int32_t lon_e5;         // west negative
    int32_t alt_dm;         // altitude, tenths of a metre
    int32_t speed_dkmh;     // tenths of a kilometre an hour
    int     sats_used;
    int     sats_view;
    int     hdop_x10;
    bool    has_time, has_date;
    int     hh, mm, ss;     // UTC
    int     dd, mo, yy;     // yy is the two digits the sentence has
    char    last[4];        // the type of the last sentence read: "GGA", "RMC", ...
};

// NMEA's checksum: every byte between '$' and '*' XORed, as two hex digits after the '*'. Any
// sentence whose sum is right is a real receiver talking, whatever it says.
bool checksum_ok(const char *sentence);

// The Nth comma-separated field, counting the sentence id itself as field 0. False (and `out`
// emptied) when the sentence runs out first.
bool field(const char *sentence, int n, char *out, unsigned cap);

// Fold one complete sentence (no line ending) into the fix. Returns true when it was a good
// sentence of a kind that is understood; GGA, RMC and GSV change the fix, anything else with a
// good checksum only proves a receiver is there.
bool consume(Fix *f, const char *sentence);

// "4807.038" with "N" to degrees x 100000. False when it is not a coordinate.
bool coord_e5(const char *ddmm, const char *hemi, int32_t *out);

// "-12.34567" for degrees x 100000.
void format_deg(int32_t e5, char *out, unsigned cap);

// "545.4 m", "41.5 km/h" - tenths with one decimal.
void format_tenths(int32_t tenths, char *out, unsigned cap);

}  // namespace gps
}  // namespace nova

#endif  // NOVA_GPS_H
