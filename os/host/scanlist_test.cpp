// The per-radio scan list, fed the ways a scan actually arrives.
//
// The driver reports every beacon it hears, so the interesting cases are all
// about repeats: the same radio twice, the same radio again but weaker, a
// network whose name is only heard on a later answer, two radios under one
// name, and more radios than there is room for. The firmware's own list merges
// by name; the point of this one is that it must not.
#include "../core/scanlist.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}
static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want) != 0) {
        printf("  FAIL: %-40s got '%s', want '%s'\n", what, got, want);
        fails++;
    }
}

static void mac(uint8_t out[6], int last) {
    static const uint8_t base[6] = { 0x1c, 0x4b, 0xd6, 0x00, 0xab, 0x00 };
    memcpy(out, base, 6);
    out[5] = (uint8_t)last;
}
static int add(ScanList *l, int last, const char *name, int rssi, int ch, unsigned sec) {
    uint8_t b[6];
    mac(b, last);
    return scanlist_add(l, b, (const uint8_t *)name, name ? (unsigned)strlen(name) : 0,
                        rssi, ch, sec);
}

static void repeats(void) {
    ScanRec store[8];
    ScanList l;
    scanlist_init(&l, store, 8);

    ck(add(&l, 1, "Home", -70, 6, SCAN_SEC_WPA2) == SCANLIST_NEW, "first sighting is new");
    ck(add(&l, 1, "Home", -70, 6, SCAN_SEC_WPA2) == SCANLIST_MERGED, "same radio again merges");
    ck(l.n == 1, "one radio, one row");

    // A weaker repeat must not drag the signal down; a stronger one lifts it,
    // and brings its channel and security with it as one sighting.
    add(&l, 1, "Home", -85, 11, SCAN_SEC_WPA);
    ck(l.rec[0].rssi == -70 && l.rec[0].channel == 6 && l.rec[0].sec == SCAN_SEC_WPA2,
       "weaker repeat changes nothing");
    add(&l, 1, "Home", -50, 1, SCAN_SEC_WPA2 | SCAN_SEC_WPA);
    ck(l.rec[0].rssi == -50 && l.rec[0].channel == 1 && l.rec[0].sec == (SCAN_SEC_WPA2 | SCAN_SEC_WPA),
       "stronger repeat takes the whole sighting");
    ck(l.n == 1, "still one row");
}

static void two_radios_one_name(void) {
    ScanRec store[8];
    ScanList l;
    scanlist_init(&l, store, 8);
    add(&l, 1, "Campus", -60, 1, SCAN_SEC_WPA2);
    add(&l, 2, "Campus", -72, 36, SCAN_SEC_WPA2);
    ck(l.n == 2, "two radios under one name are two rows");
    scanlist_sort(&l);
    ck(l.rec[0].bssid[5] == 1 && l.rec[1].bssid[5] == 2, "strongest first");
}

static void hidden(void) {
    ScanRec store[8];
    ScanList l;
    scanlist_init(&l, store, 8);

    // Empty, absent and all-NUL names are all hidden, and none is dropped.
    add(&l, 1, "", -60, 6, SCAN_SEC_WPA2);
    uint8_t b[6]; mac(b, 2);
    scanlist_add(&l, b, nullptr, 0, -61, 6, SCAN_SEC_WPA2);
    mac(b, 3);
    const uint8_t zeros[4] = { 0, 0, 0, 0 };
    scanlist_add(&l, b, zeros, 4, -62, 6, SCAN_SEC_WPA2);
    ck(l.n == 3, "hidden networks are kept");
    ck(l.rec[0].hidden && l.rec[1].hidden && l.rec[2].hidden, "all three are marked hidden");

    // The name arrives on a later answer, and is kept even though that answer
    // is the weaker sighting.
    add(&l, 1, "Secret", -80, 6, SCAN_SEC_WPA2);
    eq(l.rec[0].ssid, "Secret", "a late name fills the blank");
    ck(!l.rec[0].hidden, "no longer hidden");
    ck(l.rec[0].rssi == -60, "and the signal stays the strongest");

    // Once named, a later empty answer must not blank it again.
    add(&l, 1, "", -40, 6, SCAN_SEC_WPA2);
    eq(l.rec[0].ssid, "Secret", "an empty answer does not erase the name");
}

static void names(void) {
    ScanRec store[4];
    ScanList l;
    scanlist_init(&l, store, 4);

    // Raw beacon bytes: cut at a NUL, capped at 32, never read past the length.
    uint8_t b[6]; mac(b, 1);
    const uint8_t cut[8] = { 'a', 'b', 0, 'c', 'd', 'e', 'f', 'g' };
    scanlist_add(&l, b, cut, 8, -50, 1, 0);
    eq(l.rec[0].ssid, "ab", "cut at the first NUL");

    mac(b, 2);
    uint8_t long_name[40];
    memset(long_name, 'x', sizeof(long_name));
    scanlist_add(&l, b, long_name, 40, -50, 1, 0);
    ck(strlen(l.rec[1].ssid) == 32, "capped at 32 characters");
    ck(l.rec[1].ssid[32] == 0, "and terminated");

    mac(b, 3);
    const uint8_t pre[5] = { 'o', 'n', 'l', 'y', 'X' };
    scanlist_add(&l, b, pre, 4, -50, 1, 0);
    eq(l.rec[2].ssid, "only", "the length is honoured, not the terminator");
}

static void crowded(void) {
    ScanRec store[3];
    ScanList l;
    scanlist_init(&l, store, 3);
    add(&l, 1, "a", -50, 1, 0);
    add(&l, 2, "b", -80, 1, 0);
    add(&l, 3, "c", -60, 1, 0);

    // A fourth radio fainter than all of them is lost; a stronger one pushes
    // out the faintest. Either way the loss is counted, not silent.
    ck(add(&l, 4, "d", -90, 1, 0) == SCANLIST_FULL, "a fainter radio is turned away");
    ck(l.lost == 1, "and counted");
    ck(add(&l, 5, "e", -55, 1, 0) == SCANLIST_NEW, "a stronger one takes a place");
    ck(l.lost == 2, "the one it pushed out is counted too");
    ck(l.n == 3, "never more than there is room for");
    bool has_b = false, has_e = false;
    for (uint32_t i = 0; i < l.n; i++) {
        if (!strcmp(l.rec[i].ssid, "b")) has_b = true;
        if (!strcmp(l.rec[i].ssid, "e")) has_e = true;
    }
    ck(!has_b && has_e, "the faintest was the one replaced");

    // As loud as the faintest held is not louder: the one already there stays.
    ck(add(&l, 6, "f", -60, 1, 0) == SCANLIST_FULL, "a tie does not displace");

    // A radio already held still merges when the list is full.
    ck(add(&l, 1, "a", -40, 1, 0) == SCANLIST_MERGED, "a held radio merges when full");
    ck(l.rec[0].rssi == -40 || l.rec[1].rssi == -40 || l.rec[2].rssi == -40, "and updates");

    // Zero room must not fault.
    ScanList z;
    scanlist_init(&z, store, 0);
    ck(add(&z, 1, "x", -50, 1, 0) == SCANLIST_FULL, "no room at all is refused cleanly");
}

static void sorting(void) {
    ScanRec store[6];
    ScanList l;
    scanlist_init(&l, store, 6);
    add(&l, 1, "w", -70, 1, 0);
    add(&l, 2, "x", -50, 1, 0);
    add(&l, 3, "y", -70, 1, 0);
    add(&l, 4, "z", -30, 1, 0);
    scanlist_sort(&l);
    ck(l.rec[0].bssid[5] == 4 && l.rec[1].bssid[5] == 2, "strongest first");
    ck(l.rec[2].bssid[5] == 1 && l.rec[3].bssid[5] == 3, "equal signals keep the order heard");
}

static void text(void) {
    uint8_t b[6] = { 0x1C, 0x4B, 0xD6, 0x00, 0xAB, 0x0F };
    char s[18];
    scanlist_bssid_text(b, s);
    eq(s, "1c:4b:d6:00:ab:0f", "lowercase, colon separated");
    ck(strlen(s) == 17, "17 characters");
}

int main() {
    repeats();
    two_radios_one_name();
    hidden();
    names();
    crowded();
    sorting();
    text();
    printf("scanlist_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
