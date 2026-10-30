// Desc: WiGLE-1.4 CSV rows for a Wi-Fi survey, and the memory of which radios were already logged.
// File: novawigle.h
//
// Ported from novawardrive.py (sec_str, wigle_header, _csv_field, wigle_row, Session), and held to
// it by gen_codec_vectors.py. Pure: no fw_* call, no float, writes through a Sink.
//
// The security column is the simplified form the MicroPython suite wrote - "[WPA2-PSK]", not the
// "[WPA2-PSK-CCMP][ESS]" a Linux scan prints. It has not been uploaded to WiGLE from this port.
#ifndef NOVA_WIGLE_H
#define NOVA_WIGLE_H

#include <stdint.h>

#include "novacodec.h"

namespace nova {
namespace wigle {

// The scan result's three security bits (SCAN_SEC_* in core/scanlist.h: 1 WEP, 2 WPA, 4 WPA2)
// as the column text. Several bits set read as the strongest of them, the way the shell prints
// the same scan.
const char *sec_text(unsigned bits);

// The two header lines. `model` is what the first line calls the device.
bool put_header(const codec::Sink &k, const char *model);

// A CSV field, quoted and doubled only when it has to be (a comma, a quote or a newline).
bool put_field(const codec::Sink &k, const char *s);

struct Row {
    const uint8_t *bssid;       // six bytes
    const char    *ssid;        // "" for a hidden network
    unsigned       sec;
    const char    *time;        // "YYYY-MM-DD HH:MM:SS" UTC, or "" when the clock was never set
    int            channel;
    int            rssi;
    bool           has_pos;     // a fix: the next two are meaningful
    int32_t        lat_e5;      // degrees x 100000, as the GPS module hands them over
    int32_t        lon_e5;
    bool           has_alt;
    int32_t        alt_dm;      // tenths of a metre
};

// One data row, newline included. Position and altitude are blank columns when there is no fix.
bool put_row(const codec::Sink &k, const Row &r);

// "aa:bb:cc:dd:ee:ff" into a 18-byte buffer.
void bssid_text(const uint8_t *b, char *out);

// Which radios a survey has already written. Bounded, because a package has a few kilobytes to
// spare and a long drive meets more radios than that: when it is full the OLDEST is forgotten, so
// a radio met again after a long time is logged again - a second sighting is a harmless extra row
// to WiGLE, and a table that stopped remembering would stop logging anything new instead.
#define WIGLE_SEEN_MAX 256

struct Seen {
    uint8_t  b[WIGLE_SEEN_MAX][6];
    uint16_t n;
    uint16_t next;      // the slot to overwrite once full
};

void seen_reset(Seen *s);

// True if this radio is already held. Otherwise it is remembered and false comes back, so one call
// is both the question and the note.
bool seen_note(Seen *s, const uint8_t *bssid);

}  // namespace wigle
}  // namespace nova

#endif  // NOVA_WIGLE_H
