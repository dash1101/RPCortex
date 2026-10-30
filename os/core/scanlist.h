// Access points found by one Wi-Fi scan, kept one per radio.
//
// The driver reports every beacon it hears, so one access point arrives many
// times, and a network with several radios (a mesh, a range extender, a campus)
// arrives under one name from several addresses. The shell's own
// list merges by NAME, which is right for choosing something to join and wrong
// for anything that wants to know what is in the air: two radios are two rows.
// This list is keyed by BSSID, the radio's own address.
//
// It is plain arithmetic over a caller-supplied array, free of the driver, so
// every way a scan can arrive (a repeat, a weaker repeat, a hidden name that is
// revealed by a later answer, more radios than room) can be fed to it on the
// host. Same split as nfcframe.h and for the same reason.
#ifndef RPC_SCANLIST_H
#define RPC_SCANLIST_H

#include <stdint.h>

#define SCANLIST_SSID_MAX 32

// The security summary a scan result carries is three bits built from the
// beacon's information elements. It is NOT one of the CYW43_AUTH_* join
// constants (see the note above auth_text in net.cpp).
#define SCAN_SEC_WEP  0x01
#define SCAN_SEC_WPA  0x02
#define SCAN_SEC_WPA2 0x04

struct ScanRec {
    uint8_t  bssid[6];
    int16_t  rssi;
    uint16_t channel;
    uint8_t  sec;                          // SCAN_SEC_* bits; 0 is open
    uint8_t  hidden;                       // the name has not been heard
    char     ssid[SCANLIST_SSID_MAX + 1];
};

struct ScanList {
    ScanRec *rec;
    uint32_t cap;
    uint32_t n;
    uint32_t lost;       // radios heard that the list had no room to keep
};

enum { SCANLIST_FULL = -1, SCANLIST_MERGED = 0, SCANLIST_NEW = 1 };

void scanlist_init(ScanList *l, ScanRec *store, uint32_t cap);

// One sighting. A BSSID already held is merged: the strongest sighting keeps its
// signal, channel and security, and a name heard later fills in one that was
// not (a hidden network answers a probe with its name). A new BSSID that does
// not fit replaces the weakest held one if it is stronger, so a crowded scan
// loses the faintest radios rather than the last ones to arrive.
//
// `ssid` is the raw bytes from the beacon and need not be terminated; it is cut
// at the first NUL, and a name that is empty or all NULs is hidden.
int scanlist_add(ScanList *l, const uint8_t bssid[6], const uint8_t *ssid,
                 unsigned ssid_len, int rssi, int channel, unsigned sec);

// Strongest first, and stable, so equal signals stay in the order heard.
void scanlist_sort(ScanList *l);

// The "1c:4b:d6:00:ab:cd" form, lowercase, 17 characters and a NUL.
void scanlist_bssid_text(const uint8_t bssid[6], char out[18]);

#endif
