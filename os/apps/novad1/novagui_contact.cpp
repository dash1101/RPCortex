// Desc: The contact readers — tap an NFC tag, touch an iButton key.
// File: novagui_contact.cpp
//
// Ported from novagui_radios.py's NFCScreen, and the same screen again for a
// part v1 never had a driver for.
//
// THERE IS NO NFC OR 1-WIRE CALL IN THE PACKAGE ABI, and there should not be:
// both readers need a bus, a handshake timed in microseconds and a frame
// parser, none of which belongs on the far side of a sandbox boundary. What
// exists is the firmware's own shell commands, run through fw_shell_run:
//
//     nfc read <seconds> <sda> <scl>     poll for an ISO14443-A tag
//     ibutton read <gpio> <seconds>      wait for a DS1990A key
//
// and the text they print back. Two things follow, and both shaped these
// screens the way novagui_ble was shaped:
//
//   * A READ BLOCKS FOR AS LONG AS IT WAS ASKED TO. That is the whole point of
//     the command — it sits there waiting for somebody to present something —
//     so calling it from tick() would be a frozen panel for the entire read.
//     It runs on a worker and tick() only ever looks at the result. Same
//     pattern as the BLE scan, same reason, and the generation counter is
//     there for the same reason too: a read outliving its screen must throw
//     its answer away rather than drop it into the next screen's fields.
//   * WHAT ARRIVES IS TEXT. The commands print a UID, a type, a ROM, a family;
//     the raw card memory never leaves the firmware. So v1's NfcSaveScreen —
//     the sector-by-sector Mifare dump written out as a Flipper .nfc file — has
//     nothing to dump and is not ported. That is a capability lost rather than
//     a screen rewritten, and it needs InDataExchange in the firmware driver
//     before it can come back.
//
// The feel is v1's NFCScreen kept deliberately: a heading, "tap a tag..." while
// it waits, and the type above the UID when one arrives. What is different is
// that v1 polled every 400 ms from tick() and could show a tag the instant it
// landed; here one call covers several seconds and the answer arrives at the
// end of it. So the waiting state says how long it has been waiting, which v1
// never had to.
#include "novagui_contact.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"
#include "novaboard.h"
#include "novanotify.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// The same 140 ms every other wheel on the device turns at.
#define CT_SPIN_MS 140

// How long one press asks the reader to wait. Long enough that somebody has
// time to get a card out of a wallet, short enough that BACK is never more than
// this away from being noticed — the command cannot be interrupted from here,
// so this number IS the worst case for leaving the screen.
#define CT_READ_SECS 5

// --- the worker ------------------------------------------------------------------
//
// One at a time, shared by both screens. Not a tidiness preference: there is
// ONE output capture in the OS, so two reads at once would mean the second gets
// a refused capture and an empty buffer — which reads as "nothing was
// presented" and would be believed.
//
// A generation counter rather than a cancel flag, for the reason the BLE scan
// gives: THE READ CANNOT BE INTERRUPTED. Neither command breaks out of its wait
// for anything a screen can send, so for the length of a read there is nowhere
// for a flag to be looked at. What leaving the screen can do is make the answer
// irrelevant, and that is what this does.
//
// Plain volatile is enough for the handshake. These cores have no data cache
// over SRAM, so a write on one is visible to the other, and the ordering that
// matters is that g_ct_busy clears LAST.

static char              g_ct_out[512];     // the capture buffer, file-static
static char              g_ct_line[48];     // the command, built before the spawn
static volatile uint32_t g_ct_gen;
static volatile uint32_t g_ct_req_gen;
static volatile uint8_t  g_ct_busy;
static volatile uint8_t  g_ct_ready;
static volatile int      g_ct_rc;
static uint32_t          g_ct_start_ms;

static int ct_worker(void *arg) {
    (void)arg;
    const uint32_t mine = g_ct_req_gen;
    const int rc = fw_shell_run(g_ct_line, g_ct_out, sizeof(g_ct_out));
    if (mine == g_ct_gen && !fw_task_should_stop()) {
        g_ct_rc = rc;
        g_ct_ready = 1;
    } else {
        g_ct_out[0] = 0;                    // nobody is waiting for this any more
    }
    g_ct_busy = 0;
    return 0;
}

// What the last read amounted to.
enum CtState { CT_NEVER = 0, CT_OK, CT_NOTHING, CT_FAILED, CT_BUSY_BUF, CT_NOPIN };

static uint8_t g_ct_state;

// Three lines of answer and one of explanation. Held here rather than in a
// screen because a screen has 384 bytes of pool and this would not fit in it
// once, let alone twice.
static char g_ct_l1[26];
static char g_ct_l2[26];
static char g_ct_l3[26];
static char g_ct_why[26];

static void ct_clear(void) {
    g_ct_l1[0] = g_ct_l2[0] = g_ct_l3[0] = g_ct_why[0] = 0;
}

static bool ct_start(const char *line) {
    if (g_ct_busy || g_ct_ready) return false;
    nova::copy(g_ct_line, sizeof(g_ct_line), line);
    g_ct_out[0] = 0;
    g_ct_req_gen = g_ct_gen;
    g_ct_start_ms = fw_millis();
    g_ct_busy = 1;
    // The same 2 KB the BLE worker asks for, and for the same reason: what the
    // PACKAGE does on this stack is call one ABI function and set a flag. The
    // shell and the driver run on it too, and the loader's separate firmware
    // reserve is what carries them.
    if (fw_task_spawn("novaread", ct_worker, nullptr, 2048) < 0) {
        g_ct_busy = 0;
        g_ct_state = CT_FAILED;
        nova::copy(g_ct_why, sizeof(g_ct_why), "no task free");
        return false;
    }
    return true;
}

static void ct_disown(void) { g_ct_gen++; g_ct_ready = 0; }

// --- reading the answer back --------------------------------------------------
//
// Both commands print the same shape, which is why one parser does both: a
// tagged heading line, then indented rows of "<Name>  <value>". A row is found
// by its NAME rather than by its position, so a line added to either command
// does not silently shift what these screens read — the failure that would
// produce is a UID displayed in the Type row, which looks like a bug in the
// reader rather than in here.

static bool ct_field(const char *text, const char *name, char *out, unsigned cap) {
    const unsigned nlen = (unsigned)strlen(name);
    for (const char *p = text; p && *p; ) {
        const char *nl = strchr(p, '\n');
        const char *q = p;
        while (*q == ' ') q++;
        if (!strncmp(q, name, nlen) && (q[nlen] == ' ' || q[nlen] == '\t')) {
            q += nlen;
            while (*q == ' ' || *q == '\t') q++;
            unsigned n = nl ? (unsigned)(nl - q) : (unsigned)strlen(q);
            if (n >= cap) n = cap - 1;
            memcpy(out, q, n);
            out[n] = 0;
            // The capture strips colour but not the carriage return, and a
            // trailing one turns into a stray glyph at the end of a UID.
            while (n && (out[n - 1] == ' ' || out[n - 1] == '\r')) out[--n] = 0;
            return n > 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (cap) out[0] = 0;
    return false;
}

// Take in what the worker produced. Returns true when something changed.
//
// `first` and `second` are the two rows this screen wants on top; everything
// else is common. An empty buffer is NOT an empty reader: there is one output
// capture in the OS, and when something else holds it the command still runs
// and the buffer comes back untouched.
static bool ct_collect(const char *first, const char *second, const char *third) {
    if (!g_ct_ready) return false;
    g_ct_ready = 0;
    ct_clear();

    if (!g_ct_out[0]) {
        g_ct_state = CT_BUSY_BUF;
        return true;
    }

    // THE EXIT STATUS IS CHECKED BEFORE THE FIELDS ARE, and it has to be.
    //
    // A read whose checksum failed still prints a ROM row — deliberately, so
    // somebody at a terminal can see what came back — and looking for the field
    // first would find it and show a rejected key as a good one. Neither
    // command returns zero for anything but a clean read, so the status is the
    // reliable question and the text is only for saying why.
    if (g_ct_rc != 0) {
        g_ct_state = CT_FAILED;
        if (strstr(g_ct_out, "No tag") || strstr(g_ct_out, "Nothing answered"))
            g_ct_state = CT_NOTHING;
        if (strstr(g_ct_out, "did not check out"))
            nova::copy(g_ct_why, sizeof(g_ct_why), "bad checksum - hold still");
        else if (strstr(g_ct_out, "do not know which"))
            nova::copy(g_ct_why, sizeof(g_ct_why), "no pin configured");
        else if (strstr(g_ct_out, "No PN532") || strstr(g_ct_out, "stopped answering"))
            nova::copy(g_ct_why, sizeof(g_ct_why), "no reader answered");
        else if (strstr(g_ct_out, "DIP switches"))
            nova::copy(g_ct_why, sizeof(g_ct_why), "set the DIPs to I2C");
        return true;
    }

    if (ct_field(g_ct_out, first, g_ct_l1, sizeof(g_ct_l1))) {
        ct_field(g_ct_out, second, g_ct_l2, sizeof(g_ct_l2));
        if (third) {
            char v[16];
            // The third row carries its own label, because on its own "08" or
            // "ok" says nothing. The first two do not need one: a UID and a
            // card type are recognisable as themselves.
            if (ct_field(g_ct_out, third, v, sizeof(v)))
                snprintf(g_ct_l3, sizeof(g_ct_l3), "%s %s", third, v);
        }
        g_ct_state = CT_OK;
        return true;
    }
    // Zero status and nothing recognisable in it. Not expected, and reported as
    // a failure rather than as an empty reader — an empty reader is a thing the
    // commands say in words.
    g_ct_state = CT_FAILED;
    nova::copy(g_ct_why, sizeof(g_ct_why), "unreadable answer");
    return true;
}

// --- the shared screen ----------------------------------------------------------
//
// Both readers are one screen with two sets of words. Writing them twice would
// be two copies of the worker handshake, which is the part worth having once.

class ContactScreen : public Screen {
public:
    bool animating(void) const override { return g_ct_busy != 0; }

    void enter(void) override {
        ct_disown();
        phase_ = 0;
        g_ct_state = CT_NEVER;
        ct_clear();
        kicked_ = false;
    }

    void leave(void) override { ct_disown(); }

    bool tick(uint32_t dt) override {
        phase_ += dt;
        const bool changed = ct_collect(field1(), field2(), field3());
        // One read on arrival, then nothing until asked. Opening the screen IS
        // the request — v1's NFC app started polling the moment it opened and
        // this keeps that — but repeating it forever would hold the reader's RF
        // field on for as long as the screen is up.
        //
        // From a per-screen flag rather than from the shared state, which
        // outlives the screen: keying off that meant the reader ran the first
        // time the app was ever opened and showed a stale answer every time
        // after.
        if (!kicked_ && !g_ct_busy) {
            char line[48];
            // No pin is not a failed read, and must not be worded as one. On a
            // stock Pico 2 W the iButton pin is genuinely unassigned — novaboard
            // leaves it so on both profiles — and "the reader did not answer"
            // sends somebody looking at their wiring for a thing that was never
            // switched on.
            //
            // The home screen normally gets there first: an unwired module
            // greys its app out and explain_unavailable says which pins are
            // missing. This is the case that gets past it — a pin cleared while
            // the screen is open, and the renderer, which opens every screen
            // directly.
            if (!command(line, sizeof(line))) {
                g_ct_state = CT_NOPIN;
                kicked_ = true;
                return true;
            }
            kicked_ = ct_start(line);
        }
        return changed || g_ct_busy;
    }

    void draw(Canvas &c) override {
        int y = ui::TOP;
        c.text(2, y, heading(), 1);
        if (g_ct_busy) c.spinner(c.width() - 9, y, phase_ / CT_SPIN_MS, 1);
        y += ui::ROWH;

        if (g_ct_state == CT_OK) {
            // Type above UID, the way v1 put the identified kind above the
            // number: the kind is what somebody reads at a glance and the
            // number is what they write down.
            c.text_fit(2, y, g_ct_l2[0] ? g_ct_l2 : "?", 1, c.width() - 4, false);
            y += ui::ROWH;
            y = wrapped(c, y, g_ct_l1);
            if (g_ct_l3[0]) c.text_fit(2, y, g_ct_l3, 1, c.width() - 4, false);
        } else if (g_ct_busy) {
            c.text(2, y, waiting(), 1);
        } else {
            switch (g_ct_state) {
                case CT_NOTHING:
                    c.text(2, y, nothing(), 1);
                    y += ui::ROWH;
                    c.text_fit(2, y, hint(), 1, c.width() - 4, false);
                    break;
                case CT_BUSY_BUF:
                    c.text(2, y, "No answer - busy.", 1);
                    y += ui::ROWH;
                    c.text_fit(2, y, "Something else is using", 1, c.width() - 4, false);
                    y += ui::ROWH;
                    c.text_fit(2, y, "the command output.", 1, c.width() - 4, false);
                    break;
                case CT_NOPIN:
                    c.text(2, y, "No pin is set.", 1);
                    y += ui::ROWH;
                    c.text(2, y, "Set one with", 1);
                    y += ui::ROWH;
                    c.text_fit(2, y, nopin(), 1, c.width() - 4, false);
                    break;
                case CT_FAILED:
                    c.text(2, y, "The reader did not", 1);
                    y += ui::ROWH;
                    c.text(2, y, "answer.", 1);
                    y += ui::ROWH;
                    if (g_ct_why[0]) c.text_fit(2, y, g_ct_why, 1, c.width() - 4, false);
                    break;
                default:
                    c.text(2, y, waiting(), 1);
                    break;
            }
        }

        // The footer is a live reading of what the reader is doing, not a
        // control hint. The hints live in help(), which is what keeps this line
        // free to say something true.
        char foot[26];
        status(foot, sizeof(foot));
        c.text_fit(2, c.height() - ui::FH, foot, 1, c.width() - 4, false);
    }

    Action on_event(Event e) override {
        if (e == EV_SELECT || e == EV_SELECT_HOLD) {
            if (!g_ct_busy) {
                char line[48];
                if (command(line, sizeof(line))) {
                    ct_clear();
                    g_ct_state = CT_NEVER;
                    ct_start(line);
                }
            }
            return ui::ACT_STAY;
        }
        // BACK and HOME fall through to the base, which pops. leave() disowns
        // whatever is still running, so the answer it eventually produces is
        // dropped rather than written into the next screen's fields.
        return Screen::on_event(e);
    }

protected:
    // What the two subclasses differ by.
    virtual const char *heading(void) const { return ""; }
    virtual const char *waiting(void) const { return ""; }
    virtual const char *nothing(void) const { return ""; }
    virtual const char *hint(void) const { return ""; }
    virtual const char *nopin(void) const { return ""; }   // how to set one
    virtual const char *field1(void) const { return ""; }   // the number
    virtual const char *field2(void) const { return ""; }   // what it is
    virtual const char *field3(void) const { return nullptr; }
    virtual bool command(char *out, unsigned cap) const { (void)out; (void)cap; return false; }

    unsigned phase_;
    bool     kicked_;

private:
    // A UID or a ROM can be longer than the panel is wide — eight bytes of hex
    // is 23 characters against the twenty that fit — so it goes over two rows.
    //
    // TRUNCATING IS THE ONE THING THAT MUST NOT HAPPEN. Half a UID looks
    // exactly like a whole one, and somebody would write it down.
    //
    // Split as near the middle as a byte boundary allows, rather than filling
    // the first row and dropping the remainder onto the second. Both give two
    // legal rows; only one of them avoids a lone "3D" hanging under a full
    // line, which reads as a rendering fault rather than as a continuation.
    static int wrapped(Canvas &c, int y, const char *s) {
        const int cols = (c.width() - 4) / ui::ADV;
        const int len = (int)strlen(s);
        if (len <= cols) {
            c.text(2, y, s, 1);
            return y + ui::ROWH;
        }

        // The space nearest the middle, searched outwards from it.
        int cut = -1;
        for (int d = 0; d <= len / 2 && cut < 0; d++) {
            if (len / 2 - d > 0 && s[len / 2 - d] == ' ') cut = len / 2 - d;
            else if (len / 2 + d < len && s[len / 2 + d] == ' ') cut = len / 2 + d;
        }
        // No space at all, or a half that still will not fit: fall back to
        // filling the first row, which always leaves a second that fits.
        if (cut < 0 || cut > cols || len - cut - 1 > cols) {
            cut = cols;
            while (cut > 0 && s[cut] != ' ') cut--;
            if (cut <= 0) cut = cols;
        }

        char head[26];
        int n = cut < (int)sizeof(head) ? cut : (int)sizeof(head) - 1;
        memcpy(head, s, (size_t)n);
        head[n] = 0;
        c.text(2, y, head, 1);
        y += ui::ROWH;
        c.text_fit(2, y, s + cut + 1, 1, c.width() - 4, false);
        return y + ui::ROWH;
    }

    void status(char *out, unsigned cap) const {
        if (g_ct_busy) {
            snprintf(out, cap, "reading %us",
                     (unsigned)((fw_millis() - g_ct_start_ms) / 1000));
            return;
        }
        if (g_ct_state == CT_OK) { nova::copy(out, cap, "Sel = read again"); return; }
        if (g_ct_state == CT_NEVER) { nova::copy(out, cap, "starting..."); return; }
        // Pressing SELECT with no pin set does nothing, so it is not offered.
        if (g_ct_state == CT_NOPIN) { nova::copy(out, cap, "not configured"); return; }
        nova::copy(out, cap, "Sel = try again");
    }
};

// --- NFC ---------------------------------------------------------------------------
//
// v1's NFC app in the shape it had: open it and it reads, because that is what
// somebody opening it wanted. Its Select saved a Flipper .nfc file, which
// cannot happen here — see the note at the top — so Select reads again instead,
// which is the other thing that hand was reaching for.

class NfcScreen : public ContactScreen {
public:
    const char *title(void) const override { return "NFC"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Hold a card flat on the";
        out[1] = "antenna. SELECT reads again.";
        out[2] = "A read waits 5s for a tag.";
        return 3;
    }

protected:
    const char *heading(void) const override { return "NFC reader"; }
    const char *waiting(void) const override { return "tap a tag..."; }
    const char *nothing(void) const override { return "No tag."; }
    const char *hint(void) const override { return "Hold it flat on the antenna."; }
    const char *nopin(void) const override { return "d1 pins set sda"; }
    const char *field1(void) const override { return "UID"; }
    const char *field2(void) const override { return "Type"; }
    const char *field3(void) const override { return "SAK"; }

    bool command(char *out, unsigned cap) const override {
        // The firmware command has no default pins and should not have one:
        // guessing a pair means driving two GPIOs that belong to something
        // else. The package is the side that knows, so it says.
        const int sda = board::pin(board::PIN_SDA);
        const int scl = board::pin(board::PIN_SCL);
        if (sda == board::PIN_NONE || scl == board::PIN_NONE) return false;
        snprintf(out, cap, "nfc read %d %d %d", CT_READ_SECS, sda, scl);
        return true;
    }
};

// --- iButton -------------------------------------------------------------------------
//
// New here — v1 had the module in its table and no driver behind it.
//
// The reference board assigns no pin for one. novaboard leaves ibutton unset on
// both profiles because the map has three pins left, so on a stock Pico 2 W this
// screen opens straight into "no pin configured", which is the honest answer and
// is said in those words rather than left as an empty panel.

class IButtonScreen : public ContactScreen {
public:
    const char *title(void) const override { return "iButton"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Touch the key to both";
        out[1] = "contacts and hold it still.";
        out[2] = "Set the pin in d1 pins.";
        return 3;
    }

protected:
    const char *heading(void) const override { return "iButton reader"; }
    const char *waiting(void) const override { return "touch the key..."; }
    const char *nothing(void) const override { return "Nothing answered."; }
    const char *hint(void) const override { return "Touch the key to the reader."; }
    const char *nopin(void) const override { return "d1 pins set ibutton"; }
    const char *field1(void) const override { return "ROM"; }
    const char *field2(void) const override { return "Family"; }
    const char *field3(void) const override { return "CRC"; }

    bool command(char *out, unsigned cap) const override {
        const int pin = board::pin(board::PIN_IBUTTON);
        if (pin == board::PIN_NONE) return false;
        snprintf(out, cap, "ibutton read %d %d", pin, CT_READ_SECS);
        return true;
    }
};

// --- the App table's entry points ---------------------------------------------------

void open_nfc(void)     { gui::push<NfcScreen>(); }
void open_ibutton(void) { gui::push<IButtonScreen>(); }

}  // namespace screens
}  // namespace nova
