// Desc: The Nova D1, compiled to WebAssembly, for the simulator on the web.
// File: sim_wasm.cpp
//
// The runner and every screen, built from the SAME source files the device runs
// (novapkg_all.inc), against a faked firmware, with a page on the other end
// supplying the keys and the clock. It replaces a simulator that downloaded the
// MicroPython modules and ran those, which showed a device that no longer ships.
//
// What is real and what is not:
//
//   REAL      every pixel. The page does not draw the UI, it shows the panel the
//             SSD1309 driver would have produced: this file models the
//             controller's memory and fills it from the I2C writes display.cpp
//             makes, so a bug in the page-diff push shows up here as it would on
//             the glass.
//   REAL      every screen's logic, the folder navigation, the lock, dim and off,
//             the toast, the settings tree, the key handling.
//   SCRIPTED  everything on the far side of fw_*: the networks that are
//             "around", the radios' answers, the files, the clock. The firmware's
//             own replies are the strings fakefw_d1.inc copies from a real board.
//
// ONE THREAD, NO COROUTINES. A device runs a worker task per slow job (a scan, a
// card read) and the screen shows a spinner until it finishes. Here a spawned
// task is QUEUED to run once the wall time it would take has passed, runs to
// completion in one go on a clock of its own, and then the page's frame loop sees
// its result exactly as it would see the finished task's. The panel is therefore
// spinning for the right time and the worker never needs to be suspended. A
// worker that LOOPS (the wardrive survey) is cut off after a few passes.

#include "../include/rpc_app.h"

#include <stdint.h>
#include <string.h>

// --- the pieces of the shared fake that this file replaces ----------------------------
//
// fakefw_d1.inc defines these directly, so they are renamed out of the way for
// the length of the include and the real names are defined below. The header is
// pulled in first so its declarations keep the real names.
#define fw_millis            fakefw_millis_unused
#define fw_micros            fakefw_micros_unused
#define fw_busy_wait_us      fakefw_busy_wait_us_unused
#define fw_task_sleep_ms     fakefw_task_sleep_ms_unused
#define fw_task_should_stop  fakefw_task_should_stop_unused
#define fw_task_spawn        fakefw_task_spawn_unused
#define fw_i2c_write         fakefw_i2c_write_unused
#define fw_net_connected     fakefw_net_connected_unused
#define fw_net_ssid          fakefw_net_ssid_unused
#define fw_net_ip            fakefw_net_ip_unused
#define fw_net_rssi          fakefw_net_rssi_unused
#define fw_net_scan          fakefw_net_scan_unused
#define fw_net_scan2         fakefw_net_scan2_unused
#define fw_time_get          fakefw_time_get_unused
#define fw_time_set          fakefw_time_set_unused
#define fw_shell_run         fakefw_shell_run_unused
#define fw_dir_count         fakefw_dir_count_unused
#define fw_dir_entry         fakefw_dir_entry_unused
#include "fakefw_d1.inc"
#undef fw_millis
#undef fw_micros
#undef fw_busy_wait_us
#undef fw_task_sleep_ms
#undef fw_task_should_stop
#undef fw_task_spawn
#undef fw_i2c_write
#undef fw_net_connected
#undef fw_net_ssid
#undef fw_net_ip
#undef fw_net_rssi
#undef fw_net_scan
#undef fw_net_scan2
#undef fw_time_get
#undef fw_time_set
#undef fw_shell_run
#undef fw_dir_count
#undef fw_dir_entry

// --- time, and tasks that run when their time comes -----------------------------------

// The page's clock. Advanced only by sim_frame(), so a tab left in the background
// does not return to a device that has been running a scan for an hour.
static uint32_t g_sim_ms;

// A queued task runs on a clock of its own, starting at the moment it was due.
// Its sleeps advance THAT clock and nothing else: a worker that waits out a
// five-second scan finishes instantly in real time, having seen five seconds go
// by, and the page's clock has not moved.
static bool     g_in_task;
static uint64_t g_task_us;          // the task's clock, in microseconds
static uint32_t g_task_start;       // ... and when it began, in the page's milliseconds
static uint32_t g_task_budget;      // 0 = no limit; a looping worker is cut off

struct SimTask {
    int       (*fn)(void *);
    void       *arg;
    uint32_t    due;
    bool        used;
    bool        loops;
    char        name[16];
};
static SimTask g_sim_tasks[16];
static int     g_sim_task_seq;

extern "C" {

uint32_t fw_millis(void) { return g_in_task ? (uint32_t)(g_task_us / 1000u) : g_sim_ms; }

// READING THE CLOCK COSTS TIME, inside a task. A driver that spins on fw_micros()
// waiting for an edge (the DHT's one-wire read) would otherwise wait on a clock
// nothing advances and hang the tab; a few microseconds a read is what the real
// call costs, and it makes every such loop end in a timeout the way it does on a
// board with nothing attached.
uint32_t fw_micros(void) {
    if (!g_in_task) return g_sim_ms * 1000u;
    // While a one-wire waveform is scripted the fake keeps the clock that waveform
    // is timed against (each poll of the pin advances it); read that one, or the
    // driver would time the pulses against a clock the waveform knows nothing of.
    if (g_dht_wave_on) return g_dht_clock;
    g_task_us += 4;
    return (uint32_t)g_task_us;
}
void fw_busy_wait_us(uint32_t us) { if (g_in_task) g_task_us += us; }
void fw_task_sleep_ms(uint32_t ms) {
    if (g_in_task) g_task_us += (uint64_t)ms * 1000u; else g_sim_ms += ms;
}
int fw_task_should_stop(void) {
    if (g_should_stop) return 1;
    return g_in_task && g_task_budget && (uint32_t)(g_task_us / 1000u) - g_task_start >= g_task_budget;
}

// How long the real thing takes, measured on a Pico 2 W where it has been
// measured and rounded where it has not. These are the times a spinner is seen
// for; none of them is a promise about hardware.
static uint32_t task_duration(const char *name, bool *loops) {
    *loops = false;
    if (!strcmp(name, "novabt"))      return 6500;    // bt scan le 5: ~12 s on the board, 5 listening
    if (!strcmp(name, "novaread"))    return 2200;    // somebody presents a card
    if (!strcmp(name, "novarf"))      return 1400;
    if (!strcmp(name, "novawifi"))    return 3200;
    if (!strcmp(name, "novalan"))     return 4200;
    if (!strcmp(name, "novawardrive")) { *loops = true; return 800; }
    if (!strcmp(name, "novaclimate")) return 400;
    if (!strcmp(name, "novamedia"))   return 500;
    if (!strcmp(name, "novausb"))     return 900;
    return 600;                                       // d1cmd, d1-cmd, the tasks helper
}

int fw_task_spawn(const char *name, int (*fn)(void *), void *arg, uint32_t) {
    g_spawned++;
    int id = ++g_sim_task_seq + 10;
    if (!fn || !name) return id;
    // These two never end by design: the runner's own loop, which the page drives
    // a frame at a time, and the input poller, which the page replaces by calling
    // input().inject(). Queueing either would hang the tab.
    if (!strcmp(name, "novagui") || !strcmp(name, "novainput") || !strcmp(name, "d1self"))
        return id;
    for (SimTask &t : g_sim_tasks) {
        if (t.used) continue;
        t.used = true;
        t.fn = fn; t.arg = arg;
        snprintf(t.name, sizeof(t.name), "%s", name);
        t.due = g_sim_ms + task_duration(name, &t.loops);
        return id;
    }
    return -1;      // the device's table is full too, and says so the same way
}

}  // extern "C"

static void sim_before_task(const char *name);
static void sim_after_task(const char *name);

static void run_due_tasks(void) {
    for (SimTask &t : g_sim_tasks) {
        if (!t.used || t.due > g_sim_ms) continue;
        t.used = false;                        // free the slot first: a worker may spawn another
        g_in_task      = true;
        g_task_start   = t.due;
        g_task_us      = (uint64_t)t.due * 1000u;
        g_task_budget  = t.loops ? 9000 : 0;
        sim_before_task(t.name);
        t.fn(t.arg);
        sim_after_task(t.name);
        g_in_task      = false;
        g_task_budget  = 0;
    }
}

// --- the wall clock -------------------------------------------------------------------

static bool     g_clock_set;
static int64_t  g_clock_base_s;     // local seconds since 1970 at g_clock_base_ms
static uint32_t g_clock_base_ms;

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

extern "C" int fw_time_get(struct FwTime *out) {
    if (!g_clock_set) return 0;
    if (!out) return 1;
    int64_t s = g_clock_base_s + (int64_t)(g_sim_ms - g_clock_base_ms) / 1000;
    int64_t days = s / 86400;
    int64_t rem  = s - days * 86400;
    if (rem < 0) { rem += 86400; days--; }
    civil_from_days(days, &out->year, &out->month, &out->day);
    out->hour    = (int)(rem / 3600);
    out->minute  = (int)((rem % 3600) / 60);
    out->second  = (int)(rem % 60);
    out->weekday = (int)(((days % 7) + 11) % 7);       // 1970-01-01 was a Thursday
    return 1;
}

// Days since 1970-01-01 from a civil date (Howard Hinnant's algorithm), the inverse of the above.
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    return era * 146097 + (int64_t)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
}

// A package setting the clock moves the page's clock to match: from then on it runs from there.
extern "C" int fw_time_set(const struct FwTime *in) {
    if (!in || in->year < 2000 || in->year > 2099 || in->month < 1 || in->month > 12 || in->day < 1 || in->day > 31 ||
        in->hour < 0 || in->hour > 23 || in->minute < 0 || in->minute > 59 || in->second < 0 || in->second > 59) return 0;
    g_clock_base_s  = days_from_civil(in->year, in->month, in->day) * 86400 + in->hour * 3600 + in->minute * 60 + in->second;
    g_clock_base_ms = g_sim_ms;
    g_clock_set     = true;
    return 1;
}

// --- the network ------------------------------------------------------------------------

static int g_sim_net = 1;

extern "C" {
int fw_net_connected(void) { return g_sim_net; }
int fw_net_ssid(char *out, unsigned cap) {
    return (int)snprintf(out, cap, "%s", g_sim_net ? "Nova" : "");
}
int fw_net_ip(char *out, unsigned cap) {
    if (!g_sim_net) { if (cap) out[0] = 0; return 0; }
    return (int)snprintf(out, cap, "192.168.1.42");
}
int fw_net_rssi(void) { return g_sim_net ? -52 : 0; }

// A few access points, strongest first, so the list has something to scroll and
// the signal bars show more than one height. Invented names; nothing here is a
// network that exists.
int fw_net_scan(FwNetAp *out, unsigned max) {
    static const struct { const char *ssid; int rssi, ch, sec; } aps[] = {
        { "Nova",             -52,  6, 1 },
        { "HomeNet-5G",       -61, 36, 1 },
        { "Pixel_4A2F",       -66, 11, 1 },
        { "TP-Link_9F2A",     -72,  1, 1 },
        { "Cafe Guest",       -74,  6, 0 },
        { "Neighborhood-Mesh-Network-5GHz-Guest", -77, 40, 1 },
        { "NETGEAR47",        -79,  9, 1 },
        { "Linksys00421",     -83,  3, 1 },
    };
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof(aps) / sizeof(aps[0]) && n < max; i++, n++) {
        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", aps[i].ssid);
        out[n].rssi    = aps[i].rssi;
        out[n].channel = aps[i].ch;
        out[n].secured = aps[i].sec;
    }
    return (int)n;
}

// The same neighbourhood as the radios it has, which is not the same list: one network is on two
// radios (as a router with two bands is), and one has no name. Addresses are made up.
int fw_net_scan2(FwNetAp2 *out, unsigned max) {
    static const struct { uint8_t mac5; const char *ssid; int rssi, ch, sec; } aps[] = {
        { 0x10, "Nova",             -52,  6, 4 },
        { 0x11, "Nova",             -58, 36, 4 },
        { 0x20, "HomeNet-5G",       -61, 36, 4 },
        { 0x30, "Pixel_4A2F",       -66, 11, 4 },
        { 0x40, "TP-Link_9F2A",     -72,  1, 6 },
        { 0x50, "Cafe Guest",       -74,  6, 0 },
        { 0x60, "",                 -75,  3, 4 },
        { 0x70, "Neighborhood-Mesh-Network-5GHz-Guest", -77, 40, 4 },
        { 0x80, "NETGEAR47",        -79,  9, 4 },
        { 0x90, "Linksys00421",     -83,  3, 1 },
    };
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof(aps) / sizeof(aps[0]) && n < max; i++, n++) {
        memset(&out[n], 0, sizeof(out[n]));
        const uint8_t mac[6] = { 0x02, 0x5a, 0x4e, 0x00, 0x00, aps[i].mac5 };
        memcpy(out[n].bssid, mac, 6);
        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", aps[i].ssid);
        out[n].rssi    = (int16_t)aps[i].rssi;
        out[n].channel = (uint16_t)aps[i].ch;
        out[n].sec     = (uint8_t)aps[i].sec;
        out[n].hidden  = aps[i].ssid[0] == 0;
    }
    return (int)n;
}

// The shared fake answers the commands the screens read back, copied from a real
// board. These are the few more the simulator's visitor can reach that the tests
// never did. Anything still unknown comes back empty, which is what the real
// shell does for a command that printed nothing.
int fw_shell_run(const char *line, char *out, uint32_t cap) {
    if (line && out && cap) {
        if (!strcmp(line, "usbmode")) {
            snprintf(out, cap, "[:] USB\n  USB mode: console\n"
                               "    storage   off\n"
                               "    keyboard  off         present but idle\n");
            return 0;
        }
        if (!strncmp(line, "badusb ", 7)) {
            snprintf(out, cap, "[@] Ran 3 lines.\n");
            return 0;
        }
    }
    return fakefw_shell_run_unused(line, out, cap);
}

// A believable tree for the file browser. The shared fake lists only the few
// directories a test cares about; this adds the ones a visitor will open first.
struct SimDir { const char *dir; const char *name; int is_dir; uint32_t size; };
static const SimDir kSimDirs[] = {
    { "/",          "nova",        1, 0 },
    { "/",          "pkg",         1, 0 },
    { "/",          "users",       1, 0 },
    { "/",          "boot.rps",    0, 212 },
    { "/nova",      "apps",        1, 0 },
    { "/nova",      "badusb",      1, 0 },
    { "/nova",      "logs",        1, 0 },
    { "/nova",      "music",       1, 0 },
    { "/nova",      "codes",       1, 0 },
    { "/nova/logs", "d1.log",      0, 1432 },
    { "/pkg",       "novad1.app",  0, 190336 },
    { "/pkg",       "calc.app",    0, 7968 },
    { "/users",     "root",        1, 0 },
};
static const unsigned kSimDirsN = sizeof(kSimDirs) / sizeof(kSimDirs[0]);

int fw_dir_count(const char *path) {
    int n = fakefw_dir_count_unused(path);
    for (unsigned i = 0; i < kSimDirsN; i++) {
        if (strcmp(kSimDirs[i].dir, path)) continue;
        // The shared fake already lists some entries of /nova; do not list a name twice.
        bool dup = false;
        for (int k = 0; k < n && !dup; k++) {
            FwDirEntry e;
            if (fakefw_dir_entry_unused(path, (unsigned)k, &e) && !strcmp(e.name, kSimDirs[i].name)) dup = true;
        }
        if (!dup) n++;
    }
    return n;
}

int fw_dir_entry(const char *path, unsigned index, FwDirEntry *out) {
    int base = fakefw_dir_count_unused(path);
    if ((int)index < base) return fakefw_dir_entry_unused(path, index, out);
    unsigned want = index - (unsigned)base;
    for (unsigned i = 0; i < kSimDirsN; i++) {
        if (strcmp(kSimDirs[i].dir, path)) continue;
        bool dup = false;
        for (int k = 0; k < base && !dup; k++) {
            FwDirEntry e;
            if (fakefw_dir_entry_unused(path, (unsigned)k, &e) && !strcmp(e.name, kSimDirs[i].name)) dup = true;
        }
        if (dup) continue;
        if (want-- != 0) continue;
        snprintf(out->name, FW_NAME_MAX, "%s", kSimDirs[i].name);
        out->is_dir = kSimDirs[i].is_dir;
        out->size   = kSimDirs[i].size;
        return 1;
    }
    return 0;
}
}  // extern "C"

// --- the panel ----------------------------------------------------------------------------
//
// The SSD1309's display memory, filled from the I2C stream display.cpp writes. A
// command run (control byte 0x00) can set the page (0xB0|p), the column (0x00|lo,
// 0x10|hi), the contrast (0x81 v), on/off (0xAE/0xAF) and inversion (0xA6/0xA7);
// a data run (0x40) writes bytes at the current column and advances it. The
// SH1106 has a 132-column memory with the picture two columns in, which the init
// sequence announces with its DC-DC command (0xAD); everything else is common.

static uint8_t  g_gdram[8][132];
static unsigned g_page, g_col;
static uint32_t g_panel_gen;        // bumped on every data write, so the page can skip a still frame
static uint8_t  g_contrast = 0x80;
static bool     g_panel_on = true;
static bool     g_panel_inv;
static int      g_col_shift;        // 2 on an SH1106

extern "C" int fw_i2c_write(unsigned, unsigned, const void *buf, unsigned len, int) {
    g_i2c_writes++;
    if (!g_panel_present) return -1;
    const uint8_t *b = (const uint8_t *)buf;
    if (len < 1) return 0;
    if (b[0] == 0x40) {
        for (unsigned i = 1; i < len; i++) {
            if (g_col < 132 && g_page < 8) g_gdram[g_page][g_col] = b[i];
            g_col++;
        }
        g_panel_gen++;
        return 0;
    }
    // A command run. Two-byte commands take their argument from the next byte.
    for (unsigned i = 1; i < len; i++) {
        uint8_t c = b[i];
        if      ((c & 0xf8) == 0xb0) g_page = c & 7;
        else if ((c & 0xf0) == 0x00) g_col = (g_col & 0xf0) | (c & 0x0f);
        else if ((c & 0xf0) == 0x10) g_col = (g_col & 0x0f) | ((unsigned)(c & 0x0f) << 4);
        else if (c == 0x81 && i + 1 < len) { g_contrast = b[++i]; g_panel_gen++; }
        else if (c == 0xae) { g_panel_on = false; g_panel_gen++; }
        else if (c == 0xaf) { g_panel_on = true;  g_panel_gen++; }
        else if (c == 0xa6) { g_panel_inv = false; g_panel_gen++; }
        else if (c == 0xa7) { g_panel_inv = true;  g_panel_gen++; }
        else if (c == 0xad && i + 1 < len) { g_col_shift = 2; i++; }
        else if ((c == 0xd5 || c == 0xa8 || c == 0xd3 || c == 0x8d || c == 0x20 ||
                  c == 0xda || c == 0xd9 || c == 0xdb) && i + 1 < len) i++;   // arguments we do not model
    }
    return 0;
}

// --- the package ---------------------------------------------------------------------------

#include "novapkg_all.inc"

// --- the hardware that is not there ---------------------------------------------------------------
//
// A browser has no GPS and no DHT22, and the screens that read them would show
// "nothing answered" forever. This plays the part of the parts: a position that
// is obviously not a place (0, 0, which is in the sea off West Africa), and a
// room that warms and cools a little. Everything it says is made up, and the page
// says so.

static void nmea_line(char *out, size_t cap, const char *body) {
    unsigned cs = 0;
    for (const char *p = body; *p; p++) cs ^= (unsigned char)*p;
    snprintf(out, cap, "$%s*%02X\r\n", body, cs);
}

static uint32_t g_gps_fed_at = 0xffffffffu;

static void sim_feed_gps(void) {
    // A second of NMEA at a time, the way a module reports at 1 Hz.
    if (g_gps_fed_at != 0xffffffffu && g_sim_ms - g_gps_fed_at < 1000) return;
    g_gps_fed_at = g_sim_ms;
    struct FwTime t;
    if (!fw_time_get(&t)) { t.hour = 12; t.minute = 0; t.second = 0; t.day = 1; t.month = 1; t.year = 2026; }
    char gga_body[96], rmc_body[96], a[110], b[110], both[240];
    snprintf(gga_body, sizeof(gga_body), "GPGGA,%02d%02d%02d.00,0000.0000,N,00000.0000,E,1,07,1.1,12.0,M,0.0,M,,",
             t.hour, t.minute, t.second);
    snprintf(rmc_body, sizeof(rmc_body), "GPRMC,%02d%02d%02d.00,A,0000.0000,N,00000.0000,E,0.0,0.0,%02d%02d%02d,,,A",
             t.hour, t.minute, t.second, t.day, t.month, t.year % 100);
    nmea_line(a, sizeof(a), gga_body);
    nmea_line(b, sizeof(b), rmc_body);
    snprintf(both, sizeof(both), "%s%s", a, b);
    uart_feed(both);
}

// Temperature and humidity as the DHT22/DHT11 frame carries them: the integer
// bytes, with the checksum on the end. A slow drift so a second reading is not
// identical to the first.
static void sim_arm_dht(void) {
    int phase = (int)((g_sim_ms / 1000u) % 60u);
    unsigned char b[5] = { (unsigned char)(48 + phase / 12), 0, (unsigned char)(22 + (phase / 20)), 0, 0 };
    b[4] = (unsigned char)(b[0] + b[1] + b[2] + b[3]);
    static DhtSeg wave[2 + 40 * 2 + 1];
    int n = 0;
    wave[n].level = 0; wave[n].dur_us = 80; n++;
    wave[n].level = 1; wave[n].dur_us = 80; n++;
    for (int byte = 0; byte < 5; byte++)
        for (int bit = 7; bit >= 0; bit--) {
            bool one = (b[byte] >> bit) & 1;
            wave[n].level = 0; wave[n].dur_us = 50; n++;
            wave[n].level = 1; wave[n].dur_us = one ? 70 : 27; n++;
        }
    wave[n].level = 0; wave[n].dur_us = 50; n++;
    int pin = nova::board::pin(nova::board::PIN_DHT);
    if (pin >= 0) dht_wave_script((unsigned)pin, wave, (unsigned)n);
}

static void sim_before_task(const char *name) {
    if (!strcmp(name, "novaclimate")) sim_arm_dht();
}
static void sim_after_task(const char *name) {
    if (!strcmp(name, "novaclimate")) dht_wave_clear();
}

// --- what the page calls ----------------------------------------------------------------------

static uint8_t  g_fb[8 * 128];
static uint32_t g_next_frame;
static uint32_t g_last_frame_at;
static bool     g_up;

#define SIM_EXPORT(name) __attribute__((export_name(#name)))

extern "C" {

// Bring the device up. `local_epoch_s` is the visitor's local time as seconds
// since 1970, so the status bar shows the time where they are. Returns 1 when the
// runner started.
SIM_EXPORT(sim_init) int sim_init(uint32_t local_epoch_s) {
    g_clock_set     = local_epoch_s != 0;
    g_clock_base_s  = local_epoch_s;
    g_clock_base_ms = g_sim_ms;
    g_fake_time_ok  = false;
    g_net_up        = 1;
    if (!nova::gui::begin()) return 0;
    g_up = true;
    g_last_frame_at = g_sim_ms;
    g_next_frame = g_sim_ms;
    return 1;
}

// A gesture, as nova::Event: 1 turn right, 2 turn left, 3 select, 4 back, 5 home,
// 6 select held, 7 home held.
SIM_EXPORT(sim_key) void sim_key(int ev) {
    if (ev > 0 && ev <= 7) nova::input().inject((nova::Event)ev);
    g_next_frame = g_sim_ms;           // a press is not left waiting for the idle nap
}

// Move the world on by dt milliseconds of wall time and run whatever is due.
// Returns the panel generation, which changes when the picture does.
SIM_EXPORT(sim_frame) uint32_t sim_frame(uint32_t dt_ms) {
    if (!g_up) return 0;
    g_sim_ms += dt_ms;
    sim_feed_gps();
    run_due_tasks();
    if (g_sim_ms >= g_next_frame) {
        uint32_t nap = nova::gui::frame_once(&g_last_frame_at);
        g_next_frame = g_sim_ms + nap;
    }
    return g_panel_gen;
}

// The panel's memory as the controller holds it: 8 pages of 128 bytes, each byte
// a column, bit 0 the top row of the page.
SIM_EXPORT(sim_fb) const uint8_t *sim_fb(void) {
    for (int p = 0; p < 8; p++) memcpy(g_fb + p * 128, g_gdram[p] + g_col_shift, 128);
    return g_fb;
}

// bit 0 on, bit 1 inverted, bits 8..15 the contrast.
SIM_EXPORT(sim_panel) uint32_t sim_panel(void) {
    return (g_panel_on ? 1u : 0u) | (g_panel_inv ? 2u : 0u) | ((uint32_t)g_contrast << 8);
}

// What this module was built from, for the page to show. build_sim.sh fills it in.
SIM_EXPORT(sim_build) const char *sim_build(void) {
#ifdef SIM_STAMP
    return SIM_STAMP;
#else
    return "Nova D1 (unstamped build)";
#endif
}

// Lets the page offer a "Wi-Fi off" toggle so the offline screens can be seen.
SIM_EXPORT(sim_net) void sim_net(int up) { g_sim_net = up ? 1 : 0; g_net_up = g_sim_net; }

}  // extern "C"
