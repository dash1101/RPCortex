// Desc: The WiFi screens — the link, the scan, the saved networks and the survey.
// File: novagui_wifi.cpp
#include "novagui_wifi.h"
#include "novagui.h"
#include "novakeys.h"
#include "novacore.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace screens {

using ui::Screen;
using ui::Action;

// --- the radio, off the UI thread -------------------------------------------------
//
// A scan takes seconds; a join can take twenty. The UI loop is the thing that
// draws, so either of them run on it is a panel that stops dead for that long —
// and from the outside a device that has stopped and one that has crashed are
// the same device. Both therefore happen on a spawned task, and the screens
// POLL a flag from tick().
//
// EVERYTHING THE WORKER TOUCHES IS A FILE STATIC, deliberately. BACK is one
// press and a scan is three seconds, so the screen that asked for one is
// routinely gone before it answers; a worker writing into a screen would be
// writing into a pool slot that has since been handed to something else.

// What one scan can report. The firmware's own table is 24 entries (SCAN_MAX in
// the shell's net.cpp) and it will not hand back more, so asking for more would
// only be a larger array that never fills.
#define AP_MAX 24

// One table, shared by the scan list and the survey, and one worker at a time.
// Two scans at once would be two writers on this array — and there is one radio
// underneath them anyway, so the second was never going to be a second scan.
static FwNetAp      g_aps[AP_MAX];
static volatile int g_ap_n;

#define OP_IDLE 0
#define OP_SCAN 1
#define OP_JOIN 2

// What a finished operation wants the screen to do next. A code rather than a
// message that gets matched against: the message is for a person to read, and
// the day somebody rewords one is not the day the flow should change.
#define R_NONE      0
#define R_LISTED    1   // a scan found something
#define R_JOINED    2
#define R_NEED_PW   3   // the network wants a password nobody has stored

static volatile int  g_op;         // what the worker is doing, OP_IDLE when nothing
static volatile bool g_op_done;    // a result is waiting to be read
static int           g_op_result;
static char          g_op_msg[24]; // what to show for it, already shortened to fit

// One radio, one scanner. The survey's worker writes the same table, and it can
// still be inside a scan for seconds after the screen that started it has gone.
static bool survey_live(void);

// And one link. The device sweep does not touch the scan table, but it does
// spend a minute talking over an association a scan would break — so it counts
// as busy from both sides: nothing starts a sweep while the radio is out, and a
// scan started mid-sweep would be a scan that ruins the answer it interrupts.
static bool lan_live(void);

// The join the worker is to make. Written before it is started and not touched
// again until it finishes.
static char g_join_ssid[FW_NET_SSID_MAX];
static char g_join_pw[64];
static bool g_join_open;           // an open network, which joins by another route

// What the shell command printed. A static rather than a local because the
// worker's stack is the package's own and a couple of hundred bytes of it is
// worth more here than a message that is read once.
static char g_shell_out[192];

static void do_scan(void);
static void do_join(void);

static int radio_worker(void *) {
    int op = g_op;
    if (op == OP_SCAN)      do_scan();
    else if (op == OP_JOIN) do_join();
    // The result, then the flag, then the lock — in that order. A reader that
    // sees `done` must already be able to see everything that produced it, and
    // a reader that sees OP_IDLE must be free to start the next one.
    g_op_done = true;
    g_op      = OP_IDLE;
    // The screen is asleep for up to 300 ms at a time and this is the moment
    // its content changed, so it is told rather than left to notice.
    gui::invalidate();
    return 0;
}

static bool op_busy(void) { return g_op != OP_IDLE || survey_live() || lan_live(); }

// Start one, or say why not. Never queues: a second request while the radio is
// busy is somebody pressing again, not a second thing they want.
static bool op_start(int op) {
    if (op_busy()) return false;
    g_op_msg[0] = 0;
    g_op_done   = false;
    g_op        = op;
    // 3072 rather than the 2048 novainput asks for, and it matters: fw_shell_run
    // runs a whole shell command — the wifi command, the driver, lwIP — on the
    // stack of whichever task called it. That is what PKG_STACK_BYTES is sized
    // for, and asking for exactly it takes a pooled sandbox stack rather than a
    // fresh allocation.
    if (fw_task_spawn("novawifi", radio_worker, nullptr, 3072) >= 0) return true;
    g_op = OP_IDLE;
    nova::copy(g_op_msg, sizeof(g_op_msg), "No task free");
    g_op_done = true;
    return false;
}

// Take the result, once. Returns false when there is nothing new.
static bool op_take(void) {
    if (!g_op_done) return false;
    g_op_done = false;
    return true;
}

static void do_scan(void) {
    int n = fw_net_scan(g_aps, AP_MAX);
    g_ap_n = n > 0 ? n : 0;
    if (n < 0)       nova::copy(g_op_msg, sizeof(g_op_msg), "Radio unavailable");
    else if (n == 0) nova::copy(g_op_msg, sizeof(g_op_msg), "No networks");
    else             g_op_msg[0] = 0;
    g_op_result = n > 0 ? R_LISTED : R_NONE;
}

// --- saved networks -----------------------------------------------------------------
//
// The shell's own four slots, read and written where they already live.
//
// `wifi list`, `wifi forget` and the boot join all read WiFi.Net0..3_SSID, so
// the package uses the same keys rather than keeping a list of its own: two
// lists of saved networks disagree the first time somebody uses the shell, and
// the one that loses is always the one they are not looking at.

#define SAVED_MAX 4

static void saved_key(int i, const char *field, char *out, unsigned cap) {
    snprintf(out, cap, "WiFi.Net%d_%s", i, field);
}

static const char *saved_ssid(int i) {
    char k[24];
    saved_key(i, "SSID", k, sizeof(k));
    return nova::reg(k, "");
}

static int saved_find(const char *ssid) {
    if (!ssid[0]) return -1;
    for (int i = 0; i < SAVED_MAX; i++)
        if (!strcmp(saved_ssid(i), ssid)) return i;
    return -1;
}

// The names, without the gaps, kept alongside the registry rather than read out
// of it again for every row of every frame. A list of six rows asking whether
// each is saved is twenty-four registry calls a frame at 296 cycles each, to
// answer a question whose answer only changes when somebody changes it.
static char g_saved_names[SAVED_MAX][FW_NET_SSID_MAX];
static int  g_saved_n;

static void saved_reload(void) {
    g_saved_n = 0;
    for (int i = 0; i < SAVED_MAX; i++) {
        const char *s = saved_ssid(i);
        if (!s[0]) continue;
        nova::copy(g_saved_names[g_saved_n], FW_NET_SSID_MAX, s);
        g_saved_n++;
    }
}

static bool is_saved(const char *ssid) {
    for (int i = 0; i < g_saved_n; i++)
        if (!strcmp(g_saved_names[i], ssid)) return true;
    return false;
}

// Save, replacing an entry for the same name. THE EVICTION POLICY IS COPIED
// FROM THE SHELL rather than invented here: full and nothing matching means
// slot 0 goes, because a device that has seen five networks should still
// remember the one it just joined. Mirroring it is the lesser of two evils — the
// alternative is a package that refuses where the shell would not, so the same
// action succeeds or fails depending on which one you used.
static void saved_put(const char *ssid, const char *pw) {
    int slot = saved_find(ssid);
    if (slot < 0) {
        for (int i = 0; i < SAVED_MAX && slot < 0; i++)
            if (!saved_ssid(i)[0]) slot = i;
    }
    if (slot < 0) slot = 0;
    char k[24];
    saved_key(slot, "SSID", k, sizeof(k)); nova::reg_set(k, ssid);
    saved_key(slot, "PW",   k, sizeof(k)); nova::reg_set(k, pw ? pw : "");
}

// Forgetting is a REGISTRY EDIT, not a radio operation — the MicroPython screen
// said so in its own comment, and it is the reason this works while a scan has
// the radio. It is also two keys, which is less than a shell command and its
// quoting would have been.
static void saved_forget(const char *ssid) {
    int slot = saved_find(ssid);
    if (slot < 0) return;
    char k[24];
    saved_key(slot, "SSID", k, sizeof(k)); nova::reg_set(k, "");
    saved_key(slot, "PW",   k, sizeof(k)); nova::reg_set(k, "");
    // Written through to flash here. reg_set alone lives in RAM, and a network
    // that comes back after a reboot has not been forgotten.
    nova::reg_save();
    saved_reload();
}

// --- joining --------------------------------------------------------------------------
//
// There is no fw_net_connect: the ABI gives a package the scan and the status
// and nothing that associates, so a join is the shell's `wifi` command run
// through fw_shell_run. Three things about that spelling are load-bearing.
//
// -s IS NOT OPTIONAL. Without it the command PROMPTS on the serial console for a
// password it cannot find saved, and that prompt waits for a human by yielding
// forever. Run from this task, it is a screen that never comes back.
//
// THE PASSWORD IS NEVER ON THE LINE. `wifi connect` has no password argument at
// all — everything after the subcommand is joined back into one SSID, so a
// password there becomes part of the network's name. It is saved to the registry
// first and the command picks it up from there, which is exactly what the
// MicroPython screen did with net.add_network, and it also keeps the key out of
// the command line the crash recorder keeps a copy of.
//
// THE NAME IS QUOTED. An SSID is a string somebody else chose and the shell
// splits an unquoted line on ';', '&&', '||', '|' and '>', so a network called
// `x; reboot` would be a network name and then a second command. Quotes stop
// all of those. A '"' inside the name cannot be escaped — the tokeniser has no
// escape character — so a name containing one is refused instead of guessed at.

static bool name_is_safe(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p == '"' || *p < 32) return false;
    return true;
}

static void do_join(void) {
    char line[RPC_SHELL_LINE_MAX];

    // Only when there is something to record. Saving a network that is already
    // saved, with the empty password this screen carries when it did not ask for
    // one, would replace the stored key with nothing — and the network would
    // then fail to join for the rest of the device's life for no visible reason.
    if (g_join_pw[0] || saved_find(g_join_ssid) < 0) saved_put(g_join_ssid, g_join_pw);

    if (g_join_open) {
        // An OPEN network cannot be joined by name without a prompt. `wifi
        // connect -s` refuses when it finds no saved password, and a saved
        // password is precisely what an open network does not have; the only
        // non-interactive door left is autoconnect, which joins the strongest
        // SAVED network in range. Usually that is the one just chosen, and when
        // it is not, the screen says which one it actually got rather than
        // claiming the one it asked for.
        nova::copy(line, sizeof(line), "wifi autoconnect -s");
    } else {
        snprintf(line, sizeof(line), "wifi connect -s \"%s\"", g_join_ssid);
    }

    g_shell_out[0] = 0;
    int rc = fw_shell_run(line, g_shell_out, sizeof(g_shell_out));
    g_op_result = R_NONE;
    saved_reload();

    if (rc == 0 && fw_net_connected()) {
        // The join wrote WiFi.Active and the saved slot in RAM; this is what
        // makes them survive the next reboot.
        nova::reg_save();
        char got[FW_NET_SSID_MAX];
        got[0] = 0;
        fw_net_ssid(got, sizeof(got));
        if (got[0] && strcmp(got, g_join_ssid) != 0)
            snprintf(g_op_msg, sizeof(g_op_msg), "Joined %s", got);
        else
            nova::copy(g_op_msg, sizeof(g_op_msg), "Connected");
        g_op_result = R_JOINED;
        return;
    }

    // The command explains itself, so its own words are read rather than
    // guessed at from a status code. An EMPTY buffer is not an empty answer:
    // there is one capture and a pipeline elsewhere may hold it, in which case
    // the command still ran and nothing was recorded.
    const char *out = g_shell_out;
    if (strstr(out, "saved password")) {
        nova::copy(g_op_msg, sizeof(g_op_msg), "Password needed");
        g_op_result = R_NEED_PW;
    }
    else if (strstr(out, "wrong password"))  nova::copy(g_op_msg, sizeof(g_op_msg), "Wrong password");
    else if (strstr(out, "no such network")) nova::copy(g_op_msg, sizeof(g_op_msg), "Not in range");
    else if (strstr(out, "admin account"))   nova::copy(g_op_msg, sizeof(g_op_msg), "Needs an admin login");
    else                                     nova::copy(g_op_msg, sizeof(g_op_msg), "Could not join");
}

// --- shared drawing -------------------------------------------------------------------

// Signal as bars. The thresholds are the status bar's own — -55 and -70 — so
// three bars in the corner and three bars in a list row mean the same thing.
static void bars(Canvas &c, int x, int y, int rssi, int colour) {
    int lit = rssi == 0 ? 0 : rssi >= -55 ? 3 : rssi >= -70 ? 2 : 1;
    for (int i = 0; i < 3; i++) {
        int h = 2 + i * 2;
        if (i < lit) c.fill_rect(x + i * 3, y + (ui::FH - 1 - h), 2, h, 1);
        else         c.hline(x + i * 3, y + ui::FH - 1, 2, colour);
    }
}

// A padlock, five pixels wide. A word would not fit beside an SSID and a
// character from the font — '*', '#' — means nothing to anybody; the shape does.
static void padlock(Canvas &c, int x, int y, int colour) {
    c.fill_rect(x, y + 3, 5, 4, colour);
    c.hline(x + 1, y, 3, colour);
    c.vline(x, y + 1, 2, colour);
    c.vline(x + 4, y + 1, 2, colour);
}

// --- WiFi ------------------------------------------------------------------------------
//
// Two views in one screen, exactly as the MicroPython one had them: the link as
// it stands, and — once a scan has been run — what is in range. BACK from the
// list returns to the status rather than leaving the app, because backing out of
// a list somebody just waited three seconds for is not what they meant.

#define VIEW_STATUS 0
#define VIEW_LIST   1

// What tick() should do next. The work is deferred to tick() rather than done in
// on_event for one reason: the keyboard calls its callback and THEN returns
// ACT_BACK, and the runner pops whatever is on top when it does — so a screen
// pushed from inside the callback is the screen that gets popped. Setting a step
// and letting the next tick act on it happens after the pop has already run.
#define P_NONE     0
#define P_SCAN     1
#define P_ASK_SSID 2
#define P_ASK_PW   3
#define P_JOIN     4

// The screen's state, outside the screen.
//
// enter() runs again every time a child screen pops — the keyboard, a notice —
// so anything reset there would be lost between typing a password and using it.
// A pool slot is also reused, and a re-used slot holds the last screen's bytes
// rather than zeroes, so the alternative was not "leave the members alone".
struct WifiState {
    int  view;
    int  sel, top;
    int  pending;
    char msg[24];
};
static WifiState g_w;

class WifiScreen : public Screen {
public:
    const char *title(void) const override { return "WiFi"; }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "SELECT scans.";
        out[1] = "On a network: SELECT joins,";
        out[2] = "hold SELECT forgets it.";
        out[3] = "BACK returns to the status.";
        return 4;
    }

    // enter() runs again every time a child pops, which is exactly when the
    // saved list may have changed under this screen — a forget, or a join that
    // stored a network. Reading it again here is the refresh as well as the
    // setup, and it is the only thing this screen keeps of its own.
    void enter(void) override { poll_ = 0; saved_reload(); }

    bool tick(uint32_t dt) override {
        // A step that needs a screen pushed. One per tick, so the keyboard it
        // pushes is the top by the time the next one is looked at.
        if (g_w.pending == P_ASK_SSID) {
            g_w.pending = P_NONE;
            ui::keyboard("Network", nullptr, false, got_ssid, nullptr, nullptr);
            return true;
        }
        if (g_w.pending == P_ASK_PW) {
            g_w.pending = P_NONE;
            ui::keyboard("Password", nullptr, true, got_pw, nullptr, nullptr);
            return true;
        }
        if (g_w.pending == P_SCAN || g_w.pending == P_JOIN) {
            int op = g_w.pending == P_SCAN ? OP_SCAN : OP_JOIN;
            g_w.pending = P_NONE;
            if (!op_start(op)) {
                nova::copy(g_w.msg, sizeof(g_w.msg),
                           op_busy() ? "Radio busy" : g_op_msg);
                return true;
            }
            nova::copy(g_w.msg, sizeof(g_w.msg), op == OP_SCAN ? "Scanning..." : "Connecting...");
            return true;
        }

        if (op_take()) {
            nova::copy(g_w.msg, sizeof(g_w.msg), g_op_msg);
            if (g_op_result == R_LISTED) {
                g_w.view = VIEW_LIST;
                g_w.sel = g_w.top = 0;
            }
            // A join that came back asking for a password picks up where the
            // MicroPython screen did: the name is already known, so only the
            // keyboard is missing.
            else if (g_op_result == R_NEED_PW) g_w.pending = P_ASK_PW;
            // Back to the status view once a join has landed, which is where
            // the answer — an address — actually is.
            else if (g_op_result == R_JOINED)  g_w.view = VIEW_STATUS;
            return true;
        }

        // While the worker is out, redraw a few times a second for the spinner.
        // Not animating(): that would hold the whole loop at sixty frames for
        // the length of a scan to turn one glyph.
        if (op_busy()) {
            poll_ += dt;
            if (poll_ < 200) return false;
            poll_ = 0;
            return true;
        }
        return false;
    }

    void draw(Canvas &c) override {
        if (g_w.view == VIEW_LIST) draw_list(c);
        else                       draw_status(c);

        if (op_busy()) c.spinner(c.width() - 8, c.height() - ui::FH, fw_millis() / 120, 1);
        if (g_w.msg[0]) c.text_fit(0, c.height() - ui::FH, g_w.msg, 1, c.width() - 10, false);
    }

    Action on_event(Event e) override {
        if (g_w.view == VIEW_LIST) return list_event(e);

        if (e == EV_SELECT) { g_w.pending = P_SCAN; return ui::ACT_STAY; }
        // Holding SELECT adds a network by hand, with nothing in range and
        // without a scan. It is the only way to reach a HIDDEN one: the
        // firmware's scan drops a beacon with an empty name, so a hidden
        // network is not a row that could be chosen.
        if (e == EV_SELECT_HOLD) { g_w.pending = P_ASK_SSID; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    uint32_t poll_;

    // The list, with a synthetic row on the front. Index 0 is "add by hand" and
    // the rest are what the scan found.
    static int row_count(void) { return g_ap_n + 1; }

    void draw_status(Canvas &c) {
        const int vx = ui::ADV * 6;          // a six-character label column
        int y = ui::TOP;
        char v[40];

        bool up = fw_net_connected() != 0;
        c.text(0, y, "Link", 1);
        if (up) {
            v[0] = 0;
            fw_net_ssid(v, sizeof(v));
            c.text_fit(vx, y, v[0] ? v : "connected", 1, c.width() - vx, false);
        } else {
            c.text(vx, y, "not connected", 1);
        }
        y += ui::ROWH;

        if (up) {
            v[0] = 0;
            fw_net_ip(v, sizeof(v));
            c.text(0, y, "IP", 1);
            c.text(vx, y, v[0] ? v : "no address", 1);
            y += ui::ROWH;

            int rssi = fw_net_rssi();
            c.text(0, y, "Signal", 1);
            bars(c, vx, y, rssi, 1);
            if (rssi) {
                snprintf(v, sizeof(v), "%d dBm", rssi);
                c.text(vx + 12, y, v, 1);
            } else {
                c.text(vx + 12, y, "no reading", 1);
            }
            y += ui::ROWH;
        }

        int n = 0;
        for (int i = 0; i < SAVED_MAX; i++) if (saved_ssid(i)[0]) n++;
        snprintf(v, sizeof(v), "%d of %d", n, SAVED_MAX);
        c.text(0, y, "Saved", 1);
        c.text(vx, y, v, 1);
    }

    void draw_list(Canvas &c) {
        const int rows  = ui::rows_for(c);
        const int total = row_count();
        if (g_w.sel < g_w.top)              g_w.top = g_w.sel;
        else if (g_w.sel >= g_w.top + rows) g_w.top = g_w.sel - rows + 1;

        const bool scrolls = total > rows;
        const int  right   = scrolls ? c.width() - (ui::SB_W + 1) : c.width();

        for (int i = 0; i < rows; i++) {
            const int idx = g_w.top + i;
            if (idx >= total) break;
            const int y  = ui::TOP + i * ui::ROWH;
            const bool on = (idx == g_w.sel);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);
            const int colour = on ? 0 : 1;

            if (idx == 0) {
                c.text(3, y, "+ Add network", colour);
                continue;
            }

            const FwNetAp &ap = g_aps[idx - 1];
            // A saved network is marked at the FRONT, where the eye already is,
            // and the marks that are about the radio — locked, how strong — are
            // together at the other end.
            int x = 3;
            if (is_saved(ap.ssid)) { c.text(x, y, "*", colour); }
            x += ui::ADV;

            const int meta = 3 + 6 + 9;              // padlock, bars and their gaps
            c.text_fit(x, y, ap.ssid, colour, right - x - meta, false);
            if (ap.secured) padlock(c, right - meta + 1, y, colour);
            bars(c, right - 9, y, ap.rssi, colour);
        }

        if (scrolls)
            c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP,
                        g_w.top, rows, total);
    }

    Action list_event(Event e) {
        const int n = row_count();
        if (e == EV_ROT_CW)  { g_w.sel = (g_w.sel + 1) % n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { g_w.sel = (g_w.sel + n - 1) % n; return ui::ACT_STAY; }

        if (e == EV_SELECT) {
            if (g_w.sel == 0) { g_w.pending = P_ASK_SSID; return ui::ACT_STAY; }
            const FwNetAp &ap = g_aps[g_w.sel - 1];
            if (!name_is_safe(ap.ssid)) {
                ui::notice("WiFi", "That name has a quote in it, which cannot be "
                                   "passed safely. Join it from the shell.");
                return ui::ACT_STAY;
            }
            nova::copy(g_join_ssid, sizeof(g_join_ssid), ap.ssid);
            g_join_open = !ap.secured;
            g_join_pw[0] = 0;
            // A saved network joins with what is already stored. One that is not
            // saved goes STRAIGHT to the keyboard — the MicroPython screen used
            // to say "use the shell" here, which made a locked network
            // unjoinable from the device that had a keyboard on it.
            g_w.pending = (is_saved(ap.ssid) || g_join_open) ? P_JOIN : P_ASK_PW;
            return ui::ACT_STAY;
        }

        if (e == EV_SELECT_HOLD) {
            if (g_w.sel == 0) return ui::ACT_STAY;
            const FwNetAp &ap = g_aps[g_w.sel - 1];
            if (!is_saved(ap.ssid)) return ui::ACT_STAY;
            ask_forget(ap.ssid);
            return ui::ACT_STAY;
        }

        // BACK goes to the status view, not out of the app.
        if (e == EV_BACK) { g_w.view = VIEW_STATUS; g_w.msg[0] = 0; return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

    static void got_ssid(void *, const char *text) {
        // The text points into the keyboard and the keyboard is about to pop.
        nova::copy(g_join_ssid, sizeof(g_join_ssid), text ? text : "");
        g_join_pw[0] = 0;
        g_join_open  = false;
        if (!g_join_ssid[0]) return;
        if (!name_is_safe(g_join_ssid)) {
            nova::copy(g_w.msg, sizeof(g_w.msg), "Name not usable");
            return;
        }
        g_w.pending = P_ASK_PW;
    }

    static void got_pw(void *, const char *text) {
        nova::copy(g_join_pw, sizeof(g_join_pw), text ? text : "");
        // A blank password means the network is open, whatever the scan said —
        // somebody who pressed OK on an empty field meant it.
        g_join_open = (g_join_pw[0] == 0);
        g_w.pending = P_JOIN;
    }

    static void forget_yes(void *) {
        saved_forget(g_join_ssid);
        snprintf(g_w.msg, sizeof(g_w.msg), "Forgot %s", g_join_ssid);
    }

    static void ask_forget(const char *ssid) {
        nova::copy(g_join_ssid, sizeof(g_join_ssid), ssid);
        // The question is held in a static because the confirmation keeps the
        // POINTER it is given and outlives whatever built the text.
        static char q[48];
        snprintf(q, sizeof(q), "Forget %s?", ssid);
        ui::confirm(q, "Forget", forget_yes, nullptr);
    }
};

void open_wifi(void) {
    g_w.view    = VIEW_STATUS;
    g_w.sel     = 0;
    g_w.top     = 0;
    g_w.pending = P_NONE;
    g_w.msg[0]  = 0;
    // A worker may still be inside a scan started from a previous visit — that
    // is seconds, and this screen is one press away. Its results land in the
    // table below, so the table is only cleared when nothing is about to write
    // to it.
    if (!op_busy()) g_ap_n = 0;
    gui::push<WifiScreen>();
}

// --- Networks --------------------------------------------------------------------------
//
// The four saved slots, on their own, so a network can be forgotten or rejoined
// without waiting for a scan to find it. This is also the only screen that works
// with the radio off or out of range: both of its actions are registry edits,
// and only the join needs anything more.

class NetworksScreen : public Screen {
public:
    const char *title(void) const override { return "Networks"; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "SELECT joins the network.";
        out[1] = "Hold SELECT forgets it.";
        out[2] = "A dot marks the active one.";
        return 3;
    }

    // enter() also runs when a confirmation pops, which is exactly when the list
    // has changed — so reading it again here is the refresh as well as the setup.
    void enter(void) override {
        saved_reload();
        if (sel_ >= g_saved_n) sel_ = g_saved_n ? g_saved_n - 1 : 0;
        if (sel_ < 0) sel_ = 0;
        top_ = 0;
        poll_ = 0;
    }

    bool tick(uint32_t dt) override {
        if (op_take()) { nova::copy(msg_, sizeof(msg_), g_op_msg); return true; }
        if (!op_busy()) return false;
        poll_ += dt;
        if (poll_ < 200) return false;
        poll_ = 0;
        return true;
    }

    void draw(Canvas &c) override {
        if (!g_saved_n) {
            c.text_centred(ui::TOP + ui::ROWH, "No saved networks", 1);
        } else {
            const int rows = ui::rows_for(c) - 1;      // the last row is the status
            if (sel_ < top_)              top_ = sel_;
            else if (sel_ >= top_ + rows) top_ = sel_ - rows + 1;

            const char *active = nova::reg("WiFi.Active", "");
            for (int i = 0; i < rows; i++) {
                const int idx = top_ + i;
                if (idx >= g_saved_n) break;
                const int y = ui::TOP + i * ui::ROWH;
                const bool on = (idx == sel_);
                if (on) c.rounded_rect(0, y - 1, c.width(), ui::ROWH, 1, true);
                const int colour = on ? 0 : 1;
                c.text_fit(3, y, g_saved_names[idx], colour, c.width() - 12, false);
                // The one the device is on gets a dot rather than a word: there
                // is no room for "active" beside a 32-character name.
                if (!strcmp(g_saved_names[idx], active))
                    c.fill_circle(c.width() - 5, y + ui::FH / 2, 2, colour);
            }
        }
        if (op_busy()) c.spinner(c.width() - 8, c.height() - ui::FH, fw_millis() / 120, 1);
        if (msg_[0]) c.text_fit(0, c.height() - ui::FH, msg_, 1, c.width() - 10, false);
    }

    Action on_event(Event e) override {
        if (!g_saved_n) return Screen::on_event(e);
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % g_saved_n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + g_saved_n - 1) % g_saved_n; return ui::ACT_STAY; }

        if (e == EV_SELECT) {
            if (op_busy()) { nova::copy(msg_, sizeof(msg_), "Radio busy"); return ui::ACT_STAY; }
            if (!name_is_safe(g_saved_names[sel_])) {
                nova::copy(msg_, sizeof(msg_), "Name not usable");
                return ui::ACT_STAY;
            }
            nova::copy(g_join_ssid, sizeof(g_join_ssid), g_saved_names[sel_]);
            // Saved already, so whatever password is stored is the one to use —
            // and an empty one means the network was saved as open, which joins
            // by the other route.
            g_join_pw[0] = 0;
            int slot = saved_find(g_join_ssid);
            char k[24];
            saved_key(slot < 0 ? 0 : slot, "PW", k, sizeof(k));
            g_join_open = (slot < 0) || (nova::reg(k, "")[0] == 0);
            if (op_start(OP_JOIN)) nova::copy(msg_, sizeof(msg_), "Connecting...");
            else                   nova::copy(msg_, sizeof(msg_), g_op_msg);
            return ui::ACT_STAY;
        }

        if (e == EV_SELECT_HOLD) {
            nova::copy(g_join_ssid, sizeof(g_join_ssid), g_saved_names[sel_]);
            static char q[48];
            snprintf(q, sizeof(q), "Forget %s?", g_join_ssid);
            ui::confirm(q, "Forget", forget_yes, nullptr);
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    int      sel_, top_;
    uint32_t poll_;
    char     msg_[24];

    static void forget_yes(void *) { saved_forget(g_join_ssid); }
};

void open_networks(void) { gui::push<NetworksScreen>(); }

// --- Wardrive ---------------------------------------------------------------------------
//
// A rolling survey: scan, log what is new, wait, scan again. Scan-based and not
// packet capture, exactly as the MicroPython one was — there is no monitor mode
// here either.
//
// WHAT A PACKAGE CAN SEE IS LESS THAN WHAT THE MICROPYTHON ONE COULD, and the
// difference is in the firmware rather than in this file. The scan behind
// fw_net_scan already merges every beacon with the same name into one entry and
// drops a beacon with no name at all, so a package cannot see individual radios
// or hidden networks — there is no BSSID to key on and nothing hidden to log.
// The survey therefore keys on the NAME, which loses nothing further: the two
// access points were one row before this file ever saw them.
//
// A row is consequently not a WiGLE row and the file does not claim to be one. A
// WiGLE upload is keyed on the MAC, and a file that says WigleWifi-1.4 with an
// empty MAC column is one that gets rejected with nothing to explain why.

#define WD_INTERVAL_MS 4000     // between scans, as the MicroPython survey used
#define WD_SEEN_MAX     128     // names remembered across the session
#define WD_BUF         4096     // one part file, held in RAM
#define WD_PARTS         20     // and how many of them a survey may write

static const char *const kWdHeader = "SSID,Security,FirstSeen,Channel,RSSI\n";

static volatile bool g_wd_run;      // the worker should keep going
static volatile bool g_wd_live;     // and it is still there
static volatile int  g_wd_aps;      // unique names logged
static volatile int  g_wd_pass;     // scans completed
static uint32_t      g_wd_seen[WD_SEEN_MAX];
static int           g_wd_seen_n;
static char          g_wd_buf[WD_BUF];
static unsigned      g_wd_len;
static char          g_wd_path[40];
static int           g_wd_part;
static char          g_wd_msg[24];

// A 32-bit hash of the name rather than the name itself. A hundred and
// twenty-eight names in full is four kilobytes of a package that is counting
// them; this is five hundred, and at that many entries the chance of two names
// colliding — one row silently not written — is about two in a million.
static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1u;
}

static bool wd_seen(const char *ssid) {
    uint32_t h = name_hash(ssid);
    for (int i = 0; i < g_wd_seen_n; i++) if (g_wd_seen[i] == h) return true;
    if (g_wd_seen_n < WD_SEEN_MAX) g_wd_seen[g_wd_seen_n++] = h;
    return false;
}

// The clock, or nothing. A device that has never seen a time server would
// otherwise stamp every row with the same confident wrong minute, and a blank
// column is the honest version of not knowing.
static void wd_stamp(char *out, unsigned cap) {
    FwTime t;
    if (!fw_time_get(&t)) { out[0] = 0; return; }
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d",
             t.year, t.month, t.day, t.hour, t.minute, t.second);
}

// An SSID is not a safe CSV field: it can hold a comma or a quote, and a row
// that splits into the wrong number of columns is worse than one that is
// missing. Quoted and doubled, which is what every CSV reader expects.
static void wd_field(char *out, unsigned cap, const char *s) {
    if (!strchr(s, ',') && !strchr(s, '"')) { nova::copy(out, cap, s); return; }
    unsigned n = 0;
    if (n + 1 < cap) out[n++] = '"';
    for (const char *p = s; *p && n + 2 < cap; p++) {
        if (*p == '"' && n + 3 < cap) out[n++] = '"';
        out[n++] = *p;
    }
    if (n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

static bool wd_flush(void) {
    if (!g_wd_len) return true;
    return fw_file_write(g_wd_path, g_wd_buf, g_wd_len) != 0;
}

// Name the next part that does not exist yet, so a second survey never lands on
// the first one's file. Bounded rather than open-ended: a fixed number of
// bounded files is the disk guard the MicroPython survey had, arrived at from
// the other end — it read the filesystem's free space, and a package cannot.
static bool wd_open_part(void) {
    for (int i = g_wd_part; i < WD_PARTS; i++) {
        snprintf(g_wd_path, sizeof(g_wd_path), NOVA_LOGS "/wardrive-%d.csv", i);
        if (fw_file_exists(g_wd_path)) continue;
        g_wd_part = i + 1;
        g_wd_len  = (unsigned)nova::copy(g_wd_buf, WD_BUF, kWdHeader);
        // Written straight away, header and all, so the file exists from the
        // moment the survey starts rather than at the first access point —
        // which is also what stops the next part landing on the same name.
        return wd_flush();
    }
    nova::copy(g_wd_msg, sizeof(g_wd_msg), "Logs full - clear them");
    return false;
}

static bool wd_add(const char *row) {
    unsigned len = (unsigned)strlen(row);
    // No append in the ABI: fw_file_write replaces a file, so a part is held
    // whole in RAM and rewritten. When it will not take another row the survey
    // rolls to the next file rather than stopping — the same continuity the
    // MicroPython one had from appending, within a bound it did not need.
    if (g_wd_len + len + 1 >= WD_BUF) {
        if (!wd_flush()) return false;
        if (!wd_open_part()) return false;
    }
    memcpy(g_wd_buf + g_wd_len, row, len);
    g_wd_len += len;
    g_wd_buf[g_wd_len] = 0;
    return true;
}

static bool survey_live(void) { return g_wd_live; }

static int wardrive_worker(void *) {
    g_wd_live = true;
    while (g_wd_run && !fw_task_should_stop()) {
        int n = fw_net_scan(g_aps, AP_MAX);
        g_wd_pass++;
        // A refusal is usually the radio being busy elsewhere for a moment, so
        // the survey says so and carries on rather than ending on one bad pass.
        if (n < 0) nova::copy(g_wd_msg, sizeof(g_wd_msg), "Scan refused");

        bool wrote = false;
        char ts[24];
        wd_stamp(ts, sizeof(ts));
        for (int i = 0; i < n; i++) {
            if (!g_aps[i].ssid[0] || wd_seen(g_aps[i].ssid)) continue;
            char name[70];
            wd_field(name, sizeof(name), g_aps[i].ssid);
            char row[128];
            snprintf(row, sizeof(row), "%s,%s,%s,%d,%d\n", name,
                     g_aps[i].secured ? "secured" : "open", ts,
                     g_aps[i].channel, g_aps[i].rssi);
            if (!wd_add(row)) { g_wd_run = false; break; }
            g_wd_aps++;
            wrote = true;
        }
        // Written when there is something new, which is also what keeps a
        // stationary survey from rewriting the same file every four seconds.
        if (wrote && !wd_flush()) {
            nova::copy(g_wd_msg, sizeof(g_wd_msg), "Write failed");
            g_wd_run = false;
        }
        gui::invalidate();

        // The wait, in tenths, so stopping is felt at once rather than up to
        // four seconds later.
        for (int i = 0; i < WD_INTERVAL_MS / 100 && g_wd_run && !fw_task_should_stop(); i++)
            fw_task_sleep_ms(100);
    }
    wd_flush();
    g_wd_run  = false;
    g_wd_live = false;
    gui::invalidate();
    return 0;
}

class WardriveScreen : public Screen {
public:
    const char *title(void) const override { return "Wardrive"; }

    int help(const char **out, int max) const override {
        if (max < 4) return 0;
        out[0] = "SELECT starts and stops.";
        out[1] = "Each name is logged once,";
        out[2] = "to /nova/logs/wardrive-N.csv.";
        out[3] = "BACK ends the survey.";
        return 4;
    }

    void enter(void) override { poll_ = 0; }

    // leave() ALSO RUNS WHEN A CHILD IS PUSHED over this screen, not only when
    // this one goes — the pool calls it on the way past. The two are told apart
    // by who is on top: on a real pop the slot has already been handed back, so
    // this screen is no longer it. Without the distinction, opening a notice
    // over a running survey would quietly end it; with only the BACK gesture
    // handled instead, going home from the power menu would leave the worker
    // running with nothing on screen to say so.
    void leave(void) override {
        if (gui::top() == this) return;
        g_wd_run = false;
    }

    bool tick(uint32_t dt) override {
        if (!g_wd_live && !g_wd_run) return false;
        poll_ += dt;
        if (poll_ < 250) return false;
        poll_ = 0;
        return true;
    }

    void draw(Canvas &c) override {
        const int vx = ui::ADV * 7;
        int y = ui::TOP;
        char v[24];

        c.text(0, y, "State", 1);
        c.text(vx, y, g_wd_run ? "recording" : (g_wd_live ? "stopping" : "idle"), 1);
        if (g_wd_live) c.spinner(c.width() - 8, y, fw_millis() / 150, 1);
        y += ui::ROWH;

        snprintf(v, sizeof(v), "%d", g_wd_aps);
        c.text(0, y, "Found", 1);
        c.text(vx, y, v, 1);
        y += ui::ROWH;

        snprintf(v, sizeof(v), "%d", g_wd_pass);
        c.text(0, y, "Passes", 1);
        c.text(vx, y, v, 1);
        y += ui::ROWH;

        c.text(0, y, "Log", 1);
        // The file's name only. The directory is in the help, and the whole path
        // would push the part number off the panel to say what never changes.
        const char *name = g_wd_path + strlen(g_wd_path);
        while (name > g_wd_path && name[-1] != '/') name--;
        c.text_fit(vx, y, g_wd_path[0] ? name : "not started", 1, c.width() - vx, false);

        if (g_wd_msg[0]) c.text_fit(0, c.height() - ui::FH, g_wd_msg, 1, c.width(), false);
    }

    Action on_event(Event e) override {
        if (e == EV_SELECT) {
            if (g_wd_run)       stop();
            else if (!g_wd_live) start();     // still winding down: wait for it
            return ui::ACT_STAY;
        }
        return Screen::on_event(e);
    }

private:
    uint32_t poll_;

    void start(void) {
        if (op_busy()) { nova::copy(g_wd_msg, sizeof(g_wd_msg), "Radio busy"); return; }
        nova::paths_init();
        g_wd_msg[0]  = 0;
        g_wd_seen_n  = 0;
        g_wd_aps     = 0;
        g_wd_pass    = 0;
        g_wd_part    = 0;
        g_wd_len     = 0;
        g_wd_path[0] = 0;
        if (!wd_open_part()) return;
        g_wd_run = true;
        if (fw_task_spawn("novawardrive", wardrive_worker, nullptr, 3072) >= 0) return;
        g_wd_run = false;
        nova::copy(g_wd_msg, sizeof(g_wd_msg), "No task free");
    }

    void stop(void) {
        g_wd_run = false;
        nova::copy(g_wd_msg, sizeof(g_wd_msg), "Stopping...");
    }
};

void open_wardrive(void) { gui::push<WardriveScreen>(); }

// --- LAN ---------------------------------------------------------------------------
//
// What else is on this network.
//
// An ICMP sweep of the /24 our own address sits in, one address at a time,
// because that is the whole of what a package can do. There is no ARP table on
// the ABI, no outbound TCP to knock on a port with, and no reverse DNS anywhere
// in the firmware — `nettcp.cpp` says out loud why it has no connect. So the
// only question this screen can ask a stranger is "are you there", and the only
// interesting thing it gets back is how long the answer took.
//
// THE /24 IS THE SWEEP, NOT AN ASSUMPTION ABOUT THE NETMASK. A /16 is
// sixty-five thousand addresses and cannot be swept from a handheld at all; a
// /28 only means the last forty pings leave the subnet and are answered by
// nobody. So the mask does not change what is done, and the screen says which
// range it covered rather than implying it covered the network.
//
// A host that does not answer ICMP is not listed, and that is a real limitation
// rather than a bug: a Windows machine with its firewall on is invisible here.
// The help says so, because a scanner that quietly under-reports teaches
// somebody the network is emptier than it is.

#define LAN_MAX      32     // hosts kept; a /24 with more than this is rare
#define LAN_PING_MS 200     // per address — see the note at lan_sweep
#define LAN_FIRST     1
#define LAN_LAST    254

#define LAN_ME 1
#define LAN_GW 2

struct LanHost {
    uint8_t  host;      // the last octet; the rest is g_lan_base
    uint8_t  flag;      // LAN_ME, LAN_GW, or neither
    uint16_t ms;        // round trip in milliseconds, 0 for this device
};

// Outside the screen, like everything else a worker writes here: BACK is one
// press and a sweep is the best part of a minute.
static LanHost       g_lan[LAN_MAX];
static volatile int  g_lan_n;
static volatile int  g_lan_at;      // the octet being probed now, for the progress
static volatile bool g_lan_run;     // a worker is in the sweep
static volatile bool g_lan_stop;    // ...and has been asked to come out of it
static volatile int  g_lan_one;     // re-probe just this octet, 0 for a full sweep
static char          g_lan_base[FW_NET_ADDR_MAX];   // "192.168.1."
static int           g_lan_self;    // our own last octet, -1 when unknown
static int           g_lan_gw;      // the gateway's, -1 when it could not be read
static char          g_lan_msg[28];

static bool lan_live(void) { return g_lan_run; }

// "192.168.1.42" -> base "192.168.1." and self 42. False when there is no
// address to work from, which is the offline case and the not-yet-DHCP case
// both — 0.0.0.0 is a link that has joined and has nothing yet.
static bool lan_derive(void) {
    char ip[FW_NET_ADDR_MAX];
    ip[0] = 0;
    if (fw_net_ip(ip, sizeof(ip)) <= 0 || !ip[0]) return false;
    if (!strcmp(ip, "0.0.0.0")) return false;

    int dots = 0, cut = -1;
    for (int i = 0; ip[i]; i++) if (ip[i] == '.') { dots++; cut = i; }
    if (dots != 3 || cut <= 0) return false;

    int last = 0;
    for (const char *p = ip + cut + 1; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        last = last * 10 + (*p - '0');
    }
    if (last < 0 || last > 255) return false;

    // The base keeps its trailing dot so every address is one snprintf.
    unsigned keep = (unsigned)cut + 1;
    if (keep >= sizeof(g_lan_base)) return false;
    memcpy(g_lan_base, ip, keep);
    g_lan_base[keep] = 0;
    g_lan_self = last;
    return true;
}

// The gateway, out of `net`. OPTIONAL, and deliberately so.
//
// It is a tag on one row and nothing depends on it, which is what makes it safe
// to read through the shell: the OS has ONE output capture, so a `pkg install`
// still running from a screen somebody left two presses ago would leave this
// with an empty buffer. That has to degrade to "no gateway tag", not to a wrong
// sweep — which is why the range comes from fw_net_ip and only the tag comes
// from here.
static char g_lan_netout[256];

static void lan_read_gateway(void) {
    g_lan_gw = -1;
    g_lan_netout[0] = 0;
    if (fw_shell_run("net", g_lan_netout, sizeof(g_lan_netout)) != 0) return;

    const char *p = strstr(g_lan_netout, "Gateway");
    if (!p) return;
    p += 7;
    while (*p == ' ') p++;
    // Only a gateway on OUR /24 can be tagged, because the tag is drawn against
    // a row that is one of ours. A router reached over another interface is a
    // true answer to a question this screen is not asking.
    unsigned n = (unsigned)strlen(g_lan_base);
    if (strncmp(p, g_lan_base, n)) return;
    p += n;
    int last = 0;
    if (*p < '0' || *p > '9') return;
    for (; *p >= '0' && *p <= '9'; p++) last = last * 10 + (*p - '0');
    if (last >= LAN_FIRST && last <= LAN_LAST) g_lan_gw = last;
}

// Record a host. The entry is filled BEFORE the count moves, so a UI task that
// sees the new count can already see everything in it — the same single-writer
// order the job runner hands its output over with.
static void lan_add(int host, int ms, int flag) {
    int n = g_lan_n;
    if (n >= LAN_MAX) return;
    g_lan[n].host = (uint8_t)host;
    g_lan[n].flag = (uint8_t)flag;
    g_lan[n].ms   = (uint16_t)(ms < 0 ? 0 : (ms > 9999 ? 9999 : ms));
    g_lan_n = n + 1;
}

// Microseconds to a whole millisecond, never rounding a real reply down to
// nothing: a row reading "0ms" looks like a row that failed to measure.
static int lan_ms(int us) { return us < 1000 ? 1 : us / 1000; }

// The sweep.
//
// 200 ms an address. A LAN answers in single digits and the rest of it is for a
// device in power-save, which can take a hundred milliseconds to come back --
// and a host missed because the wait was too short is the one failure this
// screen cannot show you. 254 addresses is therefore under a minute of wall
// clock in the worst case, which is why the list fills as it goes and why BACK
// gets out of it.
static void lan_sweep(void) {
    g_lan_n = 0;
    g_lan_at = 0;
    g_lan_msg[0] = 0;

    if (!lan_derive()) {
        nova::copy(g_lan_msg, sizeof(g_lan_msg),
                   fw_net_connected() ? "No address yet" : "Not on a network");
        return;
    }
    lan_read_gateway();

    char addr[FW_NET_ADDR_MAX];
    for (int h = LAN_FIRST; h <= LAN_LAST; h++) {
        if (g_lan_stop || fw_task_should_stop()) break;
        g_lan_at = h;

        // Our own address is listed without being probed. Pinging ourselves
        // proves nothing, and leaving the row out would make the one address
        // somebody definitely wants to see the one address missing.
        if (h == g_lan_self) { lan_add(h, 0, LAN_ME); gui::invalidate(); continue; }

        snprintf(addr, sizeof(addr), "%s%d", g_lan_base, h);
        int us = fw_net_ping(addr, LAN_PING_MS);
        if (us >= 0) {
            lan_add(h, lan_ms(us), h == g_lan_gw ? LAN_GW : 0);
            gui::invalidate();
            if (g_lan_n >= LAN_MAX) {
                nova::copy(g_lan_msg, sizeof(g_lan_msg), "Full — first 32 shown");
                break;
            }
        } else if ((h & 0x0f) == 0) {
            // Progress, a few times a sweep rather than every address: the
            // panel is asleep between frames and waking it 254 times to move a
            // counter costs more than the counter is worth.
            gui::invalidate();
        }
    }

    if (!g_lan_msg[0]) {
        if (g_lan_stop)         nova::copy(g_lan_msg, sizeof(g_lan_msg), "Stopped");
        else if (g_lan_n <= 1)  nova::copy(g_lan_msg, sizeof(g_lan_msg), "Nothing else answered");
        else                    g_lan_msg[0] = 0;
    }
    g_lan_at = 0;
}

// One address again, for a row somebody chose. The reading is replaced in place
// rather than appended, so the list keeps its order and a device that has since
// gone quiet says so on its own row instead of vanishing.
static void lan_reprobe(int host) {
    char addr[FW_NET_ADDR_MAX];
    snprintf(addr, sizeof(addr), "%s%d", g_lan_base, host);
    int us = fw_net_ping(addr, LAN_PING_MS);
    for (int i = 0; i < g_lan_n; i++) {
        if (g_lan[i].host != (uint8_t)host) continue;
        if (us >= 0) {
            g_lan[i].ms = (uint16_t)lan_ms(us);
            g_lan_msg[0] = 0;
        } else {
            g_lan[i].ms = 0;
            snprintf(g_lan_msg, sizeof(g_lan_msg), "%s%d: no answer", g_lan_base, host);
        }
        return;
    }
}

// What a row says: a label on the left and its state on the right, the shape
// every list in the suite uses.
//
// Outside the screen so a test can ask it directly. It is the whole of what this
// screen renders — a row that says the wrong thing is the failure a host CAN
// see, and the alternative is asserting against pixels.
//
// The control row reads as a verb and its value as the state, so the row itself
// answers "what will pressing this do" without needing the help overlay.
static void lan_row(int idx, char *label, unsigned lcap, char *value, unsigned vcap) {
    value[0] = 0;
    if (idx == 0) {
        if (g_lan_run) {
            nova::copy(label, lcap, "Stop");
            if (g_lan_at) snprintf(value, vcap, "%d/%d", g_lan_at, LAN_LAST);
            return;
        }
        if (!fw_net_connected()) {
            nova::copy(label, lcap, "Join a network");
            nova::copy(value, vcap, ">");
            return;
        }
        nova::copy(label, lcap, g_lan_n ? "Scan again" : "Scan");
        if (g_lan_n) snprintf(value, vcap, "%d", g_lan_n);
        return;
    }
    if (idx - 1 >= g_lan_n) { nova::copy(label, lcap, ""); return; }
    const LanHost &h = g_lan[idx - 1];
    snprintf(label, lcap, "%s%u", g_lan_base, (unsigned)h.host);
    if (h.flag == LAN_ME)      nova::copy(value, vcap, "me");
    else if (!h.ms)            nova::copy(value, vcap, "--");
    else if (h.flag == LAN_GW) snprintf(value, vcap, "gw %ums", (unsigned)h.ms);
    else                       snprintf(value, vcap, "%ums", (unsigned)h.ms);
}

static int lan_worker(void *) {
    int one = g_lan_one;
    if (one) lan_reprobe(one);
    else     lan_sweep();
    g_lan_one = 0;
    g_lan_run = false;
    gui::invalidate();
    return 0;
}

// Row 0 is the control and the rest are hosts, exactly as the WiFi list puts
// "add by hand" on the front of what a scan found. One list, one cursor, and the
// thing you do most is the row the cursor starts on.
class LanScreen : public Screen {
public:
    const char *title(void) const override { return "LAN"; }

    int help(const char **out, int max) const override {
        if (max < 5) return 0;
        out[0] = "The top row starts and stops.";
        out[1] = "SELECT on a host pings it again.";
        out[2] = "It sweeps the 254 addresses";
        out[3] = "beside this one. A host that";
        out[4] = "ignores ping is not listed.";
        return 5;
    }

    // Nothing is reset here. enter() runs again when a notice pops, and a list
    // somebody waited a minute for must not be thrown away by an acknowledged
    // message. begin() does the resetting, once, on the way in.
    void enter(void) override { poll_ = 0; clamp(); }

    // Leaving ends the sweep. The survey next door deliberately runs on because
    // it is a logger and its whole point is to keep collecting; this is a
    // question somebody asked, and the answer stops mattering when they walk
    // away from it. It also frees the radio, which op_busy() holds for the
    // length of a sweep.
    void leave(void) override { g_lan_stop = true; }

    void begin(void) { sel_ = 0; top_ = 0; poll_ = 0; }

    bool tick(uint32_t dt) override {
        if (!g_lan_run) return false;
        // A few times a second while the worker is out, for the spinner and the
        // count. Not animating(): that would hold the loop at sixty frames for
        // the length of a sweep to turn one glyph.
        poll_ += dt;
        if (poll_ < 250) return false;
        poll_ = 0;
        return true;
    }

    void draw(Canvas &c) override {
        const int n    = rows();
        const int rowsv = ui::rows_for(c);
        if (sel_ < top_)               top_ = sel_;
        else if (sel_ >= top_ + rowsv) top_ = sel_ - rowsv + 1;
        const bool scrolls = n > rowsv;
        const int  right   = scrolls ? c.width() - (ui::SB_W + 1) : c.width();

        char label[24], value[12];
        for (int i = 0; i < rowsv; i++) {
            const int idx = top_ + i;
            if (idx >= n) break;
            const int y = ui::TOP + i * ui::ROWH;
            const bool on = (idx == sel_);
            if (on) c.rounded_rect(0, y - 1, right, ui::ROWH, 1, true);

            lan_row(idx, label, sizeof(label), value, sizeof(value));
            const int w = value[0] ? c.text_width(value, 1, false) : 0;
            c.text_fit(3, y, label, on ? 0 : 1, right - w - 8, false);
            if (value[0]) c.text(right - w - 2, y, value, on ? 0 : 1);
        }

        if (scrolls)
            c.scrollbar(c.width() - ui::SB_W + 1, ui::TOP, c.height() - ui::TOP,
                        top_, rowsv, n);
        else if (!g_lan_n)
            draw_empty(c, ui::TOP + ui::ROWH + 2);

        // The last row of the panel is the status line, the same as the WiFi
        // screen's. It is the only place a reason ever appears, so it is drawn
        // over whatever the list had there rather than beside it.
        if (g_lan_run) c.spinner(c.width() - 8, c.height() - ui::FH, fw_millis() / 120, 1);
        if (g_lan_msg[0])
            c.text_fit(0, c.height() - ui::FH, g_lan_msg, 1, c.width() - 10, false);
    }

    Action on_event(Event e) override {
        const int n = rows();
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % n; return ui::ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + n - 1) % n; return ui::ACT_STAY; }
        if (e == EV_SELECT)  { activate(); return ui::ACT_STAY; }
        return Screen::on_event(e);
    }

private:
    int      sel_, top_;
    uint32_t poll_;

    static int rows(void) { return 1 + g_lan_n; }

    // A panel with one row on it and nothing else says the screen is broken.
    // Before the first sweep there is exactly one row, so the body has to answer
    // the question somebody opened this to ask — either what it is about to
    // sweep, or why it cannot.
    static void draw_empty(Canvas &c, int y) {
        if (!fw_net_connected()) {
            c.text(3, y, "Nothing to scan until", 1);
            c.text(3, y + ui::ROWH, "this joins a network.", 1);
            return;
        }
        char ip[FW_NET_ADDR_MAX], line[26];
        ip[0] = 0;
        if (fw_net_ip(ip, sizeof(ip)) <= 0 || !ip[0]) {
            c.text(3, y, "Joined, no address yet.", 1);
            return;
        }
        c.text(3, y, ip, 1);
        // Built from the address on screen rather than from g_lan_base, which is
        // only filled in once a sweep has started.
        int cut = -1;
        for (int i = 0; ip[i]; i++) if (ip[i] == '.') cut = i;
        if (cut > 0) {
            ip[cut + 1] = 0;
            snprintf(line, sizeof(line), "%s%d-%d", ip, LAN_FIRST, LAN_LAST);
            c.text(3, y + ui::ROWH, line, 1);
        }
    }

    void clamp(void) {
        const int n = rows();
        if (sel_ < 0 || sel_ >= n) sel_ = 0;
        if (top_ < 0 || top_ >= n) top_ = 0;
    }

    void activate(void) {
        if (sel_ == 0) {
            if (g_lan_run) {
                g_lan_stop = true;
                nova::copy(g_lan_msg, sizeof(g_lan_msg), "Stopping...");
                return;
            }
            // Offline is a row that goes somewhere rather than a row that
            // explains itself and stops: the thing to do about no network is
            // one screen away and this is the way to it.
            if (!fw_net_connected()) { open_wifi(); return; }
            start(0);
            return;
        }
        if (sel_ - 1 >= g_lan_n) return;                 // the list shrank under it
        if (g_lan[sel_ - 1].flag == LAN_ME) {
            ui::notice("LAN", "That is this device. Its own address is listed so "
                              "the sweep reads as a complete range.");
            return;
        }
        start(g_lan[sel_ - 1].host);
    }

    // One spawn point for both jobs, so the refusals are written once.
    void start(int one) {
        if (g_lan_run) return;
        if (op_busy()) { nova::copy(g_lan_msg, sizeof(g_lan_msg), "Radio busy"); return; }
        if (one == 0 && !fw_net_connected()) {
            nova::copy(g_lan_msg, sizeof(g_lan_msg), "Not on a network");
            return;
        }
        g_lan_msg[0] = 0;
        g_lan_stop   = false;
        g_lan_one    = one;
        g_lan_run    = true;
        // A full sweep empties the list, so the cursor has nowhere to be but the
        // top. A re-probe changes one reading on one row and MUST leave the
        // cursor on it — jumping to the control row would take somebody off the
        // row whose answer they just asked for.
        if (!one) { sel_ = 0; top_ = 0; }
        // 3072, matching the radio worker: fw_shell_run puts a whole shell
        // command on the calling task's stack, and this one runs `net`.
        if (fw_task_spawn("novalan", lan_worker, nullptr, 3072) >= 0) return;
        g_lan_run = false;
        g_lan_one = 0;
        nova::copy(g_lan_msg, sizeof(g_lan_msg), "No task free");
    }
};

void open_lan(void) {
    LanScreen *s = gui::push<LanScreen>();
    if (s) s->begin();
}

}  // namespace screens
}  // namespace nova
