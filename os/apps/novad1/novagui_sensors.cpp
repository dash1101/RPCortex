// Desc: The direct-hardware screens — battery, DHT11, GPS and IR.
// File: novagui_sensors.cpp
//
// Four screens that were LISTED with no open() because none of them has a
// firmware command behind it the way the SX1276 and the iButton do — see
// novagui_contact.cpp's own note that there is no NFC or 1-Wire call in the
// package ABI, and there should not be one for a bus that needs a handshake
// timed in microseconds. Battery and IR do not need one: an ADC read and a
// coalesced edge count are both already in the ABI, on purpose, for exactly
// this. Climate is the one case where the honest answer needs a one-wire
// handshake anyway, and it is written the way the standalone `dht` package
// already proves that handshake works, not against a firmware command —
// packages cannot call packages; see the note over ClimateScreen. GPS needs
// neither: a UART is a real peripheral with its own timing, not a bit-banged
// wire, so it is just read.
//
// Every one of the four is gated on something a chip actually did — an ADC
// reading inside a plausible window, a checksum that adds up, an edge that
// was seen — never on a pin merely being assigned. "Pins assigned" is what
// module_wired() already checks at the catalogue level, and it answers a
// different question: whether the row is worth showing at all. A module can
// be wired and still not there, which is exactly why MOD_UNKNOWN does not
// grey a row out — these screens are the thing that turns "wired" into
// "answered" or "did not answer", the same way the LoRa screen's `lora
// status` does for a chip that DOES have a register to ask.
#include "novagui_sensors.h"
#include "novagui.h"
#include "novacore.h"
#include "novaboard.h"
#include "novapower.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// --- Battery ---------------------------------------------------------------------
//
// The one sensor here with a driver already written: novapower.cpp reads the
// ADC divider for the status bar's own battery badge, cached and gated for
// plausibility exactly the way this screen wants. There is nothing to
// reimplement — the screen is a window onto power::percent()/millivolts(),
// and "no sense pin" is power::percent() returning -1, the same answer the
// boot check and the Hardware screen already give that question.
//
// PIN_BATTERY has no board default on either reference profile (see
// novapower.h's own note): an unwired ADC input floats, and a floating input
// reads as SOMETHING, so guessing a pin would be a confident wrong number
// rather than no gauge. On a stock board that also means the catalogue row
// itself greys out — MOD_UNWIRED — so this screen is normally reached only by
// something that opens it directly, a test or the renderer. That is correct
// and not a gap this screen needs to work around: an unwired module is
// LISTED, not hidden, and greyed is how "listed but not wired" already looks
// everywhere else.
class BatteryScreen : public Screen {
public:
    const char *title(void) const override { return "Battery"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "A live ADC reading.";
        out[1] = "Set the pin in d1 pins.";
        return 2;
    }

    void enter(void) override { last_pct_ = -2; last_src_ = -1; }

    bool tick(uint32_t) override {
        int pct = power::percent();
        int src = (int)power::source();
        bool changed = (pct != last_pct_) || (src != last_src_);
        last_pct_ = pct;
        last_src_ = src;
        return changed;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;

        if (board::pin(board::PIN_BATTERY) == board::PIN_NONE) {
            c.text(2, y, "No sense pin.", 1);
            y += ui::ROWH;
            c.text(2, y, "Set one with", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "d1 pins set battery", 1, c.width() - 4, false);
            return;
        }

        int pct = power::percent();
        if (pct < 0) {
            // A pin is assigned and the reading still fell outside the
            // plausible window novapower.cpp gates on — an unwired divider on
            // an assigned GPIO, not a missing pin, so the remedy said here has
            // to be a different one.
            c.text(2, y, "No reading.", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "Check the divider wiring.", 1, c.width() - 4, false);
            return;
        }

        char line[32];
        snprintf(line, sizeof(line), "%d%%", pct);
        c.text(2, y, line, 1);
        y += ui::ROWH;
        snprintf(line, sizeof(line), "%d mV", power::millivolts());
        c.text(2, y, line, 1);
        y += ui::ROWH;

        switch (power::source()) {
            case power::PWR_USB:     c.text(2, y, "On USB", 1); break;
            case power::PWR_BATTERY: c.text(2, y, power::low() ? "On battery - low" : "On battery", 1); break;
            default:                 c.text(2, y, "Source unknown", 1); break;
        }
    }

private:
    int last_pct_, last_src_;
};

// --- Climate (DHT11) --------------------------------------------------------------
//
// THERE IS NO ONE-WIRE CALL IN THE PACKAGE ABI, and the honest way to read one
// is the way the standalone `dht` package already does it: a request pulse
// held by an output, then forty bits timed off fw_micros() with nothing that
// yields in between. That package cannot be called from here — a package
// dispatched from inside another package is refused, by fw_shell_run and by
// fw_shell_run_detached too, on every task including a spawned one, because
// only one package's code can be resident at a time. So the protocol below is
// the SAME one, reproduced against the same ABI rather than borrowed at
// runtime: the timing constants and the wire shape are copied from
// os/apps/dht/dht.cpp, which has already proven them on this ABI and is
// host-tested against a scripted waveform the same way this file is.
//
// DEVICE-UNCONFIRMED: a call into firmware and back carries more jitter than a
// direct GPIO toggle would, and the 26-28us/70us split is not a wide one. The
// standalone package carries the same exposure and is the standing evidence
// that it holds up on this hardware; this screen has not itself been checked
// against a real DHT11.
#define DHT_TIMEOUT_US 200      // no edge in this long means the sensor is gone

enum DhtErr { DHT_OK = 0, DHT_NO_RESPONSE, DHT_TIMEOUT, DHT_CHECKSUM };

static int dht_wait_level(unsigned pin, int level) {
    uint32_t start = fw_micros();
    while (fw_gpio_get(pin) != level) {
        if (fw_micros() - start > DHT_TIMEOUT_US) return -1;
    }
    return (int)(fw_micros() - start);
}

static DhtErr dht_read_raw(unsigned pin, unsigned char *bytes) {
    // The start pulse. Held by an output, so the only requirement is "at
    // least this long", which is a proper yielding sleep — DHT11 wants 18ms
    // or more. Everything after this point cannot yield.
    fw_gpio_init(pin, FW_PIN_OUT);
    fw_gpio_put(pin, 0);
    fw_task_sleep_ms(20);

    fw_gpio_init(pin, FW_PIN_IN);
    fw_gpio_pull(pin, FW_PULL_UP);

    if (dht_wait_level(pin, 0) < 0) return DHT_NO_RESPONSE;   // sensor's 80us low
    if (dht_wait_level(pin, 1) < 0) return DHT_TIMEOUT;       // sensor's 80us high
    if (dht_wait_level(pin, 0) < 0) return DHT_TIMEOUT;       // first bit's low

    for (int i = 0; i < 40; i++) {
        if (dht_wait_level(pin, 1) < 0) return DHT_TIMEOUT;   // the 50us low ended
        int high = dht_wait_level(pin, 0);                    // time the high
        if (high < 0) return DHT_TIMEOUT;
        // 26-28us is a zero, 70us is a one; 40 splits them with room either
        // side.
        bytes[i / 8] <<= 1;
        if (high > 40) bytes[i / 8] |= 1;
    }

    unsigned sum = (unsigned)bytes[0] + bytes[1] + bytes[2] + bytes[3];
    if ((sum & 0xff) != bytes[4]) return DHT_CHECKSUM;
    return DHT_OK;
}

static const char *dht_err_text(DhtErr e) {
    switch (e) {
        case DHT_NO_RESPONSE: return "nothing answered";
        case DHT_TIMEOUT:     return "stopped replying";
        case DHT_CHECKSUM:    return "bad checksum";
        default:              return "";
    }
}

// The worker. Runs on a task of its own, like every read here that cannot be
// interrupted: the request pulse alone holds the task for 20ms and the forty
// bits after it hold it for another five without yielding, which tick() must
// never be allowed to do to the panel. A generation counter, not a cancel
// flag, for the reason ContactScreen's is one: nothing in dht_read_raw looks
// at a flag between fw_micros() calls, so there is nowhere to be cancelled —
// what leaving the screen can do is make the answer irrelevant once it lands.
static volatile uint32_t g_cl_gen;
static volatile uint32_t g_cl_req_gen;
static volatile uint8_t  g_cl_busy;
static volatile uint8_t  g_cl_ready;
static volatile uint8_t  g_cl_err;
static unsigned char     g_cl_bytes[5];
static uint32_t          g_cl_last_ms;      // fw_millis() the last read finished

#define CL_SPIN_MS  140
#define CL_GAP_MS  2000      // the part needs this long between reads

static int cl_worker(void *arg) {
    unsigned pin = (unsigned)(uintptr_t)arg;
    const uint32_t mine = g_cl_req_gen;
    unsigned char b[5] = { 0, 0, 0, 0, 0 };
    DhtErr e = dht_read_raw(pin, b);
    if (mine == g_cl_gen && !fw_task_should_stop()) {
        for (int i = 0; i < 5; i++) g_cl_bytes[i] = b[i];
        g_cl_err = (uint8_t)e;
        g_cl_ready = 1;
    }
    g_cl_busy = 0;
    return 0;
}

static bool cl_start(unsigned pin) {
    if (g_cl_busy || g_cl_ready) return false;
    g_cl_req_gen = g_cl_gen;
    g_cl_busy = 1;
    // The same 2KB the BLE and contact workers ask for: what the PACKAGE does
    // on this stack is call ABI functions and set a flag.
    if (fw_task_spawn("novaclimate", cl_worker, (void *)(uintptr_t)pin, 2048) < 0) {
        g_cl_busy = 0;
        return false;
    }
    return true;
}
static void cl_disown(void) { g_cl_gen++; g_cl_ready = 0; }

class ClimateScreen : public Screen {
public:
    const char *title(void) const override { return "Climate"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT reads again.";
        out[1] = "Give it two seconds between";
        out[2] = "reads - the part needs it.";
        return 3;
    }

    void enter(void) override {
        cl_disown();
        phase_ = 0;
        started_ = false;
        have_ = false;
        note_[0] = 0;
    }
    void leave(void) override { cl_disown(); }

    bool animating(void) const override { return g_cl_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        bool changed = false;

        if (!started_) {
            started_ = true;
            int pin = board::pin(board::PIN_DHT);
            // No pin is answered by the catalogue gate before this screen is
            // ever reached on a stock board — DHT wired by default on both
            // profiles — but a pin can be cleared with the screen still open,
            // the same case ContactScreen defends against.
            if (pin != board::PIN_NONE) request((unsigned)pin);
            changed = true;
        }

        if (g_cl_ready) {
            g_cl_ready = 0;
            changed = true;
            DhtErr e = (DhtErr)g_cl_err;
            if (e == DHT_OK) {
                have_ = true;
                // DHT11 sends whole numbers; the "decimal" byte is usually
                // zero, but not always used as one, so it is added rather
                // than assumed to be nothing.
                hum_tenths_  = g_cl_bytes[0] * 10 + (g_cl_bytes[1] < 10 ? g_cl_bytes[1] : 0);
                temp_tenths_ = g_cl_bytes[2] * 10 + (g_cl_bytes[3] < 10 ? g_cl_bytes[3] : 0);
            } else {
                have_ = false;
                nova::copy(note_, sizeof(note_), dht_err_text(e));
            }
            g_cl_last_ms = fw_millis();
        }
        return changed || g_cl_busy;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;

        if (board::pin(board::PIN_DHT) == board::PIN_NONE) {
            c.text(2, y, "No pin is set.", 1);
            y += ui::ROWH;
            c.text(2, y, "Set one with", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "d1 pins set dht", 1, c.width() - 4, false);
            return;
        }

        char line[32];
        if (g_cl_busy) {
            c.text(2, y, "reading...", 1);
            c.spinner(c.width() - 9, y, phase_ / CL_SPIN_MS, 1);
            y += ui::ROWH;
        } else if (have_) {
            snprintf(line, sizeof(line), "%d.%d C", temp_tenths_ / 10, temp_tenths_ % 10);
            c.text(2, y, line, 1);
            y += ui::ROWH;
            snprintf(line, sizeof(line), "%d.%d %% RH", hum_tenths_ / 10, hum_tenths_ % 10);
            c.text(2, y, line, 1);
            y += ui::ROWH;
        } else {
            c.text(2, y, "No DHT11.", 1);
            y += ui::ROWH;
            if (note_[0]) {
                c.text_fit(2, y, note_, 1, c.width() - 4, false);
                y += ui::ROWH;
            }
        }

        char foot[26];
        status(foot, sizeof(foot));
        c.text_fit(2, c.height() - ui::FH, foot, 1, c.width() - 4, false);
    }

    Action on_event(Event e) override {
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            int pin = board::pin(board::PIN_DHT);
            if (pin != board::PIN_NONE && !g_cl_busy) {
                if (started_ && fw_millis() - g_cl_last_ms < CL_GAP_MS)
                    nova::copy(note_, sizeof(note_), "wait a moment, retry");
                else
                    request((unsigned)pin);
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    void request(unsigned pin) {
        have_ = false;
        note_[0] = 0;
        started_ = true;
        cl_start(pin);
    }
    void status(char *out, unsigned cap) const {
        if (g_cl_busy) nova::copy(out, cap, "reading...");
        else           nova::copy(out, cap, "Sel = read again");
    }

    unsigned phase_;
    bool     started_, have_;
    int      temp_tenths_, hum_tenths_;
    char     note_[28];
};

// --- GPS -----------------------------------------------------------------------
//
// Same reasoning as Climate, opposite conclusion: there is no firmware
// command for a GPS module either, but a UART is a real peripheral with its
// own clocked timing, not a bit-banged wire, so this reads it directly with
// nothing to get wrong from package code the way one-wire would.
//
// Presence here is a checksum that adds up, not a pin that answers back — a
// GPS module talks on its own, unasked, so a wired pin with nothing readable
// on it is exactly what an absent module looks like too. Only a genuine NMEA
// sentence, with its checksum correct, tells the two apart.
//
// DEVICE-UNCONFIRMED: the checksum and field parsing are proven against
// sentences this file builds itself; nothing here has listened to a real
// NEO-M8N.

// The GPIO -> UART peripheral mapping is a fixed property of the RP2040 and
// RP2350, not a board choice, so it is derived here with its own formula
// rather than reaching into novaboard for one — the same shape i2c_present()
// in novamodtab.cpp already uses for I2C. Matches novaboard.cpp's own
// uart_ctrl(), which the `d1 pins check` report is built on.
static unsigned gps_uart_bus(int gpio) { return (unsigned)(((gpio / 4) + 1) >> 1) & 1u; }

// NMEA 0183: every byte between '$' and '*' XORed together, printed as two
// upper-case hex digits after the '*'. Any sentence whose sum is right is a
// real GPS talking, whatever it is saying — that is the whole presence test.
static int nmea_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool nmea_checksum_ok(const char *s) {
    if (!s || s[0] != '$') return false;
    unsigned x = 0;
    const char *p = s + 1;
    for (; *p && *p != '*'; p++) x ^= (unsigned char)*p;
    if (*p != '*') return false;
    int hi = nmea_hex_nibble(p[1]), lo = nmea_hex_nibble(p[2]);
    if (hi < 0 || lo < 0) return false;
    return (unsigned)((hi << 4) | lo) == x;
}

// The Nth comma-separated token, counting the sentence id itself as token 0 —
// "GPGGA,150000,..." has the id at 0 and the fix quality at 6. `out` is
// emptied and false is returned when the sentence runs out first.
static bool nmea_field(const char *s, int n, char *out, unsigned cap) {
    const char *p = s + 1;             // past '$'
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

// What the last sentence amounted to, kept OUT of the screen instance — a
// free function needs somewhere to leave its answer, and a host test needs to
// read it back without a way into a private member. The same shape g_ct_state
// and g_msg_n already use for the same reason.
static bool g_gps_have;         // a checksum has ever added up
static bool g_gps_fixed;
static char g_gps_type[4];      // "GGA" / "RMC"
static char g_gps_sats[4];

class GpsScreen : public Screen {
public:
    const char *title(void) const override { return "GPS"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Needs sky. A fix can take";
        out[1] = "a minute or more, outdoors.";
        return 2;
    }

    void enter(void) override {
        line_n_ = 0;
        g_gps_have = false;
        g_gps_fixed = false;
        g_gps_type[0] = 0;
        g_gps_sats[0] = 0;
        int tx = board::pin(board::PIN_GPS_TX), rx = board::pin(board::PIN_GPS_RX);
        wired_ = (tx != board::PIN_NONE && rx != board::PIN_NONE);
        if (wired_) {
            bus_ = gps_uart_bus(rx);
            // NEO-M8N ships at 9600 baud until told otherwise; nothing here
            // reconfigures a module, only listens at its default rate.
            fw_uart_init(bus_, (unsigned)tx, (unsigned)rx, 9600);
        }
    }

    void leave(void) override { if (wired_) fw_uart_deinit(bus_); }

    bool tick(uint32_t) override {
        if (!wired_) return false;
        char chunk[64];
        int n = fw_uart_read(bus_, chunk, sizeof(chunk), 0);
        if (n <= 0) return false;
        bool changed = false;
        for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            if (ch == '\r' || ch == '\n') {
                if (line_n_ > 0) {
                    line_[line_n_] = 0;
                    if (consume(line_)) changed = true;
                    line_n_ = 0;
                }
                continue;
            }
            if (line_n_ + 1 < (int)sizeof(line_)) line_[line_n_++] = ch;
            else line_n_ = 0;          // a line too long to be real NMEA: resync
        }
        return changed;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;

        if (!wired_) {
            c.text(2, y, "No pin is set.", 1);
            y += ui::ROWH;
            c.text(2, y, "Set one with", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "d1 pins set gps_tx", 1, c.width() - 4, false);
            return;
        }

        if (!g_gps_have) {
            c.text(2, y, "No GPS.", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "Nothing readable on the UART.", 1, c.width() - 4, false);
            return;
        }

        char line[32];
        snprintf(line, sizeof(line), "%s answering", g_gps_type);
        c.text(2, y, line, 1);
        y += ui::ROWH;
        if (g_gps_fixed) snprintf(line, sizeof(line), "Fix%s%s", g_gps_sats[0] ? " - " : "", g_gps_sats);
        else             nova::copy(line, sizeof(line), "Searching for a fix...");
        c.text_fit(2, y, line, 1, c.width() - 4, false);
    }

private:
    bool consume(const char *s) {
        if (!nmea_checksum_ok(s)) return false;
        g_gps_have = true;

        // s[0] is '$', s[1..2] the talker (GP, GN, GL, ...), s[3..5] the type.
        char t[4] = { 0, 0, 0, 0 };
        if (strlen(s) >= 6) { t[0] = s[3]; t[1] = s[4]; t[2] = s[5]; }
        nova::copy(g_gps_type, sizeof(g_gps_type), t);

        // Two sentence types answer "is there a fix", which is the one thing
        // worth surfacing without decoding a position — see the file note on
        // why lat/lon stop here.
        if (!strcmp(t, "GGA")) {
            char fq[4];
            g_gps_fixed = nmea_field(s, 6, fq, sizeof(fq)) && fq[0] && fq[0] != '0';
            nmea_field(s, 7, g_gps_sats, sizeof(g_gps_sats));
        } else if (!strcmp(t, "RMC")) {
            char st[4];
            g_gps_fixed = nmea_field(s, 2, st, sizeof(st)) && st[0] == 'A';
        }
        return true;
    }

    bool     wired_;
    unsigned bus_;
    char     line_[96];
    int      line_n_;
};

// --- IR --------------------------------------------------------------------------
//
// A receiver like the VS1838B has no register to ask and no reply to parse —
// it only ever does one thing, which is toggle its output when it sees 38kHz
// carrier. So "is one fitted" cannot be asked, only seen, the same way a
// bench check of one would be a scope and no code at all. fw_gpio_watch and
// fw_gpio_events — the same edge counter the buttons are read through — is
// exactly that: a toggle count since it was last asked, coalesced in firmware
// so nothing here has to catch an interrupt or time a pulse.
//
// This deliberately does not decode anything. A real decode needs each edge's
// TIMESTAMP, and the ABI only ever hands back a coalesced COUNT — enough to
// prove a receiver is there and answering, not enough to read what a remote
// sent. Guessing at NEC or RC5 timing from a count alone would be exactly the
// unproven protocol work the house rule warns against, so it is not attempted.
//
// The honest state stays binary on purpose: a receiver that has never seen a
// signal and one that was never fitted look IDENTICAL from here, and the
// screen says so rather than picking one. Seeing an edge is proof; seeing
// none is not proof of the opposite — the asymmetry the LoRa screen does not
// have, because a chip register either answers or it does not, and this
// screen must not pretend to that certainty.
// Whether a signal has ever been seen, kept OUT of the screen instance for the
// same reason g_gps_have is: a host test needs to read it back and there is no
// way into a private member from outside one.
static bool g_ir_seen;

class IrScreen : public Screen {
public:
    const char *title(void) const override { return "IR"; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Point a remote at the";
        out[1] = "receiver and press a button.";
        return 2;
    }

    void enter(void) override {
        g_ir_seen = false;
        watching_ = false;
        pin_ = board::pin(board::PIN_IR_RX);
        if (pin_ != board::PIN_NONE) {
            fw_gpio_init((unsigned)pin_, FW_PIN_IN);
            watching_ = fw_gpio_watch((unsigned)pin_, FW_EDGE_BOTH) == 0;
        }
    }

    void leave(void) override {
        if (watching_) fw_gpio_watch((unsigned)pin_, 0);
        watching_ = false;
    }

    bool tick(uint32_t) override {
        if (!watching_) return false;
        int level = 1;
        int edges = fw_gpio_events((unsigned)pin_, &level);
        if (edges > 0 && !g_ir_seen) { g_ir_seen = true; return true; }
        return false;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;

        if (pin_ == board::PIN_NONE) {
            c.text(2, y, "No pin is set.", 1);
            y += ui::ROWH;
            c.text(2, y, "Set one with", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "d1 pins set ir_rx", 1, c.width() - 4, false);
            return;
        }

        if (!watching_) {
            c.text(2, y, "No IR receiver.", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "The GPIO would not arm.", 1, c.width() - 4, false);
            return;
        }

        if (g_ir_seen) {
            c.text(2, y, "Signal seen.", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "A receiver is answering.", 1, c.width() - 4, false);
        } else {
            c.text(2, y, "No signal yet.", 1);
            y += ui::ROWH;
            c.text_fit(2, y, "Point a remote and press", 1, c.width() - 4, false);
            y += ui::ROWH;
            c.text_fit(2, y, "a button.", 1, c.width() - 4, false);
        }
    }

private:
    int  pin_;
    bool watching_;
};

// --- the App table's entry points -----------------------------------------------

void open_battery(void) { gui::push<BatteryScreen>(); }
void open_climate(void) { gui::push<ClimateScreen>(); }
void open_gps(void)     { gui::push<GpsScreen>(); }
void open_ir(void)      { gui::push<IrScreen>(); }

}  // namespace screens
}  // namespace nova
