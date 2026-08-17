// Desc: The Bluetooth screens — what is around, how strong it is, and who came home.
// File: novagui_ble.cpp
//
// Ported from novable.py, novableid.py and novagui_watch.py, against an ABI that
// reaches the radio very differently from the way MicroPython did.
//
// THERE IS NO BLE CALL IN THE PACKAGE ABI. What exists is the firmware's own `bt`
// shell command, run through fw_shell_run, which hands back the text it printed:
//
//     bt scan le <seconds>        every device advertising
//     bt scan classic <seconds>   every discoverable device
//     bt status                   is the stack up
//     bt advertise ...            transmit a chosen advertisement, bounded in time
//
// Everything below is built on parsing that text, and three things follow from
// it that shaped every screen here:
//
//   * A scan BLOCKS. Measured on a Pico 2 W, `bt scan le 5` takes about twelve
//     seconds end to end — up to three of them bringing the stack up the first
//     time, five scanning, and the rest printing two dozen rows at 115200.
//     Called from tick() that is twelve seconds of frozen panel, so it runs on a
//     task and tick() only ever looks at the result.
//   * THE ADVERTISEMENT ITSELF NEVER ARRIVES on the scan path. `bt scan` prints
//     an address, LE or BR, an RSSI and a name; the raw AD payload of what it
//     HEARS stays in the firmware. So all of novableid — company identifiers,
//     service UUIDs, TX power, the Apple message types — has nothing to decode
//     and is not ported. Neither is the tracker mark Radar used to show, because
//     that came from the manufacturer data.
//   * TRANSMITTING, though, is here. `bt advertise` was wired into the firmware,
//     so novable's proximity ping — the crafted Apple Continuity / Google Fast
//     Pair beacon that raises a pairing card on a nearby phone — is back, on the
//     Ping screen. The firmware builds the payload and bounds the transmission;
//     this file just drives `bt advertise ping ios|android` on a worker and
//     shows the countdown, the same shape as the scan screens.
//
// What replaces the v1 background observer: nothing. novawatch ran continuously
// as a service and these screens only read its table. Here the table is filled by
// whichever of these three screens is open, so arrivals and departures are only
// noticed while somebody is looking. Said out loud on the Presence screen rather
// than left to be discovered.
#include "novagui_ble.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"
#include "novanotify.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

// How long one step of a spinner lasts. The same 140 ms the Tools screens
// use, so every wheel on the device turns at one rate.
//
// These three counted FRAMES rather than milliseconds. That was survivable
// while a still screen redrew three times a second and became a blur the
// moment the loop ran at sixty — the wheel spun so fast it read as a static
// smudge, which is the opposite of what a spinner is for.
#define BLE_SPIN_MS 140


using ui::Screen;
using ui::Action;

// --- the shared table ---------------------------------------------------------
//
// One table and one scan for all three screens. Not a tidiness preference: there
// is ONE radio and ONE output capture in the OS, so two screens each running
// their own scan would be two tasks in the same driver and the second would get
// a refused capture and an empty buffer. The table is a file-static because a
// screen has 384 bytes of pool, which this would not fit in even once.

// The firmware keeps at most 24 devices per scan (BT_MAX_SEEN). A little more
// than that here, because this table accumulates ACROSS scans and a room that
// filled one scan will overflow a table the same size on the next.
constexpr int MAX_DEV = 32;

struct Dev {
    char     mac[18];       // "AA:BB:CC:DD:EE:FF", as the shell prints it
    char     name[20];      // very often empty — 15 of 24 in an ordinary room
    int16_t  rssi;          // 0 means the row carried no reading, as `bt` does it
    uint16_t count;         // scans this device has answered
    uint32_t first_ms;
    uint32_t last_ms;
    uint8_t  classic;       // BR, found by inquiry, rather than LE
    uint8_t  gone;          // missed long enough to count as departed
    uint8_t  misses;
    uint8_t  random;        // locally administered address
};

static Dev g_dev[MAX_DEV];
static int g_ndev;

// Not seen in this long and it has left, with one extra pass of hysteresis.
// Both are v1's numbers and v1's reason: BLE advertising is bursty and a device
// can be behind a wall for one pass, so a single miss must not fire a departure.
constexpr uint32_t GONE_MS = 90000;
constexpr uint8_t  HYST    = 2;

// Which device the detail and locate screens are about. A file-static rather
// than a member, the same way ModuleScreen carries its module: two screens need
// the same answer and one of them is pushed from three different places.
static char g_focus[18];

// --- the scan -----------------------------------------------------------------
//
// A generation counter, not a cancel flag. THE SCAN CANNOT BE INTERRUPTED: `bt`
// only breaks out of its wait for a console Ctrl+C, which no screen can send, so
// for twelve seconds there is nowhere for a flag to be read. What a screen change
// can do is make the result irrelevant — the task compares the generation it
// started with against the current one AFTER the call returns and before it
// writes anything, so a scan outliving its screen throws its answer away instead
// of dropping it into a table the next screen is already using.
//
// The task is never killed for the same reason it cannot be cancelled: it is
// inside the radio driver holding the shared WiFi/Bluetooth lock, and killing it
// there would leave that lock held for good.
//
// Plain volatile is enough for the handshake. These cores have no data cache over
// SRAM, so a write on one is visible to the other; the ordering that matters is
// that g_ble_busy clears LAST, after g_ready is set.
static char              g_ble_out[1400];    // the capture buffer, file-static
static char              g_ble_line[32];     // the command, built before the spawn
static volatile uint32_t g_gen;
static volatile uint32_t g_req_gen;
static volatile uint8_t  g_ble_busy;
static volatile uint8_t  g_ready;
static volatile int      g_ble_rc;
static uint32_t          g_start_ms;     // when the running scan was asked for
static uint32_t          g_last_end_ms;  // when the last scan finished

// What the last scan amounted to, for the screens to show.
enum ScanState { SC_NEVER = 0, SC_OK, SC_EMPTY, SC_REFUSED };
static uint8_t g_state;

static int scan_task(void *arg) {
    (void)arg;
    uint32_t mine = g_req_gen;
    int rc = fw_shell_run(g_ble_line, g_ble_out, sizeof(g_ble_out));
    if (mine == g_gen && !fw_task_should_stop()) {
        g_ble_rc = rc;
        g_ready = 1;
    } else {
        g_ble_out[0] = 0;               // nobody is waiting for this any more
    }
    g_ble_busy = 0;
    return 0;
}

// Ask for a scan. False when one is already running, which is not an error — the
// caller tries again on a later tick.
static bool scan_start(bool classic, unsigned secs) {
    if (g_ble_busy || g_ready) return false;
    snprintf(g_ble_line, sizeof(g_ble_line), "bt scan %s %u", classic ? "classic" : "le", secs);
    g_ble_out[0] = 0;
    g_req_gen = g_gen;
    g_start_ms = fw_millis();
    g_ble_busy = 1;
    // TASK_STACK_MIN's worth is ample for what the PACKAGE does here — call one
    // ABI function and set a flag. The shell, `bt` and btstack all run on this
    // stack too, but the loader already adds a separate firmware reserve on top
    // of whatever a package asks for, and that reserve is what carries them.
    if (fw_task_spawn("novabt", scan_task, nullptr, 2048) < 0) {
        g_ble_busy = 0;
        g_state = SC_REFUSED;
        return false;
    }
    return true;
}

// Fire an arbitrary shell line on the SAME worker the scans use — one radio, one
// output capture, one task at a time. False when one is already in flight, which
// the caller retries on a later tick or ignores. Unlike a scan it sets no
// ScanState: an advertise is not a scan and must never be folded into the table.
static bool cmd_start(const char *line) {
    if (g_ble_busy || g_ready) return false;
    snprintf(g_ble_line, sizeof(g_ble_line), "%s", line);
    g_ble_out[0] = 0;
    g_req_gen = g_gen;
    g_start_ms = fw_millis();
    g_ble_busy = 1;
    if (fw_task_spawn("novabt", scan_task, nullptr, 2048) < 0) {
        g_ble_busy = 0;
        return false;
    }
    return true;
}

// The generation moves whenever the top screen changes, so anything in flight is
// disowned. Both halves matter: enter() as well as leave(), because a screen
// pushed on top of a scanning one becomes the owner of the table.
static void scan_disown(void) { g_gen++; g_ready = 0; }

// --- parsing what `bt` printed --------------------------------------------------
//
// Written against print_seen() in os/shell/bt.cpp and checked against a real run
// on a Pico 2 W. A row is two spaces, the address, two spaces, "LE" or "BR"
// padded to four, then either an RSSI right-aligned in four columns followed by
// " dBm" and two spaces, or ten spaces where that would have been:
//
//     "  D9:33:36:32:1B:59  LE    -78 dBm  Govee_H6199_1B59"
//     "  37:96:7B:89:C2:EB  LE    -80 dBm  "
//
// Interleaved with those are the command's own tagged lines — "[:] Scanning for
// 5 seconds...", "[:] 24 devices", and on a bad day "[!] ..." — which fw_shell_run
// captures too, with the colour stripped. Rather than skipping those by their
// tag, a line is a device only if it carries a well-formed address, which also
// rejects the "  Devices only appear while..." note that follows an empty scan
// and is indented exactly like a row.

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Seventeen characters of "XX:XX:XX:XX:XX:XX" and nothing else.
static bool looks_like_mac(const char *p) {
    for (int i = 0; i < 17; i++) {
        if ((i % 3) == 2) { if (p[i] != ':') return false; }
        else if (!is_hex(p[i])) return false;
    }
    return true;
}

static bool mac_eq(const char *a, const char *b) { return nova::ieq(a, b); }

// --- the tagged devices ----------------------------------------------------------
//
// Stored in v1's key and v1's format, "mac|label,mac|label", so a device named
// under the MicroPython suite is still watched here. That format is why
// novacore's csv_has cannot be used: it compares whole comma-separated fields, so
// it would look for an entry named exactly the address and never match one. The
// silent version of that bug is a Presence screen that is permanently empty.
//
// A registry value holds 96 characters, so this is about four devices. That is a
// real limit and Presence says so rather than dropping the fifth quietly.
#define KEY_KNOWN NOVA_KEY_PREFIX "Known"

// Walk the pairs. `at` is the index wanted, or -1 to search by address instead.
// Returns true when something was found.
static bool known_walk(int at, const char *want_mac,
                       char *mac_out, unsigned mcap, char *label_out, unsigned lcap,
                       int *count_out) {
    char buf[NOVA_VAL_MAX];
    nova::copy(buf, sizeof(buf), nova::reg(KEY_KNOWN, ""));

    int n = 0;
    bool hit = false;
    char *p = buf;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        char *end = p;
        while (*end && *end != ',') end++;
        char saved = *end;
        *end = 0;

        char *bar = strchr(p, '|');
        const char *label = p;
        if (bar) { *bar = 0; label = bar + 1; }
        if (*p) {
            bool match = (at >= 0) ? (n == at) : (want_mac && mac_eq(p, want_mac));
            if (match && !hit) {
                if (mac_out)   nova::copy(mac_out, mcap, p);
                // A tagged device that was never given a name is shown by its
                // address rather than as a blank row.
                if (label_out) nova::copy(label_out, lcap, *label ? label : p);
                hit = true;
            }
            n++;
        }
        *end = saved;
        p = *end ? end + 1 : end;
    }
    if (count_out) *count_out = n;
    return hit;
}

static int  known_count(void) { int n = 0; known_walk(-2, nullptr, nullptr, 0, nullptr, 0, &n); return n; }
static bool known_at(int i, char *mac, unsigned mcap, char *label, unsigned lcap) {
    return known_walk(i, nullptr, mac, mcap, label, lcap, nullptr);
}
static bool known_label(const char *mac, char *label, unsigned lcap) {
    return known_walk(-1, mac, nullptr, 0, label, lcap, nullptr);
}
static bool is_known(const char *mac) { return known_walk(-1, mac, nullptr, 0, nullptr, 0, nullptr); }

// Add, rename or remove. Rebuilt rather than spliced, for the reason csv_remove
// gives: splicing has to get the separator right at four edges and rebuilding has
// none of those cases.
static void known_set(const char *mac, const char *label) {
    char out[NOVA_VAL_MAX];
    out[0] = 0;
    unsigned at = 0;

    char cur_mac[18], cur_label[20];
    int n = known_count();
    for (int i = 0; i < n; i++) {
        if (!known_at(i, cur_mac, sizeof(cur_mac), cur_label, sizeof(cur_label))) continue;
        if (mac_eq(cur_mac, mac)) continue;          // replaced or removed below
        int w = snprintf(out + at, sizeof(out) - at, "%s%s|%s",
                         at ? "," : "", cur_mac, cur_label);
        if (w < 0 || at + (unsigned)w >= sizeof(out)) { out[at] = 0; break; }
        at += (unsigned)w;
    }
    if (label && *label) {
        char clean[16];
        // Neither separator may appear in a label or the next read splits in the
        // wrong place. v1 replaced them with spaces and so does this.
        nova::copy(clean, sizeof(clean), label);
        for (char *q = clean; *q; q++) if (*q == ',' || *q == '|') *q = ' ';
        int w = snprintf(out + at, sizeof(out) - at, "%s%s|%s", at ? "," : "", mac, clean);
        if (w > 0 && at + (unsigned)w < sizeof(out)) at += (unsigned)w;
    }
    out[at] = 0;
    nova::reg_set(KEY_KNOWN, out);
    nova::reg_save();
}

// --- folding a scan into the table ------------------------------------------------

static Dev *dev_find(const char *mac) {
    for (int i = 0; i < g_ndev; i++)
        if (mac_eq(g_dev[i].mac, mac)) return &g_dev[i];
    return nullptr;
}

// Room for one more, evicting if it has to. The stalest goes first and a TAGGED
// device is never dropped — the whole point of naming one is that it matters.
static Dev *dev_room(void) {
    if (g_ndev < MAX_DEV) return &g_dev[g_ndev++];
    int worst = -1;
    for (int i = 0; i < MAX_DEV; i++) {
        if (is_known(g_dev[i].mac)) continue;
        if (worst < 0 || g_dev[i].last_ms < g_dev[worst].last_ms) worst = i;
    }
    return worst < 0 ? nullptr : &g_dev[worst];
}

static bool notify_on(void) { return nova::reg_is(NOVA_KEY_PREFIX "Watch_Notify", "on", true); }
static bool notify_new(void) { return nova::reg_is(NOVA_KEY_PREFIX "Watch_New", "on", false); }

// Say something happened, if anybody asked to be told.
//
// A NAMED device is always worth reporting; an unnamed one only when the person
// opted in, because in a room with two dozen advertisers that alert fires
// constantly and drowns the one that mattered. Both halves are v1's rule.
static void announce(const char *mac, const char *what, bool tagged_only) {
    if (!notify_on()) return;
    char label[20];
    if (!known_label(mac, label, sizeof(label))) {
        if (tagged_only) return;
        nova::copy(label, sizeof(label), mac);
    }
    char msg[nova::notify::TEXT_MAX];
    snprintf(msg, sizeof(msg), "%s %s", label, what);
    nova::notify::post(msg);
}

// A device seen for the first time. One that was named in an earlier session has
// ARRIVED, which is the whole point of having named it; anything else is merely
// new, and that is the noisy one Watch_New guards.
static void announce_first(const char *mac) {
    if (is_known(mac)) announce(mac, "arrived", true);
    else if (notify_new()) announce(mac, "seen", false);
}

// One row, already known to start with an address. Returns false if it does not.
static bool merge_row(const char *p, uint32_t now) {
    while (*p == ' ') p++;
    if (!looks_like_mac(p)) return false;

    char mac[18];
    memcpy(mac, p, 17);
    mac[17] = 0;
    p += 17;

    while (*p == ' ') p++;
    bool classic = (p[0] == 'B' && p[1] == 'R');
    while (*p && *p != ' ') p++;                  // past LE / BR
    while (*p == ' ') p++;

    // The RSSI, when there is one. A device with no reading prints spaces where
    // the number would be, so the test is not "is there a number here" — a name
    // could start with a digit — but "is there a number followed by dBm".
    int rssi = 0;
    const char *after = p;
    {
        const char *q = p;
        int sign = 1;
        if (*q == '-') { sign = -1; q++; }
        int v = 0, digits = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; digits++; }
        if (digits) {
            const char *r = q;
            while (*r == ' ') r++;
            if (r[0] == 'd' && r[1] == 'B' && r[2] == 'm') {
                rssi = sign * v;
                after = r + 3;
            }
        }
    }
    while (*after == ' ') after++;

    // Everything left is the name, which may contain spaces ("Quest 3S") and may
    // already have been truncated with ".." by the firmware. It is passed through
    // as it came rather than tidied, and its trailing padding is cut: `bt` leaves
    // it on every row that has no name at all.
    char name[20];
    nova::copy(name, sizeof(name), after);
    for (int i = (int)strlen(name) - 1; i >= 0 && (name[i] == ' ' || name[i] == '\r'); i--)
        name[i] = 0;

    Dev *d = dev_find(mac);
    if (!d) {
        d = dev_room();
        if (!d) return true;                      // full of tagged devices
        memset(d, 0, sizeof(*d));
        nova::copy(d->mac, sizeof(d->mac), mac);
        d->first_ms = now;
        // The locally administered bit, which is v1's test and costs nothing
        // because it is part of the address already in hand.
        //
        // It is a heuristic and it UNDER-REPORTS. The authoritative answer is the
        // BLE address TYPE, which the shell does not print; failing that, a
        // random static address is marked by the top two bits of this octet
        // rather than by this one. D9:33:36:32:1B:59 from a real scan is such an
        // address and this test calls it public. Under-reporting is the safe
        // direction and the reason it is kept as it stands: a device is never
        // wrongly said to be anonymous, it is only sometimes not noticed to be.
        int hi = 0;
        for (int i = 0; i < 2; i++) {
            char c = mac[i];
            hi = hi * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        d->random = (hi & 0x02) ? 1 : 0;
        announce_first(mac);
    } else if (d->gone) {
        // v1 set `gone` and never cleared it, so a device that left and came back
        // read as away for the rest of the session and its arrival was never
        // announced a second time. Cleared here, which is what makes "comes and
        // goes" mean more than once.
        d->gone = 0;
        announce(mac, "arrived", true);
    }

    if (*name && !d->name[0]) nova::copy(d->name, sizeof(d->name), name);
    d->rssi    = (int16_t)rssi;
    d->classic = classic ? 1 : 0;
    d->last_ms = now;
    d->misses  = 0;
    if (d->count < 0xffff) d->count++;
    return true;
}

// Anything not heard from recently has left. Time-based rather than counting
// missed passes, because the two scan kinds see different devices: a classic
// inquiry finds no LE device at all, and counting passes would report every
// phone in the room as departed the moment somebody changed the filter.
static void sweep(uint32_t now) {
    for (int i = 0; i < g_ndev; i++) {
        Dev &d = g_dev[i];
        if (now - d.last_ms < GONE_MS) continue;
        if (d.misses < 255) d.misses++;
        if (d.misses == HYST && !d.gone) {
            d.gone = 1;
            announce(d.mac, "left", true);
        }
    }
}

// Read the finished scan. Returns true when the table changed.
static bool scan_collect(void) {
    if (!g_ready) return false;
    g_ready = 0;
    g_last_end_ms = fw_millis();

    // An empty buffer is NOT an empty room. There is one output capture in the
    // OS, and when something else holds it the command still runs and the buffer
    // comes back untouched — which would otherwise read as "nothing answered"
    // and be believed.
    if (!g_ble_out[0]) { g_state = SC_EMPTY; return true; }
    if (g_ble_rc != 0) { g_state = SC_REFUSED; return true; }

    uint32_t now = fw_millis();
    const char *p = g_ble_out;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (nl && (unsigned)(nl - p) < 120) {
            char line[120];
            unsigned n = (unsigned)(nl - p);
            memcpy(line, p, n);
            line[n] = 0;
            merge_row(line, now);
        } else if (!nl) {
            merge_row(p, now);
        }
        if (!nl) break;
        p = nl + 1;
    }
    sweep(now);
    g_state = SC_OK;
    return true;
}

// How long to rest between scans, in milliseconds.
//
// v1 scanned for 3 s inside an 8 s cycle. That number cannot be carried over: a
// call here costs about TWELVE seconds of wall clock for a five-second scan, so
// an eight-second period would mean the next scan starts before the last has
// returned and the radio is simply never off. The rest is measured against that
// twelve, and at the default the radio is idle for most of each cycle.
static uint32_t rest_ms(void) {
    int v = nova::reg_int(NOVA_KEY_PREFIX "Watch_Period", 20000);
    if (v < 8000) v = 8000;
    return (uint32_t)v;
}

// Drive one repeated-scan screen. `secs` is the scan length, `rest` the gap after
// the last one finished; a rest of 0 means back to back.
static bool scan_pump(unsigned secs, uint32_t rest, bool classic) {
    bool changed = scan_collect();
    if (!g_ble_busy && !g_ready) {
        uint32_t now = fw_millis();
        if (g_state == SC_NEVER || now - g_last_end_ms >= rest)
            scan_start(classic, secs);
    }
    return changed;
}

// --- shared drawing --------------------------------------------------------------

// Rows a list gets when it also carries a status footer. Derived the same way
// ui::rows_for is, one row shorter.
static int list_rows(const Canvas &c) { return (c.height() - ui::TOP - ui::FH) / ui::ROWH; }

// The label a device gets in a list. Name if it advertised one, else the first
// eight characters of the address.
//
// v1 fell back name -> vendor -> OUI prefix -> address, and the two middle steps
// are gone with the advertisement payload. What is left of that idea is that the
// first eight characters ARE the OUI, so an unnamed device is still shown by the
// part of its address that identifies who made it.
static void dev_label(const Dev &d, char *out, unsigned cap) {
    if (d.name[0]) { nova::copy(out, cap, d.name); return; }
    if (d.random) { nova::copy(out, cap, "(random)"); return; }
    nova::copy(out, cap, d.mac);
    if (cap > 9) out[8] = 0;
}

// 'now' / '3m' / '2h', in four characters. Clamped at both ends: fw_millis wraps
// at about seven weeks, and a span measured either side of the wrap would print
// something like 495992h next to a device, which is worse than not knowing.
static void age_str(uint32_t ms, char *out, unsigned cap) {
    if (ms > 0x7fffffffu) { nova::copy(out, cap, "now"); return; }
    uint32_t s = ms / 1000;
    if (s < 45)   { nova::copy(out, cap, "now"); return; }
    if (s < 3600) { snprintf(out, cap, "%um", (unsigned)(s / 60)); return; }
    uint32_t h = s / 3600;
    if (h < 100) snprintf(out, cap, "%uh", (unsigned)h);
    else         nova::copy(out, cap, "99h+");
}

// The status line every one of these screens carries. It is a live reading of
// what the radio is doing, not a control hint — the hints live in help(), which
// is what gave these lists their sixth row back.
static void scan_status(char *out, unsigned cap, unsigned secs, bool repeating) {
    if (g_ble_busy) {
        // Counted from when THIS scan was asked for. A five-second scan takes
        // about twelve seconds to come back, so a count that stops at five would
        // look like a hang for the other seven.
        snprintf(out, cap, "scanning %us", (unsigned)((fw_millis() - g_start_ms) / 1000));
        return;
    }
    switch (g_state) {
        case SC_REFUSED:
            // `bt` is an admin command and fw_shell_run runs as whoever is signed
            // in, so a guest session gets a refusal rather than a scan. Reported
            // as refused rather than as an empty room, which is what it would
            // otherwise look like.
            nova::copy(out, cap, "scan refused");
            return;
        case SC_EMPTY:
            // The command ran and the buffer came back untouched, which means
            // something else held the one output capture.
            nova::copy(out, cap, "no result - busy");
            return;
        case SC_NEVER:
            snprintf(out, cap, "%us scan", secs);
            return;
        default: {
            const uint32_t since = fw_millis() - g_last_end_ms;
            const uint32_t r = rest_ms();
            // Only a screen that actually rescans may promise a next one.
            if (repeating && since < r)
                snprintf(out, cap, "%d dev  next %us", g_ndev, (unsigned)((r - since) / 1000));
            else
                snprintf(out, cap, "%d dev", g_ndev);
            return;
        }
    }
}

// --- the ordered index -------------------------------------------------------------
//
// Filtering and sorting produce an index rather than moving the table, so the
// records keep their identity while a screen is looking at one of them. Rebuilt
// when the table or the filter changes, never per frame: it is an insertion sort
// and draw runs sixty times a second when something is animating.

enum Filter { F_ALL = 0, F_LE, F_BR, F_KNOWN, F_COUNT };
static const char *const kFilterName[F_COUNT] = { "all", "LE", "BR", "known" };

static uint8_t g_idx[MAX_DEV];
static int     g_nidx;

static void index_build(int filter) {
    g_nidx = 0;
    for (int i = 0; i < g_ndev; i++) {
        const Dev &d = g_dev[i];
        if (filter == F_LE    && d.classic) continue;
        if (filter == F_BR    && !d.classic) continue;
        if (filter == F_KNOWN && !is_known(d.mac)) continue;
        g_idx[g_nidx++] = (uint8_t)i;
    }
    // Strongest first. A device with no reading sorts to the bottom rather than
    // to the top, which is where a signed 0 would otherwise put it.
    for (int i = 1; i < g_nidx; i++) {
        uint8_t v = g_idx[i];
        int key = g_dev[v].rssi ? g_dev[v].rssi : -121;
        int j = i - 1;
        while (j >= 0) {
            int other = g_dev[g_idx[j]].rssi ? g_dev[g_idx[j]].rssi : -121;
            if (other >= key) break;
            g_idx[j + 1] = g_idx[j];
            j--;
        }
        g_idx[j + 1] = v;
    }
}

// One device row: an optional mark, the label, and the reading right-aligned.
static void draw_dev_row(Canvas &c, const Dev &d, int y, bool sel, bool marked) {
    const int right = c.width() - (ui::SB_W + 1);
    if (sel) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
    const int tc = sel ? 0 : 1;

    if (marked) c.text(2, y, "*", tc);

    char db[8];
    if (d.rssi) snprintf(db, sizeof(db), "%d", (int)d.rssi);
    else        nova::copy(db, sizeof(db), "--");
    const int dbw = c.text_width(db, 1, false);
    c.text(right - dbw - 2, y, db, tc);

    char label[24];
    dev_label(d, label, sizeof(label));
    c.text_fit(2 + ui::ADV, y, label, tc, right - dbw - 6 - ui::ADV, false);
}

// --- Locate --------------------------------------------------------------------
//
// Walk around with this. A single antenna cannot give a bearing — only a
// magnitude that rises as it closes in — so it reports level and trend and lets
// the person do the triangulating with their feet, which is how every practical
// single-antenna locator works. It deliberately does not draw an arrow, because
// it does not know one.
//
// v1's estimated distance in metres is NOT ported. It needs the TX power the
// device advertised, which only reaches a caller that can see the advertisement,
// and the log-distance model behind it wants pow() and log10() from a library
// this package does not link. The best reading so far takes its place on the
// screen, which is the number that is actually useful while hunting.
//
// DEVICE-UNCONFIRMED: the meter has never been walked with. Confirming warmer and
// colder needs a second Bluetooth device and somebody to carry one away from the
// other, and the level is only resampled once per scan — roughly every fifteen
// seconds even with no rest — so how it feels in the hand is genuinely unknown.
class LocateScreen : public Screen {
public:
    const char *title(void) const override { return "Locate"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Walk. Warmer means closer.";
        out[1] = "One reading per scan, so it";
        out[2] = "updates slowly, not smoothly.";
        return 3;
    }

    void enter(void) override {
        scan_disown();
        level_ = 0;
        have_ = false;
        trend_ = 0;
        best_ = 0;
        samples_ = 0;
        seen_count_ = 0;
        phase_ = 0;
    }

    void leave(void) override { scan_disown(); }

    bool animating(void) const override { return true; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        // No rest at all. Every other screen here spaces its scans out to keep
        // the radio off, and this is the one where that trade goes the other way:
        // hunting for a device IS the activity, it lasts as long as somebody is
        // standing there doing it, and it stops when the screen closes.
        bool changed = scan_pump(3, 0, false);
        if (changed) feed();
        return true;              // the spinner and the elapsed count both move
    }

    void draw(Canvas &c) override {
        const Dev *d = dev_find(g_focus);

        char label[24];
        if (d) dev_label(*d, label, sizeof(label));
        else   nova::copy(label, sizeof(label), g_focus);
        c.text_fit(2, ui::TOP, label, 1, c.width() - 14, false);
        if (g_ble_busy) c.spinner(c.width() - 9, ui::TOP, phase_ / BLE_SPIN_MS, 1);

        // The meter. A proportion reads faster as a length than as a number, and
        // it is the only thing on this screen that is a picture.
        const int y = ui::TOP + ui::ROWH + 2;
        const int bw = c.width() - 8;
        c.rect(4, y, bw, 11, 1);
        const int n = bars(20);
        if (n > 0) c.fill_rect(6, y + 2, (bw - 4) * n / 20, 7, 1);

        char line[28];
        int ty = y + 14;
        if (!have_) {
            c.text(4, ty, "listening...", 1);
        } else {
            snprintf(line, sizeof(line), "%d dBm  %s", level_ / 10, hint());
            c.text_fit(4, ty, line, 1, c.width() - 6, false);
            ty += ui::ROWH;
            snprintf(line, sizeof(line), "best %d dBm", (int)best_);
            c.text(4, ty, line, 1);
        }

        scan_status(line, sizeof(line), 3, true);
        c.text_fit(2, c.height() - ui::FH, line, 1, c.width() - 4, false);
    }

private:
    // Tenths of a dBm. v1 held a float and the arithmetic is the same weighting;
    // integers avoid a floating-point call in a package that has no reason to
    // pull one in.
    int32_t  level_;
    int32_t  trend_;
    int16_t  best_;
    uint16_t samples_;
    uint16_t seen_count_;
    unsigned phase_;
    bool     have_;

    void feed(void) {
        const Dev *d = dev_find(g_focus);
        if (!d || !d->rssi) return;
        if (d->count == seen_count_) return;        // no new sighting this pass
        seen_count_ = d->count;
        const int32_t r = (int32_t)d->rssi * 10;
        samples_++;
        if (!have_) { level_ = r; have_ = true; }
        else {
            // EWMA at v1's weighting: raw RSSI jitters several dB between
            // packets, and an unsmoothed readout cannot be walked with.
            const int32_t prev = level_;
            level_ = (level_ * 7 + r * 3) / 10;
            trend_ = level_ - prev;
        }
        if (!best_ || d->rssi > best_) best_ = d->rssi;
    }

    // 0..n. -30 dBm is on top of it, -95 is the far edge — v1's window.
    int bars(int n) const {
        if (!have_) return 0;
        int32_t frac = (level_ + 950) * n;
        if (frac < 0) return 0;
        frac /= 650;
        return frac > n ? n : (int)frac;
    }

    const char *hint(void) const {
        if (!have_) return "listening";
        if (samples_ > 2) {
            if (trend_ > 12)  return "warmer";
            if (trend_ < -12) return "colder";
        }
        if (level_ > -450) return "very close";
        if (level_ > -600) return "close";
        if (level_ > -750) return "nearby";
        return "far";
    }
};

// --- Device --------------------------------------------------------------------
//
// One device in full, plus the two things worth doing with it.

static void name_done(void *ctx, const char *text) {
    (void)ctx;
    // The keyboard's buffer does not outlive this call, and known_set copies.
    known_set(g_focus, (text && *text) ? text : g_focus);
}

class BleDeviceScreen : public Screen {
public:
    const char *title(void) const override { return "Device"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Naming a device puts it on";
        out[1] = "the Presence screen.";
        return 2;
    }

    // The cursor stays. Naming a device comes back through here, and the row
    // somebody just used is the one they are most likely to want next — the
    // reset moved them off it.
    void enter(void) override { scan_disown(); }
    void leave(void) override { scan_disown(); }

    void draw(Canvas &c) override {
        const Dev *d = dev_find(g_focus);
        int y = ui::TOP;
        char line[32];

        if (!d) {
            c.text(2, y, "Not in the table.", 1);
            return;
        }

        char label[24];
        if (d->name[0]) nova::copy(label, sizeof(label), d->name);
        else            nova::copy(label, sizeof(label), "(no name)");
        c.text_fit(2, y, label, 1, c.width() - 4, false);
        y += ui::ROWH;

        // The full address, which is the one field the list rows cannot show:
        // seventeen characters leaves nothing beside it on a 21-column panel.
        c.text(2, y, d->mac, 1);
        y += ui::ROWH;

        char age[8];
        age_str(fw_millis() - d->last_ms, age, sizeof(age));
        if (d->rssi) snprintf(line, sizeof(line), "%s %d dBm  seen %s",
                              d->classic ? "BR" : "LE", (int)d->rssi, age);
        else         snprintf(line, sizeof(line), "%s  seen %s",
                              d->classic ? "BR" : "LE", age);
        c.text_fit(2, y, line, 1, c.width() - 4, false);
        y += ui::ROWH;

        char held[8];
        age_str(fw_millis() - d->first_ms, held, sizeof(held));
        snprintf(line, sizeof(line), "here %s  x%u%s", held, (unsigned)d->count,
                 d->random ? "  random" : "");
        c.text_fit(2, y, line, 1, c.width() - 4, false);
        y += ui::ROWH;

        // The actions, as buttons rather than list rows: there are two of them,
        // they are not a list, and drawing them as one would say they were.
        int x = 2;
        for (int i = 0; i < 2; i++) {
            const char *lab = action_label(i);
            const int w = c.text_width(lab, 1, true) + 6;
            if (i == sel_) c.rounded_rect(x, y - 1, w, ui::ROWH, 1, true);
            c.text(x + 3, y, lab, i == sel_ ? 0 : 1, 1, true);
            x += w + 3;
        }
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { sel_ ^= 1; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ ^= 1; return ui::ACT_STAY; }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            if (sel_ == 0) { gui::push<LocateScreen>(); return ui::ACT_STAY; }
            if (is_known(g_focus)) known_set(g_focus, nullptr);
            else {
                const Dev *d = dev_find(g_focus);
                ui::keyboard("Name", d && d->name[0] ? d->name : "", false,
                             name_done, nullptr, nullptr);
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int sel_;

    static const char *action_label(int i) {
        if (i == 0) return "Locate";
        return is_known(g_focus) ? "Forget" : "Name it";
    }
};

// Shared by the two list screens: open the device under the cursor.
static void open_device(const Dev &d) {
    nova::copy(g_focus, sizeof(g_focus), d.mac);
    gui::push<BleDeviceScreen>();
}

// --- Ping ----------------------------------------------------------------------
//
// The capability v1 had and this suite was long said to have lost: transmit a
// crafted advertisement that makes a nearby phone raise a pairing card. It is
// back, because `bt advertise` is. The firmware builds the sourced Apple
// Continuity / Google Fast Pair payload, sends it from a random address and
// stops it on a deadline; this screen just asks for it and shows the countdown.
//
// It follows the exact async shape the scans use — the shell call runs on the
// worker, never on tick() — but it is not a scan and stores nothing in the
// table, so it does its own tiny result handling rather than scan_collect's.
//
// Bounded three ways over: the firmware stops at its own deadline whatever the
// UI does, leaving stops it early, and this screen only ever asks for a short
// burst. So a Ping screen left open does NOT sit transmitting; each burst is a
// deliberate press.
//
// DEVICE-UNCONFIRMED: no card has been raised on a real handset from here. The
// payload is host-tested (btadv_test), the screen is driven on the host, but the
// radio and the phone are not. To confirm: open Ping, pick iOS with an iPhone in
// range, press SELECT, watch for the AirPods card; then Android with a Pixel.
class PingScreen : public Screen {
public:
    const char *title(void) const override { return "Ping"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Turn to pick iPhone / Android.";
        out[1] = "SELECT sends a pairing card.";
        out[2] = "BACK stops it.";
        return 3;
    }

    void enter(void) override {
        scan_disown();
        android_  = false;
        phase_    = 0;
        adv_until_ = 0;
        sent_     = false;
        note_[0]  = 0;
    }

    // Best-effort stop on the way out. The firmware's own deadline stops it
    // regardless, so a refused send here (worker still busy) is not a leak.
    void leave(void) override {
        cmd_start("bt advertise stop");
        adv_until_ = 0;
        scan_disown();
    }

    bool animating(void) const override { return advertising() || g_ble_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        // Reap the worker's reply just enough to tell started from refused. The
        // fw text is "[@] Advertising ..." on success; a guest session or a
        // held capture gives a tagged error or an empty buffer instead.
        if (g_ready) {
            g_ready = 0;
            if (!g_ble_out[0])                       nova::copy(note_, sizeof(note_), "no result - busy");
            else if (g_ble_rc != 0)                  nova::copy(note_, sizeof(note_), "refused");
            else if (strstr(g_ble_out, "Advertising")) note_[0] = 0;   // it started
            else                                     nova::copy(note_, sizeof(note_), "refused");
        }
        return advertising() || g_ble_busy;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;

        // The two targets as buttons, the selected one filled — the same button
        // row the Device screen draws its actions with.
        static const char *const kOpt[2] = { "iPhone", "Android" };
        int x = 2;
        for (int i = 0; i < 2; i++) {
            const bool on = ((android_ ? 1 : 0) == i);
            const int w = c.text_width(kOpt[i], 1, true) + 6;
            if (on) c.rounded_rect(x, y - 1, w, ui::ROWH, 1, true);
            c.text(x + 3, y, kOpt[i], on ? 0 : 1, 1, true);
            x += w + 3;
        }
        if (g_ble_busy) c.spinner(c.width() - 9, ui::TOP, phase_ / BLE_SPIN_MS, 1);
        y += ui::ROWH + 2;

        char line[28];
        if (advertising()) {
            const uint32_t left = (adv_until_ - fw_millis()) / 1000 + 1;
            snprintf(line, sizeof(line), "Advertising %us", (unsigned)left);
            c.text(2, y, line, 1);
        } else {
            c.text(2, y, sent_ ? "Sent." : "SELECT to ping.", 1);
        }
        y += ui::ROWH;
        if (note_[0]) c.text(2, y, note_, 1);

        // The footer says what this is and, while live, how to stop it.
        const char *foot = advertising() ? "BACK stops it"
                                         : "proximity pairing beacon";
        c.text_fit(2, c.height() - ui::FH, foot, 1, c.width() - 4, false);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW || e == EV_ROT_CCW) {
            if (!advertising()) android_ = !android_;   // no switching mid-burst
            return ui::ACT_STAY;
        }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) { fire(); return ui::ACT_STAY; }
        // BACK falls through to the base, which pops; leave() sends the stop.
        return Screen::on_event(e);
    }

private:
    static constexpr unsigned PING_SECS = 10;

    bool     android_;
    unsigned phase_;
    uint32_t adv_until_;      // fw_millis deadline of the burst this screen asked for
    bool     sent_;
    char     note_[18];

    bool advertising(void) const { return adv_until_ && fw_millis() < adv_until_; }

    void fire(void) {
        char line[32];
        snprintf(line, sizeof(line), "bt advertise ping %s %u",
                 android_ ? "android" : "ios", PING_SECS);
        if (cmd_start(line)) {
            adv_until_ = fw_millis() + PING_SECS * 1000;
            sent_ = true;
            note_[0] = 0;
            return;
        }
        // cmd_start refuses while the shared BLE worker is out, and it used to
        // refuse in silence: note_ was left alone, so the panel went on reading
        // "SELECT to ping." after a press that did nothing at all. Every other
        // screen on this worker says "busy" — this one is the exception, and it
        // was the exception by omission rather than on purpose.
        nova::copy(note_, sizeof(note_), "busy");
    }
};

// --- BLE -----------------------------------------------------------------------
//
// What is around, right now. The MicroPython BLE app was a menu of three — scan,
// ping an iPhone, ping an Android. The scan is the body of this screen and starts
// on open, which is what somebody opening it wanted; the two pings, which need a
// way to advertise, live on the Ping screen now that `bt advertise` exists. A
// pinned first row reaches it — the same synthetic-row trick Radar uses for its
// settings — so the capability is one press from where it always was.
class BleScreen : public Screen {
public:
    const char *title(void) const override { return "BLE"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Top row pings a phone.";
        out[1] = "SELECT opens a device.";
        out[2] = "Hold SELECT to scan again.";
        return 3;
    }

    void enter(void) override {
        // THE CURSOR IS NOT RESET. enter() runs again every time a screen
        // above this one pops, and going back to the top of the room after
        // looking at one device is the thing that makes a list feel like it
        // forgot you. draw() clamps it against the count, which is the part
        // that has to be right — the table changes under this screen on
        // every scan whether anybody left it or not.
        scan_disown();
        phase_ = 0;
        kicked_ = false;
        index_build(F_ALL);
    }

    void leave(void) override { scan_disown(); }

    bool animating(void) const override { return g_ble_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        // One scan on arrival and then nothing until asked. This screen answers
        // "what is here" once; Radar is the one that keeps looking, and doing it
        // here as well would hold the radio on two screens instead of one.
        //
        // Kicked off from a per-screen flag rather than from the shared scan
        // state, because that state outlives the screen: keying off it meant the
        // app scanned the first time it was ever opened and showed a stale table
        // every time after.
        const bool changed = scan_collect();
        if (!kicked_ && !g_ble_busy) kicked_ = scan_start(false, 5);
        if (changed) index_build(F_ALL);
        return changed || g_ble_busy;
    }

    void draw(Canvas &c) override {
        const int rows = list_rows(c);
        char line[32];

        // Row zero is the Ping entry, pinned like Radar's settings row, so the
        // list is never empty and the transmit capability is always in reach.
        const int total = g_nidx + 1;
        if (sel_ >= total) sel_ = total - 1;
        if (sel_ < 0) sel_ = 0;
        if (sel_ < top_) top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

        const int right = c.width() - (ui::SB_W + 1);
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= total) break;
            const int y = ui::TOP + i * ui::ROWH;
            if (idx == 0) {
                if (sel_ == 0) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
                c.text_fit(2, y, "Ping a phone", sel_ == 0 ? 0 : 1, right - 12, false);
                c.text(right - ui::ADV - 2, y, ">", sel_ == 0 ? 0 : 1);
                continue;
            }
            const Dev &d = g_dev[g_idx[idx - 1]];
            draw_dev_row(c, d, y, idx == sel_, is_known(d.mac));
        }
        if (total > 1)
            c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP,
                        c.height() - ui::TOP - ui::FH, top_, rows, total);

        // An empty room still says so, on the row under the ping entry, rather
        // than taking the whole panel and crowding out the one press that works.
        if (g_nidx == 0 && !g_ble_busy && g_state == SC_OK)
            c.text(2, ui::TOP + ui::ROWH, "Nothing else answered.", 1);

        // This screen scans once and stops, so it never promises a next one.
        scan_status(line, sizeof(line), 5, false);
        c.text_fit(2, c.height() - ui::FH, line, 1, c.width() - 4, false);
        if (g_ble_busy) c.spinner(c.width() - 9, c.height() - ui::FH, phase_ / BLE_SPIN_MS, 1);
    }

    Action on_event(Event e) override {
        const int total = g_nidx + 1;
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % total; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + total - 1) % total; return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            if (sel_ == 0) gui::push<PingScreen>();
            else if (sel_ - 1 < g_nidx) open_device(g_dev[g_idx[sel_ - 1]]);
            return ui::ACT_STAY;
        }
        if (e == EV_SELECT_HOLD) { scan_start(false, 5); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    int      sel_, top_;
    unsigned phase_;
    bool     kicked_;
};

// --- Radar settings ---------------------------------------------------------------
//
// Reached from the Radar screen itself rather than from the global Settings menu.
// These only mean anything while somebody is looking at Radar, and a setting is
// easiest to find next to the thing it changes — which is where v1 put them.
//
// The keys are v1's, so a device configured under the MicroPython suite keeps its
// choices. What is NOT here is v1's "Observer" switch: there is no background
// observer to switch on, because scanning now happens only while one of these
// screens is open.
class RadarSettings : public Screen {
public:
    // NOT "Radar", which is what the screen underneath is called. The status
    // bar is the only thing that says where you are, and a sub-screen that
    // repeats its parent's name reads as a press that did not take — the same
    // mismatch the Versions screen had when it titled itself Device.
    const char *title(void) const override { return "Radar setup"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "A scan costs about 12s of";
        out[1] = "radio time whatever the rest.";
        return 2;
    }

    void enter(void) override {
        period_ = nova::reg_int(NOVA_KEY_PREFIX "Watch_Period", 20000);
        tell_   = notify_on();
        fresh_  = notify_new();
        dirty_  = false;
    }

    void leave(void) override {
        if (!dirty_) return;
        nova::reg_set_int(NOVA_KEY_PREFIX "Watch_Period", period_);
        nova::reg_set(NOVA_KEY_PREFIX "Watch_Notify", tell_ ? "on" : "off");
        nova::reg_set(NOVA_KEY_PREFIX "Watch_New", fresh_ ? "on" : "off");
        nova::reg_save();
    }

    void draw(Canvas &c) override {
        static const char *const kLabels[ROWS] = { "Scan every", "Tell me", "New devices" };
        char v[12];
        for (int i = 0; i < ROWS; i++) {
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (i == sel_);
            if (on) c.rounded_rect(0, y - 1, c.width(), ui::ROWH, 1, true);
            c.text(3, y, kLabels[i], on ? 0 : 1);
            if (i == 0) snprintf(v, sizeof(v), "%ds", period_ / 1000);
            else nova::copy(v, sizeof(v), (i == 1 ? tell_ : fresh_) ? "on" : "off");
            const int w = c.text_width(v, 1, false);
            c.text(c.width() - w - 3, y, v, on ? 0 : 1);
        }
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % ROWS; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + ROWS - 1) % ROWS; return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            dirty_ = true;
            if (sel_ == 0) {
                // v1 offered 4 s as well. It is not offered here: a single call
                // takes about twelve, so a four-second rest is indistinguishable
                // from no rest at all and only reads as one.
                static const int kSteps[] = { 8000, 20000, 60000 };
                int i = 0;
                while (i < 2 && kSteps[i] != period_) i++;
                period_ = kSteps[(i + 1) % 3];
            } else if (sel_ == 1) tell_ = !tell_;
            else                  fresh_ = !fresh_;
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    static constexpr int ROWS = 3;
    int  sel_, period_;
    bool tell_, fresh_, dirty_;
};

// --- Radar -----------------------------------------------------------------------
//
// Everything heard so far, strongest first. That is the whole screen, and it is
// deliberately the same list the MicroPython Radar was: a list that keeps its
// shape while the numbers move is far easier to read at a glance than a picture
// that redraws, and it is the shape somebody who used v1 already knows.
//
// One thing it can no longer show: v1 marked a device '!' when the advertisement
// identified it as a tracker. That came from the manufacturer data, which the
// shell does not pass on, so only the '*' for a named device is left.
//
// The filter is v1's, adapted to what this radio reports. v1 cycled all / BLE /
// WiFi / known because its observer folded in an access-point scan; there is no
// WiFi in this table, and `bt` distinguishes LE from classic, so the useful cycle
// is all / LE / BR / known.
class RadarScreen : public Screen {
public:
    const char *title(void) const override { return "Radar"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Hold SELECT to filter.";
        out[1] = "* is a device you named.";
        out[2] = "Top row is Radar setup.";
        return 3;
    }

    void enter(void) override {
        // Same as the BLE list: the cursor stays, draw() clamps it.
        scan_disown();
        phase_ = 0;
        index_build(filter_);
    }

    void leave(void) override { scan_disown(); }

    bool animating(void) const override { return g_ble_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        // Classic devices are only found by a classic inquiry, so the BR filter
        // asks for one. Every other mode scans LE, which is where nearly
        // everything actually is.
        const bool changed = scan_pump(5, rest_ms(), filter_ == F_BR);
        if (changed) index_build(filter_);
        return changed || g_ble_busy;
    }

    void draw(Canvas &c) override {
        const int rows = list_rows(c);
        // The settings row is pinned to the front, the same synthetic-first-row
        // pattern the rest of the suite uses, so it sits somewhere predictable
        // instead of behind a gesture nobody finds.
        const int total = g_nidx + 1;
        // Both ends, like the BLE list. The cursor is not reset on the way in
        // any more, so this clamp is the whole of what keeps it inside a table
        // that changes size on every scan.
        if (sel_ >= total) sel_ = total - 1;
        if (sel_ < 0)      sel_ = 0;
        if (sel_ < top_) top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

        const int right = c.width() - (ui::SB_W + 1);
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= total) break;
            const int y = ui::TOP + i * ui::ROWH;
            if (idx == 0) {
                if (sel_ == 0) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
                // The same words as the screen it opens. "Radar settings" was
                // longer than the status bar can show beside the clock, so the
                // two could not agree until one of them got shorter.
                c.text_fit(2, y, "Radar setup", sel_ == 0 ? 0 : 1, right - 12, false);
                c.text(right - ui::ADV - 2, y, ">", sel_ == 0 ? 0 : 1);
                continue;
            }
            const Dev &d = g_dev[g_idx[idx - 1]];
            draw_dev_row(c, d, y, idx == sel_, is_known(d.mac));
        }
        c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP,
                    c.height() - ui::TOP - ui::FH, top_, rows, total);

        // The count and the filter, which is v1's footer. What the radio is
        // doing takes the line instead whenever that is the more urgent fact —
        // scanning, or a scan that did not happen at all.
        char line[32];
        if (g_ble_busy || g_state != SC_OK) scan_status(line, sizeof(line), 5, true);
        else snprintf(line, sizeof(line), "%d %s", g_nidx, kFilterName[filter_]);
        c.text_fit(2, c.height() - ui::FH, line, 1, c.width() - 12, false);
        if (g_ble_busy) c.spinner(c.width() - 9, c.height() - ui::FH, phase_ / BLE_SPIN_MS, 1);
    }

    Action on_event(Event e) override {
        const int total = g_nidx + 1;
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % total; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + total - 1) % total; return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            if (sel_ == 0) gui::push<RadarSettings>();
            else if (sel_ - 1 < g_nidx) open_device(g_dev[g_idx[sel_ - 1]]);
            return ui::ACT_STAY;
        }
        if (e == EV_SELECT_HOLD) {
            filter_ = (filter_ + 1) % F_COUNT;
            sel_ = top_ = 0;
            index_build(filter_);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int      sel_, top_, filter_;
    unsigned phase_;
};

// --- Presence -----------------------------------------------------------------------
//
// The named devices, and whether they are here. The "is anyone home" view — the
// same trick a smart camera uses when it recognises a phone, and nothing is
// transmitted to do it.
//
// DEVICE-UNCONFIRMED: no arrival or departure has been watched happen. It needs a
// second Bluetooth device carried out of range and brought back, over more than
// the ninety seconds and two passes it takes to call something departed, and that
// cannot be done on the host. The parsing and the table logic underneath it are
// exercised; the transitions themselves are not.
class PresenceScreen : public Screen {
public:
    const char *title(void) const override { return "Presence"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT locates a device.";
        out[1] = "Arrivals are only noticed";
        out[2] = "while this screen is open.";
        return 3;
    }

    // The cursor stays; draw() clamps it against the named-device count.
    void enter(void) override { scan_disown(); phase_ = 0; }
    void leave(void) override { scan_disown(); }

    bool animating(void) const override { return g_ble_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        const bool changed = scan_pump(5, rest_ms(), false);
        return changed || g_ble_busy;
    }

    void draw(Canvas &c) override {
        const int n = known_count();
        char line[32];

        if (n == 0) {
            c.text(2, ui::TOP, "No named devices.", 1);
            c.text(2, ui::TOP + ui::ROWH, "Open one in BLE or", 1);
            c.text(2, ui::TOP + 2 * ui::ROWH, "Radar and name it to", 1);
            c.text(2, ui::TOP + 3 * ui::ROWH, "watch for it here.", 1);
            return;
        }

        const int rows = list_rows(c);
        if (sel_ >= n) sel_ = n - 1;
        if (sel_ < 0)  sel_ = 0;
        if (sel_ < top_) top_ = sel_;
        else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

        char mac[18], label[20];
        for (int i = 0; i < rows; i++) {
            const int idx = top_ + i;
            if (idx >= n) break;
            if (!known_at(idx, mac, sizeof(mac), label, sizeof(label))) continue;
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, c.width(), ui::ROWH, 1, true);

            const Dev *d = dev_find(mac);
            // A named device that has never been heard still gets a row, as
            // absent. Leaving it out would look like it was never named.
            const bool here = d && !d->gone;
            if (here && d->rssi) snprintf(line, sizeof(line), "%d dBm", (int)d->rssi);
            else                 nova::copy(line, sizeof(line), here ? "here" : "away");
            const int w = c.text_width(line, 1, true);
            c.text(c.width() - w - 2, y, line, on ? 0 : 1, 1, true);
            c.text_fit(2, y, label, on ? 0 : 1, c.width() - w - 6, false);
        }

        if (g_ble_busy || (g_state != SC_OK && g_state != SC_NEVER))
            scan_status(line, sizeof(line), 5, true);
        else
            snprintf(line, sizeof(line), "%d watched", n);
        c.text_fit(2, c.height() - ui::FH, line, 1, c.width() - 12, false);
        if (g_ble_busy) c.spinner(c.width() - 9, c.height() - ui::FH, phase_ / BLE_SPIN_MS, 1);
    }

    Action on_event(Event e) override {
        const int n = known_count();
        if (!n) return Screen::on_event(e);
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + n - 1) % n; return ui::ACT_STAY; }
        if (e == EV_SELECT) {
            char mac[18], label[20];
            if (known_at(sel_, mac, sizeof(mac), label, sizeof(label))) {
                nova::copy(g_focus, sizeof(g_focus), mac);
                gui::push<LocateScreen>();
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int      sel_, top_;
    unsigned phase_;
};

// --- the App table's entry points ---------------------------------------------------

void open_ble(void)      { gui::push<BleScreen>(); }
void open_radar(void)    { gui::push<RadarScreen>(); }
void open_presence(void) { gui::push<PresenceScreen>(); }

}  // namespace screens
}  // namespace nova
