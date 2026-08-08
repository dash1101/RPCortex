// Desc: The rest of the `d1` command surface — everything that is not pins or the screen.
// File: novad1cmd.cpp
//
// Split out of novad1.cpp because the command surface is twenty-odd verbs and
// the entry point should stay readable. Everything here is reachable from
// cmd_extra() and nothing else calls into it.
#include "novad1cmd.h"
#include "novacore.h"
#include "novaboard.h"
#include "novagui.h"
#include "novalog.h"
#include "novanotify.h"
#include "display.h"

#include "rpc_app.h"
#include <string.h>
#include <stdio.h>

namespace nova {
namespace cmd {

// --- setup -----------------------------------------------------------------------
//
// One command that turns a board with the OS on it into a Nova D1: make the
// storage, check the pins, and register the screen so it comes up at every boot
// without anyone typing anything.

int setup(void) {
    fw_printf("Nova D1 setup\n\n");

    nova::paths_init();
    fw_printf("  storage    %s\n", fw_file_exists(NOVA_ROOT) ? NOVA_ROOT : "COULD NOT CREATE");

    static char report[1024];
    unsigned bad = board::check(report, sizeof(report));
    if (bad) {
        fw_printf("  pins       %u problem%s — run `novad1 pins check`\n", bad, bad == 1 ? "" : "s");
    } else {
        fw_printf("  pins       %s, valid\n", board::board_id());
    }

    // Registered as a SERVICE rather than a startup command. A service is
    // supervised and restarted; a startup entry runs once and, if it fails, the
    // device comes up with no screen and nothing saying why.
    // 512 rather than 256. Three entries plus a header and a footer is about
    // 180 bytes of text and rather more with the colour still in it, and this
    // is the one function whose whole job is "remove ALL of them" — a truncated
    // list reads as a shorter list and leaves the rest behind.
    static char out[512];
    // EVERY existing entry goes first, then exactly one is added.
    //
    // Without this, running setup twice registered the screen twice, and two
    // service entries both ran `novad1 gui --bg` at the next boot. That is not a
    // cosmetic duplicate: the second runner reset the screen stack under the
    // first and the device jumped to address zero. Adding a service is cheap;
    // adding a second copy of this one is a crash.
    //
    // Entries are removed by INDEX and the list shifts as they go, so this walks
    // downward — removing 1 then 2 removes the wrong second one.
    // RE-READ THE LIST EVERY TIME, and remove the first match.
    //
    // The first version captured the list once and then removed indices from
    // highest to lowest. It dropped one of three. Removing renumbers everything
    // after it, so a single snapshot is stale the moment the first remove
    // succeeds — and scanning a snapshot for "3 " finds text that describes a
    // list which no longer exists.
    //
    // Asking again each time costs three shell round trips on a device with
    // three stale entries, and is right regardless of how the list renumbers.
    int removed = 0;
    for (int guard = 0; guard < 12; guard++) {
        out[0] = 0;
        fw_shell_run("service list", out, sizeof(out));
        if (!out[0]) {
            // Either the command printed nothing, or the OS could not lend out
            // its one capture buffer because something else held it. Both mean
            // the same thing here: there is nothing to read, so read nothing
            // into it rather than concluding the list is empty.
            fw_printf("  service    could not read the list; skipping the tidy-up\n");
            break;
        }

        // The first entry whose command is this package's — somebody else's
        // services are not setup's business.
        int found = nova::listing_index_of(out, "novad1");
        if (found < 0) break;

        char cmd[32];
        snprintf(cmd, sizeof(cmd), "service remove %d", found);
        if (fw_shell_run(cmd, nullptr, 0) != 0) break;   // stop rather than spin
        removed++;
    }
    if (removed) fw_printf("  service    dropped %d stale entr%s\n",
                           removed, removed == 1 ? "y" : "ies");

    // `service add <command>` — the whole rest of the line IS the command, with
    // no name in front of it. Passing one made the service "novad1 novad1 gui
    // --bg", which is not a command and failed at every boot saying so.
    int rc = fw_shell_run("service add novad1 gui --bg", out, sizeof(out));
    if (rc == 0) {
        fw_printf("  service    registered — the screen starts at every boot\n");
    } else {
        fw_printf("  service    could not register\n");
        if (out[0]) fw_printf("             %s", out);
        // `service` is admin-only, and fw_shell_run runs as whoever is logged
        // in. That is the usual reason and it is not obvious from the message.
        fw_printf("             (adding a service needs an admin session)\n");
    }

    fw_printf("\nReboot, or `novad1 gui` to start it now.\n");
    log::write("setup ran");
    return bad ? 1 : 0;
}

// --- selftest ----------------------------------------------------------------------
//
// The screens run a shell command on a task of their own — spawn, call
// fw_shell_run there, poll a flag — and several of them came back with nothing
// on a real device while the same command typed at the prompt worked perfectly.
// That difference is the whole bug and it cannot be seen from a command running
// on the shell task, so this runs the screens' path and reports what it got.

static char          g_st_out[512];
static volatile bool g_st_done;
static volatile int  g_st_rc;
static const char   *g_st_cmd;

static int selftest_task(void *) {
    g_st_out[0] = 0;
    g_st_rc = fw_shell_run(g_st_cmd, g_st_out, sizeof(g_st_out));
    g_st_done = true;
    return 0;
}

static void try_one(const char *line) {
    g_st_cmd  = line;
    g_st_done = false;
    g_st_rc   = -999;

    int pid = fw_task_spawn("d1self", selftest_task, nullptr, 4096);
    if (pid < 0) { fw_printf("  %-16s SPAWN FAILED\n", line); return; }

    for (int i = 0; i < 200 && !g_st_done; i++) fw_task_sleep_ms(25);
    if (!g_st_done) { fw_printf("  %-16s never finished (task %d)\n", line, pid); return; }

    unsigned n = 0, lines = 0;
    for (const char *p = g_st_out; *p; p++) { n++; if (*p == '\n') lines++; }
    fw_printf("  %-16s rc %-4d %4u bytes, %u line%s%s\n", line, g_st_rc, n, lines,
              lines == 1 ? "" : "s", n ? "" : "   <-- NOTHING CAME BACK");
    if (n) {
        // The first line, so the shape of what came back is visible and not
        // just its size — a buffer of the right length holding the wrong thing
        // is the other way this fails.
        char first[72];
        unsigned i = 0;
        while (i + 1 < sizeof(first) && g_st_out[i] && g_st_out[i] != '\n') { first[i] = g_st_out[i]; i++; }
        first[i] = 0;
        fw_printf("                   | %s\n", first);
    }
}

int selftest(void) {
    fw_printf("Running each command the way a SCREEN does — on its own task.\n\n");
    fw_printf("  ON THIS TASK (the shell), for comparison:\n");
    static char here[512];
    here[0] = 0;
    int rc = fw_shell_run("ps", here, sizeof(here));
    fw_printf("  %-16s rc %-4d %4u bytes%s\n\n", "ps", rc, (unsigned)strlen(here),
              here[0] ? "" : "   <-- NOTHING CAME BACK");

    fw_printf("  ON A SPAWNED TASK, which is what every screen does:\n");
    try_one("ps");
    try_one("service list");
    try_one("pkg list");
    try_one("df");
    try_one("free");
    fw_printf("\nA screen reading a listing is only as good as the lines above.\n");
    return 0;
}

// --- tap ---------------------------------------------------------------------------

int tap(int argc, char **argv) {
    if (!gui::running()) {
        fw_printf("The screen is not running. `novad1 gui --bg` first.\n");
        return 1;
    }
    if (argc < 3) {
        fw_printf("novad1 tap cw | ccw | sel | hold | back | home | homehold [n]\n");
        fw_printf("  Injects the gesture the encoder would have made.\n");
        return 1;
    }
    const char *what = argv[2];
    Event e = EV_NONE;
    if      (!strcmp(what, "cw"))       e = EV_ROT_CW;
    else if (!strcmp(what, "ccw"))      e = EV_ROT_CCW;
    else if (!strcmp(what, "sel"))      e = EV_SELECT;
    else if (!strcmp(what, "hold"))     e = EV_SELECT_HOLD;
    else if (!strcmp(what, "back"))     e = EV_BACK;
    else if (!strcmp(what, "home"))     e = EV_HOME;
    else if (!strcmp(what, "homehold")) e = EV_HOME_HOLD;
    else { fw_printf("Not a gesture: %s\n", what); return 1; }

    // Parsed here rather than with atoi, which the firmware does not export to
    // packages — and adding it to the ABI for one repeat count would be a poor
    // trade. Anything that is not a number reads as one gesture, which is the
    // safe way to be wrong about a repeat.
    int n = 1;
    if (argc > 3) {
        n = 0;
        for (const char *p = argv[3]; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
        if (n < 1) n = 1;
    }
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        input().inject(e);
        // A detent at a time, with a frame between, because the runner takes at
        // most one rotation per frame on purpose — injecting sixteen at once
        // would queue them and arrive as a spin rather than as sixteen steps.
        fw_task_sleep_ms(40);
    }
    // Long enough for the frame after the last gesture to have been drawn, so
    // what is reported below is what is on the glass.
    fw_task_sleep_ms(120);

    ui::Screen *s = gui::top();
    fw_printf("%-9s x%-2d  ->  depth %u, showing '%s'\n", what, n,
              gui::depth(), s ? s->title() : "(nothing)");
    return 0;
}

// --- shot --------------------------------------------------------------------------

int shot(void) {
    if (!gui::running()) {
        fw_printf("The screen is not running. `novad1 gui --bg` first.\n");
        return 1;
    }
    Canvas &c = gui::canvas();
    ui::Screen *s = gui::top();
    fw_printf("%dx%d  depth %u  '%s'\n", c.width(), c.height(), gui::depth(),
              s ? s->title() : "");

    // A row at a time. The frame is live and the runner may redraw underneath
    // this, so a torn picture is possible — and is worth far more than no
    // picture, which is what there was.
    char row[160];
    for (int y = 0; y < c.height(); y++) {
        int n = 0;
        for (int x = 0; x < c.width() && n < (int)sizeof(row) - 1; x++)
            row[n++] = c.get(x, y) ? '#' : '.';
        row[n] = 0;
        fw_printf("%s\n", row);
    }
    return 0;
}

// --- service ---------------------------------------------------------------------

static int gui_task(void *) {
    gui::run();
    return 0;
}

int screen_start(bool bg) {
    if (gui::started()) {
        // A BACKGROUND start that finds the screen already up has SUCCEEDED at
        // what it was asked to do, and says nothing.
        //
        // Three duplicate service entries each printed two lines of complaint
        // over the login prompt at every boot, and the shell had to be nudged
        // with a return key to come back. A service arriving to find its job
        // already done is not an error, and the console belongs to the person
        // sitting at it.
        if (bg) return 0;
        fw_printf("The screen is already running.\n");
        fw_printf("  novad1 service restart   to start it again with new settings\n");
        return 1;
    }

    if (!gui::begin()) {
        // Said out loud rather than left as a dark screen. A device with no
        // panel answering on I2C is the ordinary state of a half-built one, and
        // the two pins it is looking at are the useful part of the message.
        fw_printf("No panel answered on I2C (SDA %d, SCL %d).\n",
                  board::pin(board::PIN_SDA), board::pin(board::PIN_SCL));
        fw_printf("Check the wiring, then `novad1 scan`. `i2cscan` lists what is there.\n");
        return 1;
    }
    fw_printf("Panel: %s at 0x%02x.\n", display().kind_name(), display().address());

    if (!bg) {
        fw_printf("Running. BACK on the home screen does nothing; Ctrl+C here stops it.\n");
        gui::run();
        return 0;
    }
    int pid = fw_task_spawn("novagui", gui_task, nullptr, 4096);
    if (pid < 0) { fw_printf("Could not start the screen task.\n"); return 1; }
    fw_printf("Running as task %d.\n", pid);
    return 0;
}

int service(int argc, char **argv) {
    const char *sub = argc > 2 ? argv[2] : "status";
    char out[256];

    if (!strcmp(sub, "status")) {
        fw_printf("Nova D1 screen: %s\n", gui::running() ? "running" : "stopped");
        fw_shell_run("service list", out, sizeof(out));
        fw_printf("%s", out);
        return 0;
    }
    if (!strcmp(sub, "start")) {
        if (gui::started()) { fw_printf("The screen is already running.\n"); return 1; }
        return screen_start(true);
    }
    if (!strcmp(sub, "stop")) {
        if (!gui::running()) { fw_printf("Not running.\n"); return 1; }
        gui::stop();
        fw_printf("Stopping.\n");
        return 0;
    }
    if (!strcmp(sub, "restart") || !strcmp(sub, "refresh") || !strcmp(sub, "reload")) {
        // refresh is restart under another name, because what people mean by
        // "reload the pins" is "start the screen again with the new ones" —
        // there is no way to re-pin a running panel that is worth the code.
        if (gui::started()) {
            gui::stop();
            // The loop checks its flag once a frame, and the longest nap is
            // 300 ms. Waiting is what makes `restart` mean restart rather than
            // "ask it to stop, then race it".
            for (int i = 0; i < 40 && gui::started(); i++) fw_task_sleep_ms(50);
        }
        return screen_start(true);
    }
    fw_printf("novad1 service start | stop | restart | status\n");
    return 1;
}

// --- style, apps, favourites ------------------------------------------------------

int style(int argc, char **argv) {
    if (argc < 3) {
        fw_printf("Home style: %s\n", nova::reg(NOVA_KEY_PREFIX "HomeStyle", "folders"));
        fw_printf("  novad1 style folders | gallery | menu\n");
        fw_printf("    folders   apps grouped, one icon per group (default)\n");
        fw_printf("    gallery   every app in one ring of icons\n");
        fw_printf("    menu      a plain list\n");
        return 0;
    }
    const char *s = argv[2];
    const char *set = (*s == 'f') ? "folders" : (*s == 'g') ? "gallery" : (*s == 'm') ? "menu" : nullptr;
    if (!set) { fw_printf("Not a style: %s\n", s); return 1; }
    nova::reg_set(NOVA_KEY_PREFIX "HomeStyle", set);
    nova::reg_save();
    fw_printf("Home style: %s. Re-open the screen for it.\n", set);
    return 0;
}

// The setting stores what is HIDDEN, not what is shown.
//
// The MicroPython suite stored the enabled list, and that does not survive the
// move: a registry value here is capped at 96 characters, and thirty-two app
// keys are about three hundred and fifty. Writing the shown list would truncate
// it, and every app past the cutoff would vanish from the home screen with the
// setting looking perfectly reasonable.
//
// Hidden is a handful of keys at most, and it has the better default anyway —
// nothing hidden is the empty string, so a device that has never been configured
// and a device whose setting got lost both show everything rather than nothing.
static bool home_shows(const char *key) {
    return !nova::csv_has(nova::reg(NOVA_KEY_PREFIX "Hidden", ""), key);
}

int apps(int argc, char **argv) {
    const gui::App *a = gui::apps();
    unsigned n = gui::app_count();

    if (argc < 3) {
        fw_printf("%-14s %-9s %-6s %s\n", "app", "folder", "home", "state");
        for (unsigned i = 0; i < n; i++) {
            const char *state = !a[i].open ? "not built"
                              : gui::app_available(a[i]) ? "ready" : "no module";
            fw_printf("%-14s %-9s %-6s %s\n", a[i].key, category_name(a[i].cat),
                      home_shows(a[i].key) ? "yes" : "no", state);
        }
        fw_printf("\n  novad1 apps show <key> | hide <key> | reset\n");
        return 0;
    }

    if (!strcmp(argv[2], "reset")) {
        nova::reg_set(NOVA_KEY_PREFIX "Hidden", "");
        nova::reg_save();
        fw_printf("Every app shows again.\n");
        return 0;
    }
    if (argc < 4) { fw_printf("novad1 apps show|hide <key>\n"); return 1; }

    // Check the key exists before writing it. A typo saved into the list is a
    // setting that does nothing and gives no hint why.
    bool known = false;
    for (unsigned i = 0; i < n; i++) if (!strcmp(a[i].key, argv[3])) known = true;
    if (!known) { fw_printf("No app called '%s'. `novad1 apps` lists them.\n", argv[3]); return 1; }

    bool hide = !strcmp(argv[2], "hide");
    char csv[NOVA_VAL_MAX];
    nova::copy(csv, sizeof(csv), nova::reg(NOVA_KEY_PREFIX "Hidden", ""));
    bool changed = hide ? nova::csv_add(csv, sizeof(csv), argv[3])
                        : nova::csv_remove(csv, sizeof(csv), argv[3]);
    if (!changed) { fw_printf("Already %s.\n", hide ? "hidden" : "shown"); return 0; }
    nova::reg_set(NOVA_KEY_PREFIX "Hidden", csv);
    nova::reg_save();
    fw_printf("%s is %s on the home screen.\n", argv[3], hide ? "hidden" : "shown");
    return 0;
}

int fav(int argc, char **argv) {
    char csv[NOVA_VAL_MAX * 2];
    nova::copy(csv, sizeof(csv), nova::reg(NOVA_KEY_PREFIX "Favorites", ""));

    if (argc < 3 || !strcmp(argv[2], "list")) {
        if (!csv[0]) {
            fw_printf("No favourites. The bar above the folders is empty.\n");
            fw_printf("  novad1 fav add <key>   — `d1 apps` lists the keys\n");
            return 0;
        }
        fw_printf("Favourites: %s\n", csv);
        return 0;
    }
    if (argc < 4) { fw_printf("novad1 fav add|remove <key> | list | clear\n"); return 1; }

    if (!strcmp(argv[2], "clear")) {
        nova::reg_set(NOVA_KEY_PREFIX "Favorites", "");
        nova::reg_save();
        fw_printf("Favourites cleared.\n");
        return 0;
    }

    const gui::App *a = gui::apps();
    bool known = false;
    for (unsigned i = 0; i < gui::app_count(); i++) if (!strcmp(a[i].key, argv[3])) known = true;
    if (!known) { fw_printf("No app called '%s'. `novad1 apps` lists them.\n", argv[3]); return 1; }

    bool changed = !strcmp(argv[2], "remove") ? nova::csv_remove(csv, sizeof(csv), argv[3])
                                              : nova::csv_add(csv, sizeof(csv), argv[3]);
    if (!changed) { fw_printf("Nothing changed.\n"); return 0; }
    nova::reg_set(NOVA_KEY_PREFIX "Favorites", csv);
    nova::reg_save();
    fw_printf("Favourites: %s\n", csv[0] ? csv : "(none)");
    return 0;
}

// --- the lock ----------------------------------------------------------------------

int lock(int argc, char **argv) {
    const char *kind = nova::reg(NOVA_KEY_PREFIX "Lock_Kind", "none");

    if (argc < 3) {
        fw_printf("Lock: %s\n", kind);
        int secs = nova::reg_int(NOVA_KEY_PREFIX "LockSec", 0);
        if (secs) fw_printf("  locks itself after %ds idle\n", secs);
        fw_printf("  novad1 pin set <6 digits> | clear | auto <seconds>\n");
        return 0;
    }

    if (!strcmp(argv[2], "clear")) {
        nova::reg_set(NOVA_KEY_PREFIX "PIN", "");
        nova::reg_set(NOVA_KEY_PREFIX "Lock_Kind", "none");
        nova::reg_save();
        fw_printf("Lock removed.\n");
        log::write("screen lock removed");
        return 0;
    }

    if (!strcmp(argv[2], "auto")) {
        if (argc < 4) { fw_printf("novad1 pin auto <seconds>, or 0 to never\n"); return 1; }
        int s = 0;
        for (const char *p = argv[3]; *p; p++) {
            if (*p < '0' || *p > '9') { fw_printf("Not a number: %s\n", argv[3]); return 1; }
            s = s * 10 + (*p - '0');
        }
        nova::reg_set_int(NOVA_KEY_PREFIX "LockSec", s);
        nova::reg_save();
        if (s) fw_printf("Locks itself after %ds idle.\n", s);
        else   fw_printf("Never locks itself.\n");
        return 0;
    }

    if (!strcmp(argv[2], "set")) {
        if (argc < 4) { fw_printf("novad1 pin set <6 digits>\n"); return 1; }
        const char *pin = argv[3];
        unsigned n = (unsigned)strlen(pin);
        if (n != 6) { fw_printf("A PIN is six digits; that is %u.\n", n); return 1; }
        for (unsigned i = 0; i < n; i++)
            if (pin[i] < '0' || pin[i] > '9') { fw_printf("Digits only.\n"); return 1; }
        nova::reg_set(NOVA_KEY_PREFIX "PIN", pin);
        nova::reg_set(NOVA_KEY_PREFIX "Lock_Kind", "pin");
        nova::reg_save();
        // The PIN is not echoed back. It is stored in the registry in the clear
        // — this locks a screen, it does not protect a filesystem, and saying so
        // is better than implying otherwise.
        fw_printf("PIN set. It guards the screen, not the shell or the files.\n");
        log::write("screen lock set");
        return 0;
    }
    fw_printf("novad1 pin set <6 digits> | clear | auto <seconds>\n");
    return 1;
}

// --- incognito ---------------------------------------------------------------------

int incognito(int argc, char **argv) {
    const char *sub = argc > 2 ? argv[2] : "status";
    char out[192];

    if (!strcmp(sub, "status")) {
        fw_shell_run("radio", out, sizeof(out));
        fw_printf("%s", out);
        return 0;
    }
    if (!strcmp(sub, "on")) {
        // THE LOCK LIVES UNDER THE OS NETWORK LAYER, not in this package.
        //
        // The MicroPython version learned this the hard way: it was a flag Nova
        // D1 code checked, so anything else — the shell's own `wifi scan`, most
        // obviously — went straight past it and brought the radio back up. The
        // OS latch has no path around it and survives a reboot, because a
        // privacy switch that forgets itself is not one.
        int rc = fw_shell_run("radio off", out, sizeof(out));
        fw_printf("%s", out);
        if (rc == 0) {
            notify::post("Incognito on");
            log::write("incognito on");
        }
        return rc;
    }
    if (!strcmp(sub, "off")) {
        int rc = fw_shell_run("radio on", out, sizeof(out));
        fw_printf("%s", out);
        if (rc == 0) log::write("incognito off");
        return rc;
    }
    fw_printf("novad1 incognito on | off | status\n");
    return 1;
}

// --- logs and notifications ----------------------------------------------------------

int logs(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[2], "clear")) {
        log::clear();
        fw_printf("Log cleared.\n");
        return 0;
    }
    int want = 20;
    if (argc > 2) {
        int v = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        if (v > 0) want = v;
    }
    int n = log::count();
    if (!n) { fw_printf("Nothing logged yet.\n"); return 0; }
    if (want > n) want = n;
    // Oldest of the requested window first, so it reads downward like a
    // transcript rather than backwards.
    for (int i = want - 1; i >= 0; i--) {
        char line[96];
        if (log::line(i, line, sizeof(line))) fw_printf("%s\n", line);
    }
    return 0;
}

int notifications(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[2], "clear")) {
        notify::clear();
        fw_printf("Cleared.\n");
        return 0;
    }
    if (argc > 2) {
        // Everything after the verb, joined — a notification is a sentence and
        // quoting it should not be required.
        char text[notify::TEXT_MAX];
        unsigned at = 0;
        for (int i = 2; i < argc && at + 1 < sizeof(text); i++) {
            if (at) text[at++] = ' ';
            at += nova::copy(text + at, sizeof(text) - at, argv[i]);
        }
        notify::post(text);
        fw_printf("Sent to the screen.\n");
        return 0;
    }
    int n = notify::count();
    if (!n) { fw_printf("No notifications.\n"); return 0; }
    fw_printf("%d notification%s, %d unread\n\n", n, n == 1 ? "" : "s", notify::unread());
    for (int i = 0; i < n; i++) {
        char t[notify::TEXT_MAX];
        if (notify::at(i, t, sizeof(t))) fw_printf("  %s\n", t);
    }
    return 0;
}

// --- wifiprobe -------------------------------------------------------------------------

int wifiprobe(void) {
    // The MicroPython version probed the firmware for a promiscuous API, because
    // on an ESP32 the answer depended on which build was flashed. On this
    // hardware it does not depend on anything: the CYW43439 has no monitor mode
    // exposed by its driver, and no build of this OS changes that.
    //
    // Kept as a command because the QUESTION keeps being asked, and an answer
    // that says why is worth more than a missing command.
    char b[24];
    fw_board(b, sizeof(b));
    fw_printf("802.11 capture on %s: no.\n\n", b);
    fw_printf("The radio is a CYW43439 and its driver exposes no monitor mode, so\n");
    fw_printf("raw frames never reach this side. That rules out packet capture,\n");
    fw_printf("probe-request capture, deauthentication detection, and spotting a\n");
    fw_printf("phone passively as it walks past. None of those are backlog items.\n\n");
    fw_printf("What DOES work:\n");
    fw_printf("  wifi scan        access points — BSSID, SSID, channel, signal\n");
    fw_printf("  novad1 wardrive      the same, logged to a WiGLE CSV with a position\n");
    fw_printf("  Bluetooth        nearly everything advertises, and the payload\n");
    fw_printf("                   says what a device is rather than that it exists\n\n");
    fw_printf("Capture belongs to a device with a radio that supports it.\n");
    return 0;
}

// --- selfupdate --------------------------------------------------------------------

int selfupdate(void) {
    char have[24] = "";
    fw_printf("Nova D1 update\n\n");
    // Straight through the package manager rather than a downloader of its own.
    // It verifies a SHA-256 against the index and validates the package by
    // actually loading it before installing — none of which is worth a second,
    // worse implementation inside the thing being replaced.
    (void)have;
    char out[512];
    int rc = fw_shell_run("pkg install novad1", out, sizeof(out));
    fw_printf("%s", out);
    if (rc == 0) fw_printf("\nRestart the screen: `d1 service restart`.\n");
    return rc;
}

}  // namespace cmd
}  // namespace nova
