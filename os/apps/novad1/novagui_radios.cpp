// Desc: The SPI-radio screens — capture/replay a sub-GHz code, and a LoRa link test.
// File: novagui_radios.cpp
//
// Ported in spirit from v1's novagui_radios.py (SubGhzFireScreen, the code-list
// fire flow) and novalora.py's messaging, against an ABI that reaches the radios
// only through the firmware's shell commands. THERE IS NO RADIO CALL IN THE
// PACKAGE ABI: what exists is `subghz` and `lora` (see os/shell/cc1101.cpp and
// os/shell/sx1276.cpp), run through fw_shell_run, which hands back the text they
// printed. Everything here parses that text.
//
// The commands BLOCK — a capture or a receive listens for seconds — so they run
// on a worker task and tick() only ever looks at the result, the same shape and
// for the same reason as novagui_ble.cpp's scan. One worker, because there is one
// output capture in the OS and one SPI0 bus shared by both radios; two commands
// in flight would fight over both.
//
// The parse helpers are split out above RADIO_PARSE_ONLY with no UI or ABI
// dependency, so radio_test can prove them against the exact strings the firmware
// emits — a mis-parsed status or a dropped capture line is otherwise a silent
// failure on a screen nobody can unit-test on the host.
//
// .sub FILE INTEROP is not here (a separate agent owns file formats). A capture
// is the firmware's plain internal form — a comma-separated microsecond timing
// list — carried straight back to `subghz tx`. It would slot in at
// parse_subghz_capture (accept a .sub) and the replay path (emit one).

#include <string.h>
#include <stdio.h>
#include <stdint.h>
// Deliberately NO <stdlib.h>: a package links against the firmware's ABI symbol
// table, which does not export atoi (a package build fails loudly if it needs
// one), so the small integer parse below is used instead.

namespace nova {
namespace screens {
namespace radios {

// atoi, but without the libc dependency a package cannot satisfy. Skips leading
// spaces, takes an optional sign, reads digits. Enough for an RSSI, an SNR and a
// pulse count.
static int radios_atoi(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}

// ===========================================================================
// Parsing the shell text. Pure — string in, fields out — so radio_test exercises
// it against the same lines fakefw_d1.inc reproduces and the firmware emits.
// ===========================================================================

struct RadioStatus {
    bool present;
    char detail[24];        // "433.92 MHz" for sub-GHz, "SF7 BW125 CR4/5" for LoRa
};

struct LoraRx {
    // Enough of the payload for a screen summary and the host test; the mesh
    // agent has its own parser for a full frame. Kept small so it fits a local
    // and never pushes a screen over its 384-byte pool slot.
    char hex[64];
    int  rssi;
    int  snr;
};

// Copy [start,end) with the ends trimmed of spaces/CR into out.
static void copy_trim(char *out, unsigned cap, const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    unsigned n = 0;
    while (start < end && n + 1 < cap) out[n++] = *start++;
    if (cap) out[n] = 0;
}

// The value after `keyword` on the line that carries it: "  Freq    433.92 MHz"
// with keyword "Freq" gives "433.92 MHz". False (and out cleared) if absent.
static bool line_value(const char *text, const char *keyword, char *out, unsigned cap) {
    const char *p = strstr(text, keyword);
    if (!p) { if (cap) out[0] = 0; return false; }
    p += strlen(keyword);
    const char *end = strchr(p, '\n');
    if (!end) end = p + strlen(p);
    copy_trim(out, cap, p, end);
    return out[0] != 0;
}

// Present iff the "Chip" line says "present". Confined to that line so the word
// appearing elsewhere cannot flip it.
static bool text_has_present(const char *text) {
    const char *p = strstr(text, "Chip");
    const char *end;
    if (!p) { p = text; end = text + strlen(text); }
    else { end = strchr(p, '\n'); if (!end) end = p + strlen(p); }
    for (const char *q = p; q + 7 <= end; q++)
        if (!strncmp(q, "present", 7)) return true;
    return false;
}

void parse_subghz_status(const char *text, RadioStatus *out) {
    out->present = text_has_present(text);
    line_value(text, "Freq", out->detail, sizeof(out->detail));
}

void parse_lora_status(const char *text, RadioStatus *out) {
    out->present = text_has_present(text);
    line_value(text, "Modem", out->detail, sizeof(out->detail));
}

// The replayable timing line out of `subghz rx` output: the first line whose
// first non-space character is a digit and which contains a comma. That skips the
// tagged "[@] Captured ..." summary and never matches "[:] Nothing captured."
bool parse_subghz_capture(const char *text, char *out, unsigned cap) {
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        const char *s = p;
        while (s < end && (*s == ' ' || *s == '\t')) s++;
        if (s < end && *s >= '0' && *s <= '9') {
            bool comma = false;
            for (const char *q = s; q < end; q++) if (*q == ',') { comma = true; break; }
            if (comma) { copy_trim(out, cap, s, end); return true; }
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (cap) out[0] = 0;
    return false;
}

// The pulse count from `subghz rx`'s "[@] Captured N pulses ..." summary. 0 when
// nothing was captured ("[:] Nothing captured." has no capital-C "Captured "). The
// screen replays via `subghz tx last`, so it needs the count, not the whole list —
// which would not fit its 384-byte pool slot anyway.
int parse_subghz_pulses(const char *text) {
    const char *p = strstr(text, "Captured ");
    return p ? radios_atoi(p + 9) : 0;
}

// "[@] RX <hex> rssi <n> snr <n>" -> fields. False on "Nothing received." The
// mesh agent parses the same line from its own worker; this is the screen's copy.
bool parse_lora_recv(const char *text, LoraRx *out) {
    if (strstr(text, "Nothing received")) return false;
    const char *rx = strstr(text, "RX ");
    if (!rx) return false;
    rx += 3;
    unsigned n = 0;
    while (rx[n] && rx[n] != ' ' && rx[n] != '\n' && n + 1 < sizeof(out->hex)) {
        out->hex[n] = rx[n];
        n++;
    }
    out->hex[n] = 0;
    const char *r = strstr(rx, "rssi");
    out->rssi = r ? radios_atoi(r + 4) : 0;
    const char *s = strstr(rx, "snr");
    out->snr = s ? radios_atoi(s + 3) : 0;
    return true;
}

// Text <-> hex. `lora send`/`lora recv` carry the payload as hex bytes and a
// person types words, so the Messages screen turns one into the other. Pure, so
// the round trip is host-tested against the exact strings the firmware prints.
bool text_to_hex(const char *text, char *out, unsigned cap) {
    static const char *const d = "0123456789abcdef";
    unsigned n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (n + 3 > cap) { if (cap) out[0] = 0; return false; }   // two chars + NUL
        out[n++] = d[*p >> 4];
        out[n++] = d[*p & 0x0f];
    }
    if (!cap) return false;
    out[n] = 0;
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Hex -> text. Returns the text length, or -1 on an odd or non-hex string. A byte
// outside printable ASCII becomes '.', so a binary mesh frame shown on the panel
// is legible rather than a scatter of control codes that could move the cursor.
int hex_to_text(const char *hex, char *out, unsigned cap) {
    unsigned n = 0;
    while (hex[0] && hex[1]) {
        int hi = hex_nibble(hex[0]), lo = hex_nibble(hex[1]);
        if (hi < 0 || lo < 0) return -1;
        if (n + 1 >= cap) break;                         // full — a summary, not the frame
        int ch = (hi << 4) | lo;
        out[n++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
        hex += 2;
    }
    if (hex[0] && !hex[1]) return -1;                    // a dangling nibble is malformed
    if (cap) out[n] = 0;
    return (int)n;
}

}  // namespace radios
}  // namespace screens
}  // namespace nova

#ifndef RADIO_PARSE_ONLY

// ===========================================================================
// The screens. Everything below needs the UI and the ABI.
// ===========================================================================

#include "novagui_radios.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"
#include "novanotify.h"

#include "rpc_app.h"

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;
using radios::RadioStatus;
using radios::LoraRx;

// One step of a spinner, the 140 ms every wheel on the device turns at.
#define RF_SPIN_MS 140

// --- the shared worker --------------------------------------------------------
//
// One command in flight at a time. There is one output capture and one SPI0 bus
// shared by the CC1101, the SX1276 and the SD card, so a second command started
// while one is running would get a refused capture and could collide on the bus.
// The generation counter disowns a reply whose screen has since changed, exactly
// as the BLE scan does; the buffer is file-static because a screen has 384 bytes
// of pool and a capture line does not fit there even once.
static char              g_rf_out[2048];    // capture buffer (a timing list is long)
static char              g_rf_line[160];    // the command, built before the spawn
static volatile uint32_t g_rf_gen;
static volatile uint32_t g_rf_req_gen;
static volatile uint8_t  g_rf_busy;
static volatile uint8_t  g_rf_ready;
static volatile int      g_rf_rc;

static int rf_task(void *arg) {
    (void)arg;
    uint32_t mine = g_rf_req_gen;
    int rc = fw_shell_run(g_rf_line, g_rf_out, sizeof(g_rf_out));
    if (mine == g_rf_gen && !fw_task_should_stop()) {
        g_rf_rc = rc;
        g_rf_ready = 1;
    } else {
        g_rf_out[0] = 0;
    }
    g_rf_busy = 0;
    return 0;
}

// Fire a shell line on the worker. False if one is already in flight (the caller
// retries on a later tick, or shows why nothing happened).
static bool rf_run(const char *line) {
    if (g_rf_busy || g_rf_ready) return false;
    snprintf(g_rf_line, sizeof(g_rf_line), "%s", line);
    g_rf_out[0] = 0;
    g_rf_req_gen = g_rf_gen;
    g_rf_busy = 1;
    if (fw_task_spawn("novarf", rf_task, nullptr, 2048) < 0) {
        g_rf_busy = 0;
        return false;
    }
    return true;
}

// Ask again for something that was refused rather than answered.
//
// THE BUG THIS CLOSES. Both screens ask for `status` from enter(), and rf_run
// refuses while a worker is still winding down — rf_disown() bumps the
// generation and clears g_rf_ready but leaves g_rf_busy set, so the previous
// screen's reply lands nowhere and clears the flag without ever setting
// g_rf_ready. Nothing then arrives, nothing retried, and the panel read
// "CC1101: checking..." for ever, with no spinner and no way to make it ask
// again short of leaving and coming back — on a device whose radio was fine.
// It happened on any second visit that came fast enough, and on opening LoRa
// while a Sub-GHz capture was still out.
//
// So the request is re-asked until it is actually made, and gives up out loud
// rather than silently. Three tries at a frame apiece is a few hundred
// milliseconds, which is longer than a disowned worker takes to finish and
// short enough that nobody waits on a bus that is genuinely stuck.
#define RF_TRIES 3

static bool rf_retry(uint8_t *tries, const char *line) {
    if (g_rf_busy || g_rf_ready) return false;      // still in flight; wait
    if (*tries >= RF_TRIES) return false;
    (*tries)++;
    return rf_run(line);
}

// A finished reply is waiting; consume it. False when nothing is ready.
static bool rf_reap(void) {
    if (!g_rf_ready) return false;
    g_rf_ready = 0;
    return true;
}

// Disown anything in flight when the top screen changes, both on the way in and
// out, so a reply never lands in a screen that did not ask for it.
static void rf_disown(void) { g_rf_gen++; g_rf_ready = 0; }

// The button row both screens draw their two actions with — the same idiom the
// BLE Device screen uses.
static void draw_actions(Canvas &c, int y, const char *const *labels, int n, int sel) {
    int x = 2;
    for (int i = 0; i < n; i++) {
        const int w = c.text_width(labels[i], 1, true) + 6;
        if (i == sel) c.rounded_rect(x, y - 1, w, ui::ROWH, 1, true);
        c.text(x + 3, y, labels[i], i == sel ? 0 : 1, 1, true);
        x += w + 3;
    }
}

// --- Sub-GHz ------------------------------------------------------------------
//
// Check the CC1101 is there, capture one fixed-code OOK burst, and replay it —
// the "capture a garage remote" flow, which in v1 was a code list plus
// SubGhzFireScreen. Reduced to the two live actions here: a capture keeps the
// last burst in memory (saving to a file is where .sub interop will go), and
// replay fires it back.
//
// DEVICE-UNCONFIRMED end to end: no CC1101 has answered. The screen logic and the
// parsing are host-tested; whether a real remote is captured and a real receiver
// hears the replay is the hardware check in os/shell/cc1101.cpp.
class SubGhzScreen : public Screen {
public:
    const char *title(void) const override { return "Sub-GHz"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Capture holds one OOK burst.";
        out[1] = "Replay sends it back.";
        out[2] = "Fixed codes only.";
        return 3;
    }

    void enter(void) override {
        rf_disown();
        sel_ = 0; phase_ = 0;
        have_status_ = false; st_.present = false; st_.detail[0] = 0;
        have_capture_ = false; pulses_ = 0; note_[0] = 0;
        pending_ = P_STATUS;
        tries_ = 0;
        if (rf_run("subghz status")) tries_ = 1;
    }

    void leave(void) override { rf_disown(); }

    bool animating(void) const override { return g_rf_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        bool changed = false;
        // The status request, until it has actually been made — see rf_retry.
        if (pending_ == P_STATUS && !tries_) {
            if (!rf_retry(&tries_, "subghz status") && tries_ >= RF_TRIES) {
                pending_ = P_IDLE;
                nova::copy(note_, sizeof(note_), "radio busy - reopen");
                changed = true;
            }
        }
        if (rf_reap()) {
            changed = true;
            if (pending_ == P_STATUS) {
                // A held output capture returns an empty buffer, and a guest
                // session gets a refusal (subghz is admin) — neither means the
                // chip is absent, and mustn't render as "check your wiring".
                if (!g_rf_out[0])      nova::copy(note_, sizeof(note_), "no result - busy");
                else if (g_rf_rc != 0) nova::copy(note_, sizeof(note_), "refused");
                else { radios::parse_subghz_status(g_rf_out, &st_); have_status_ = true; }
            } else if (pending_ == P_RX) {
                pulses_ = radios::parse_subghz_pulses(g_rf_out);
                if (pulses_ > 0) {
                    have_capture_ = true;
                    snprintf(note_, sizeof(note_), "Captured %d pulses", pulses_);
                } else {
                    nova::copy(note_, sizeof(note_), "Nothing captured");
                }
            } else if (pending_ == P_TX) {
                nova::copy(note_, sizeof(note_),
                           strstr(g_rf_out, "Sent") ? "Sent" : "TX failed");
            }
            pending_ = P_IDLE;
        }
        return changed || g_rf_busy;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;
        char line[40];

        // Three states, not two. "checking" is only honest while something is
        // actually being asked; a screen that gave up has to say it does not
        // know rather than go on claiming to be busy.
        if (st_.present && have_status_) snprintf(line, sizeof(line), "CC1101 %s", st_.detail);
        else if (have_status_)           nova::copy(line, sizeof(line), "CC1101: absent");
        else if (pending_ == P_STATUS)   nova::copy(line, sizeof(line), "CC1101: checking...");
        else                             nova::copy(line, sizeof(line), "CC1101: unknown");
        c.text_fit(2, y, line, 1, c.width() - 12, false);
        if (g_rf_busy) c.spinner(c.width() - 9, y, phase_ / RF_SPIN_MS, 1);
        y += ui::ROWH;

        if (note_[0])            c.text_fit(2, y, note_, 1, c.width() - 4, false);
        else if (have_capture_) { snprintf(line, sizeof(line), "Captured %d pulses", pulses_);
                                 c.text(2, y, line, 1); }
        else                     c.text(2, y, "No capture yet", 1);
        y += ui::ROWH + 2;

        static const char *const kAct[2] = { "Capture", "Replay" };
        draw_actions(c, y, kAct, 2, sel_);

        c.text_fit(2, c.height() - ui::FH,
                   (have_status_ && !st_.present) ? "check CC1101 wiring" : "SELECT to run",
                   1, c.width() - 4, false);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW || e == EV_ROT_CCW) { sel_ ^= 1; return ui::ACT_STAY; }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            if (sel_ == 0) {
                if (rf_run("subghz rx 5")) { pending_ = P_RX; nova::copy(note_, sizeof(note_), "capturing..."); }
                else nova::copy(note_, sizeof(note_), "busy");
            } else {
                // The firmware still holds the last capture, so replay never has to
                // carry the timing list back through the command line (it would not
                // fit this screen's pool slot or the shell line).
                if (!have_capture_) nova::copy(note_, sizeof(note_), "nothing to replay");
                else if (rf_run("subghz tx last")) {
                    pending_ = P_TX;
                    nova::copy(note_, sizeof(note_), "sending...");
                }
                else nova::copy(note_, sizeof(note_), "busy");
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    enum { P_IDLE, P_STATUS, P_RX, P_TX };
    int          sel_;
    unsigned     phase_;
    RadioStatus  st_;
    bool         have_status_;
    bool         have_capture_;       // firmware holds the burst; replay via "tx last"
    int          pulses_;
    char         note_[24];
    int          pending_;
    uint8_t      tries_;          // status requests actually made, for rf_retry
};

// --- LoRa ---------------------------------------------------------------------
//
// A link test, not the messenger: is the SX1276 there, what is it set to, send a
// known test packet, and listen once. The Messages screen and the mesh live
// elsewhere and own the `lora` text contract this leans on.
//
// DEVICE-UNCONFIRMED: real comms need two boards. Send a test packet from one and
// Listen on the other; the receiver should show the same bytes with an RSSI and
// SNR. The parsing is host-tested; the radio is not.
class LoRaScreen : public Screen {
public:
    const char *title(void) const override { return "LoRa"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Send fires a test packet.";
        out[1] = "Listen waits for one.";
        out[2] = "Needs a second board.";
        return 3;
    }

    void enter(void) override {
        rf_disown();
        sel_ = 0; phase_ = 0;
        have_status_ = false; st_.present = false; st_.detail[0] = 0;
        note_[0] = 0;
        pending_ = P_STATUS;
        tries_ = 0;
        if (rf_run("lora status")) tries_ = 1;
    }

    void leave(void) override { rf_disown(); }

    bool animating(void) const override { return g_rf_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        bool changed = false;
        if (pending_ == P_STATUS && !tries_) {
            if (!rf_retry(&tries_, "lora status") && tries_ >= RF_TRIES) {
                pending_ = P_IDLE;
                nova::copy(note_, sizeof(note_), "radio busy - reopen");
                changed = true;
            }
        }
        if (rf_reap()) {
            changed = true;
            if (pending_ == P_STATUS) {
                // As above: busy/refused is not "no SX1276".
                if (!g_rf_out[0])      nova::copy(note_, sizeof(note_), "no result - busy");
                else if (g_rf_rc != 0) nova::copy(note_, sizeof(note_), "refused");
                else { radios::parse_lora_status(g_rf_out, &st_); have_status_ = true; }
            } else if (pending_ == P_SEND) {
                nova::copy(note_, sizeof(note_),
                           strstr(g_rf_out, "Sent") ? "Sent test packet" : "send failed");
            } else if (pending_ == P_RECV) {
                LoraRx rx;
                if (radios::parse_lora_recv(g_rf_out, &rx))
                    // Bounded hex width: a summary, and it keeps the line inside note_.
                    snprintf(note_, sizeof(note_), "RX %.16s %ddBm", rx.hex, rx.rssi);
                else
                    nova::copy(note_, sizeof(note_), "Nothing received");
            }
            pending_ = P_IDLE;
        }
        return changed || g_rf_busy;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;
        char line[40];

        if (have_status_ && st_.present) nova::copy(line, sizeof(line), "SX1276: present");
        else if (have_status_)           nova::copy(line, sizeof(line), "SX1276: absent");
        else if (pending_ == P_STATUS)   nova::copy(line, sizeof(line), "SX1276: checking...");
        else                             nova::copy(line, sizeof(line), "SX1276: unknown");
        c.text_fit(2, y, line, 1, c.width() - 12, false);
        if (g_rf_busy) c.spinner(c.width() - 9, y, phase_ / RF_SPIN_MS, 1);
        y += ui::ROWH;

        if (st_.present && st_.detail[0]) c.text_fit(2, y, st_.detail, 1, c.width() - 4, false);
        y += ui::ROWH;

        if (note_[0]) c.text_fit(2, y, note_, 1, c.width() - 4, false);
        y += ui::ROWH + 1;

        static const char *const kAct[2] = { "Send", "Listen" };
        draw_actions(c, y, kAct, 2, sel_);

        c.text_fit(2, c.height() - ui::FH,
                   (have_status_ && !st_.present) ? "check SX1276 wiring" : "SELECT to run",
                   1, c.width() - 4, false);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW || e == EV_ROT_CCW) { sel_ ^= 1; return ui::ACT_STAY; }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            if (sel_ == 0) {
                // "Hello" — a fixed, recognisable test payload.
                if (rf_run("lora send 48656c6c6f")) {
                    pending_ = P_SEND;
                    nova::copy(note_, sizeof(note_), "sending...");
                }
                else nova::copy(note_, sizeof(note_), "busy");
            } else {
                if (rf_run("lora recv 5")) {
                    pending_ = P_RECV;
                    nova::copy(note_, sizeof(note_), "listening...");
                }
                else nova::copy(note_, sizeof(note_), "busy");
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    enum { P_IDLE, P_STATUS, P_SEND, P_RECV };
    int          sel_;
    unsigned     phase_;
    RadioStatus  st_;
    bool         have_status_;
    char         note_[40];
    int          pending_;
    uint8_t      tries_;
};

// --- Messages -----------------------------------------------------------------
//
// The messenger v1 had in novalora.py, on the same `lora` text contract the LoRa
// link test leans on: type or pick a short line and send it, listen for one, and
// keep the last few either way. It is a package, so a message is hex over `lora
// send`/`lora recv`, and the screen turns words into bytes and back.
//
// Every received line goes through the notification path (msg_receive), so one
// that arrives is seen from any screen — the same toast the rest of the device
// uses. That is the background-receive path task #36 asks for; the always-on
// LISTENER that would drive it unattended is a background service, and left to the
// device pass because it needs the chip to prove and would fight the one shared
// SPI worker in the meantime. Here Listen drives one receive at a time.
//
// DEVICE-UNCONFIRMED end to end: both radios read absent on the bench board, and
// real RF needs a second SX1276. The screen logic, the hex round trip and the
// status/receive parse are host-tested; the air is not.

// The recent messages, kept OUT of the 384-byte pool slot so they survive the
// screen closing and a receive can append from anywhere. Newest at head.
#define MSG_MAX      8
#define MSG_TEXT_MAX 22
struct MsgItem { char text[MSG_TEXT_MAX]; int rssi; bool rx; };
static MsgItem  g_msg[MSG_MAX];
static int      g_msg_n;
static unsigned g_msg_head;

static void msg_store(const char *text, bool rx, int rssi) {
    MsgItem &m = g_msg[g_msg_head];
    nova::copy(m.text, sizeof(m.text), text);
    m.rx = rx; m.rssi = rssi;
    g_msg_head = (g_msg_head + 1) % MSG_MAX;
    if (g_msg_n < MSG_MAX) g_msg_n++;
}
// Newest first: i = 0 is the most recent.
static const MsgItem *msg_at(int i) {
    if (i < 0 || i >= g_msg_n) return nullptr;
    return &g_msg[(g_msg_head + MSG_MAX - 1 - (unsigned)i) % MSG_MAX];
}

// The one receive path — decode, keep, and RAISE A NOTIFICATION. A background
// listener would call exactly this, which is the whole point of routing it here:
// a message lands the same way whether the screen is up or not, and the toast
// carries it to wherever you are.
static void msg_receive(const char *hex, int rssi) {
    char txt[MSG_TEXT_MAX];
    if (radios::hex_to_text(hex, txt, sizeof(txt)) < 0) return;
    msg_store(txt, true, rssi);
    char banner[48];
    snprintf(banner, sizeof(banner), "LoRa: %s", txt);
    nova::notify::post(banner);
}

// The quick lines, so most messages are two presses and no keyboard.
static const char *const kCanned[] = { "Hello", "On my way", "ACK", "SOS" };
#define MSG_CANNED_N 4

// A send queued from the compose keyboard (whose text does not outlive the
// callback) or from a canned row, fired by tick() once the shared bus is free.
static char g_msg_compose[MSG_TEXT_MAX];
static char g_msg_last_sent[MSG_TEXT_MAX];
static bool g_msg_send_pending;

static void msg_typed(void *, const char *text) {
    nova::copy(g_msg_compose, sizeof(g_msg_compose), text);
    g_msg_send_pending = (g_msg_compose[0] != 0);
}

class MsgScreen : public Screen {
public:
    const char *title(void) const override { return "Messages"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Send picks a line or types one.";
        out[1] = "Listen waits for a message.";
        out[2] = "Needs a second SX1276.";
        return 3;
    }

    void enter(void) override {
        rf_disown();
        phase_ = 0;
        mode_ = MODE_MAIN;
        // First open checks the chip; coming back from the keyboard or a notice
        // must NOT re-ask, or a queued send is thrown away and the cursor jumps.
        if (!started_) {
            started_ = true;
            sel_ = 0; pick_ = 0;
            have_status_ = false; present_ = false; detail_[0] = 0; note_[0] = 0;
            pending_ = P_STATUS; tries_ = 0;
            if (rf_run("lora status")) tries_ = 1;
        }
    }

    void leave(void) override { rf_disown(); }

    bool animating(void) const override { return g_rf_busy != 0; }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        bool changed = false;

        // A queued send, fired once the shared worker is free.
        if (g_msg_send_pending && !g_rf_busy && !g_rf_ready && pending_ == P_IDLE) {
            char hex[2 * MSG_TEXT_MAX + 1];
            if (!radios::text_to_hex(g_msg_compose, hex, sizeof(hex))) {
                nova::copy(note_, sizeof(note_), "too long");
            } else {
                char line[64];
                snprintf(line, sizeof(line), "lora send %s", hex);
                if (rf_run(line)) {
                    nova::copy(g_msg_last_sent, sizeof(g_msg_last_sent), g_msg_compose);
                    pending_ = P_SEND;
                    nova::copy(note_, sizeof(note_), "sending...");
                }
            }
            g_msg_send_pending = false;
            changed = true;
        }

        // The status request, until it has actually been made (see rf_retry).
        if (pending_ == P_STATUS && !tries_) {
            if (!rf_retry(&tries_, "lora status") && tries_ >= RF_TRIES) {
                pending_ = P_IDLE;
                nova::copy(note_, sizeof(note_), "radio busy - reopen");
                changed = true;
            }
        }

        if (rf_reap()) {
            changed = true;
            if (pending_ == P_STATUS) {
                // A held capture returns empty and a guest gets a refusal; neither
                // is "no chip", and neither may render as "needs an SX1276".
                if (!g_rf_out[0])      nova::copy(note_, sizeof(note_), "no result - busy");
                else if (g_rf_rc != 0) nova::copy(note_, sizeof(note_), "refused");
                else {
                    RadioStatus st;
                    radios::parse_lora_status(g_rf_out, &st);
                    present_ = st.present;
                    nova::copy(detail_, sizeof(detail_), st.detail);
                    have_status_ = true;
                }
            } else if (pending_ == P_SEND) {
                if (strstr(g_rf_out, "Sent")) {
                    msg_store(g_msg_last_sent, false, 0);
                    nova::copy(note_, sizeof(note_), "sent");
                } else {
                    nova::copy(note_, sizeof(note_), "send failed");
                }
            } else if (pending_ == P_RECV) {
                LoraRx rx;
                if (radios::parse_lora_recv(g_rf_out, &rx)) {
                    msg_receive(rx.hex, rx.rssi);
                    char txt[MSG_TEXT_MAX];
                    radios::hex_to_text(rx.hex, txt, sizeof(txt));
                    snprintf(note_, sizeof(note_), "RX %.12s %ddBm", txt, rx.rssi);
                } else {
                    nova::copy(note_, sizeof(note_), "nothing received");
                }
            }
            pending_ = P_IDLE;
        }
        return changed || g_rf_busy;
    }

    void draw(Canvas &c) override {
        if (mode_ == MODE_PICK) { draw_pick(c); return; }

        int y = ui::TOP;
        char line[40];

        // The chip, said plainly. "needs an SX1276" is the honest state on a board
        // without one, and it is NOT a dead button — SELECT below says the same.
        if (have_status_ && present_)      snprintf(line, sizeof(line), "SX1276 %s", detail_);
        else if (have_status_)             nova::copy(line, sizeof(line), "No SX1276 - needs one");
        else if (pending_ == P_STATUS)     nova::copy(line, sizeof(line), "SX1276: checking...");
        else                               nova::copy(line, sizeof(line), "SX1276: unknown");
        c.text_fit(2, y, line, 1, c.width() - 12, false);
        if (g_rf_busy) c.spinner(c.width() - 9, y, phase_ / RF_SPIN_MS, 1);
        y += ui::ROWH;

        // The two most recent messages, newest first.
        for (int i = 0; i < 2; i++) {
            const MsgItem *m = msg_at(i);
            if (m) {
                if (m->rx) snprintf(line, sizeof(line), "< %.12s %ddBm", m->text, m->rssi);
                else       snprintf(line, sizeof(line), "> %.16s", m->text);
            } else if (i == 0) {
                nova::copy(line, sizeof(line), "no messages yet");
            } else line[0] = 0;
            if (line[0]) c.text_fit(2, y, line, 1, c.width() - 4, false);
            y += ui::ROWH;
        }

        if (note_[0]) c.text_fit(2, y, note_, 1, c.width() - 4, false);
        y += ui::ROWH;

        static const char *const kAct[2] = { "Send", "Listen" };
        draw_actions(c, y, kAct, 2, sel_);
    }

    Action on_event(Event e) override {
        if (mode_ == MODE_PICK) return pick_event(e);

        if (e == EV_ROT_CW || e == EV_ROT_CCW) { sel_ ^= 1; return ui::ACT_STAY; }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            if (have_status_ && !present_) {
                ui::notice("Messages", "This needs an SX1276. Fit one and set its "
                                       "pins in System, Hardware.");
                return ui::ACT_STAY;
            }
            if (sel_ == 0) { mode_ = MODE_PICK; pick_ = 0; }        // Send
            else {                                                  // Listen
                if (rf_run("lora recv 10")) {
                    pending_ = P_RECV;
                    nova::copy(note_, sizeof(note_), "listening...");
                } else nova::copy(note_, sizeof(note_), "busy");
            }
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    enum { MODE_MAIN, MODE_PICK };
    enum { P_IDLE, P_STATUS, P_SEND, P_RECV };

    void draw_pick(Canvas &c) {
        c.text(2, ui::TOP, "Send which?", 1);
        const int rows = ui::rows_for(c) - 1;
        int top = pick_ - (rows - 1);
        if (top < 0) top = 0;
        for (int i = 0; i < rows; i++) {
            int idx = top + i;
            if (idx > MSG_CANNED_N) break;                 // 0 = Compose, 1..N = canned
            int y = ui::TOP + ui::ROWH + i * ui::ROWH;
            bool on = (idx == pick_);
            if (on) c.rounded_rect(0, y - 1, c.width(), ui::ROWH, 1, true);
            const char *label = idx == 0 ? "Compose..." : kCanned[idx - 1];
            c.text(3, y, label, on ? 0 : 1);
        }
    }

    Action pick_event(Event e) {
        const int n = MSG_CANNED_N + 1;                    // Compose + canned
        if (e == EV_ROT_CW)  { pick_ = (pick_ + 1) % n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { pick_ = (pick_ + n - 1) % n; return ui::ACT_STAY; }
        if (e == EV_BACK)    { mode_ = MODE_MAIN; return ui::ACT_STAY; }
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            if (pick_ == 0) {
                ui::keyboard("Message", "", false, msg_typed, nullptr, this);
            } else {
                nova::copy(g_msg_compose, sizeof(g_msg_compose), kCanned[pick_ - 1]);
                g_msg_send_pending = true;
            }
            mode_ = MODE_MAIN;
            return ui::ACT_STAY;
        }
        return ui::ACT_STAY;                               // BACK is the only way up
    }

    int          sel_;        // Send / Listen
    int          pick_;       // row in the send picker
    unsigned     phase_;
    char         detail_[24]; // "SF7 BW125 CR4/5"
    bool         present_, have_status_, started_;
    char         note_[40];
    int          pending_;
    int          mode_;
    uint8_t      tries_;
};

// --- the App table's entry points ---------------------------------------------

void open_subghz(void)   { gui::push<SubGhzScreen>(); }
void open_lora(void)     { gui::push<LoRaScreen>(); }
void open_messages(void) { gui::push<MsgScreen>(); }

}  // namespace screens
}  // namespace nova

#endif  // RADIO_PARSE_ONLY
