// Desc: The Nova D1 entry point — the `d1` command and the pin editor.
// File: novad1.cpp
//
// Orchestration. This is the only file allowed to know about every layer, and
// the only one that registers anything with the shell.
//
// Nova D1 is a PACKAGE. It is installed with `pkg install novad1` and removed
// with `pkg remove novad1`, its commands appear and disappear with it, and a
// version that misbehaves costs a command rather than a device. That was true of
// the MicroPython suite and it stays true here — the alternative, shipping a
// custom firmware build per device, is what makes a thing nobody can install.
#include "rpc_app.h"
#include "novacore.h"
#include "novaboard.h"
#include "novamodtab.h"
#include "novagui.h"
#include "display.h"

#include <string.h>
#include <stdio.h>

RPC_APP_VER("novad1", "0.88.0");

namespace {

using nova::board::PinId;
using nova::board::PIN_COUNT;
using nova::board::PIN_NONE;

const char *source_word(nova::board::Source s) {
    switch (s) {
        case nova::board::SRC_REG:      return "set by hand";
        case nova::board::SRC_PROFILE:  return "board default";
        case nova::board::SRC_FALLBACK: return "caller";
        default:                        return "unassigned";
    }
}

// --- d1 pins ----------------------------------------------------------------

void pins_show(void) {
    fw_printf("%s (%s)\n", nova::board::board_name(), nova::board::board_id());
    const char *det = nova::board::detect();
    if (!det)
        fw_printf("  This board has no Nova D1 profile.\n");
    else if (strcmp(det, nova::board::board_id()) != 0)
        fw_printf("  Detected %s, overridden to %s.\n", det, nova::board::board_id());
    fw_printf("\n");

    for (int i = 0; i < PIN_COUNT; i++) {
        PinId id = (PinId)i;
        int g = nova::board::pin(id);
        if (g == PIN_NONE)
            fw_printf("  %-9s   --   %s\n", nova::board::name(id),
                      source_word(nova::board::source(id)));
        else
            fw_printf("  %-9s   %-2d   %s\n", nova::board::name(id), g,
                      source_word(nova::board::source(id)));
    }
}

int pins_check(void) {
    // Sized for every signal being wrong at once, which is what a map for the
    // wrong board looks like. A report that stops halfway is a report that sends
    // someone back to the bench twice.
    static char report[1024];
    unsigned n = nova::board::check(report, sizeof(report));
    if (!n) { fw_printf("The pin map is valid for this board.\n"); return 0; }
    fw_printf("%s", report);
    fw_printf("\n%u problem%s.\n", n, n == 1 ? "" : "s");
    return 1;
}

int cmd_pins(int argc, char **argv) {
    // argv[0] is "pins" — this is dispatched from cmd_d1, not registered itself.
    if (argc < 2) { pins_show(); return 0; }

    if (!strcmp(argv[1], "check")) return pins_check();

    if (!strcmp(argv[1], "board")) {
        if (argc < 3) {
            fw_printf("Profiles:\n");
            for (unsigned i = 0; i < nova::board::profile_count(); i++) {
                const char *id = nova::board::profile_id(i);
                fw_printf("  %-12s %s\n", id,
                          strcmp(id, nova::board::board_id()) == 0 ? "(in use)" : "");
            }
            fw_printf("  auto         follow what the board reports\n");
            return 0;
        }
        if (!nova::board::board_set(argv[2])) {
            fw_printf("No profile called '%s'.\n", argv[2]);
            return 1;
        }
        nova::reg_save();
        fw_printf("Board profile: %s\n", nova::board::board_id());
        return 0;
    }

    if (!strcmp(argv[1], "set")) {
        if (argc < 4) { fw_printf("Usage: d1 pins set <name> <gpio>\n"); return 1; }
        PinId id = nova::board::by_name(argv[2]);
        if (id == PIN_COUNT) { fw_printf("No pin called '%s'.\n", argv[2]); return 1; }
        int g = 0;
        for (const char *p = argv[3]; *p; p++) {
            if (*p < '0' || *p > '9') { fw_printf("'%s' is not a GPIO number.\n", argv[3]); return 1; }
            g = g * 10 + (*p - '0');
        }
        if ((unsigned)g >= fw_gpio_count()) {
            fw_printf("GPIO %d is not on this board (it has %u).\n", g, fw_gpio_count());
            return 1;
        }
        // Warned, not refused. Somebody deliberately setting a reserved pin on a
        // board they have modified knows more about their hardware than this
        // does; somebody doing it by accident needs to be told, and both are
        // served by saying so and carrying on.
        if (nova::board::reserved(g))
            fw_printf("Note: GPIO %d is normally the board's own.\n", g);
        nova::board::set(id, g);
        nova::reg_save();
        fw_printf("%s is GPIO %d.\n", nova::board::name(id), g);
        return 0;
    }

    if (!strcmp(argv[1], "clear")) {
        if (argc < 3) { fw_printf("Usage: d1 pins clear <name>\n"); return 1; }
        PinId id = nova::board::by_name(argv[2]);
        if (id == PIN_COUNT) { fw_printf("No pin called '%s'.\n", argv[2]); return 1; }
        nova::board::clear(id);
        nova::reg_save();
        int g = nova::board::pin(id);
        if (g == PIN_NONE) fw_printf("%s is unassigned.\n", nova::board::name(id));
        else               fw_printf("%s is back to GPIO %d.\n", nova::board::name(id), g);
        return 0;
    }

    fw_printf("d1 pins [check | board [<id>|auto] | set <name> <gpio> | clear <name>]\n");
    return 1;
}

// --- d1 ---------------------------------------------------------------------

void usage(void) {
    fw_printf("Nova D1\n");
    fw_printf("  d1 gui [--bg]          run the screen, in front or as a service\n");
    fw_printf("  d1 stop                stop it\n");
    fw_printf("  d1 status              what is configured and what answered\n");
    fw_printf("  d1 scan                probe every module\n");
    fw_printf("  d1 perf                what the last second of frames cost\n");
    fw_printf("  d1 display [<kind>]    sh1106 | ssd1306 | ssd1309\n");
    fw_printf("  d1 pins                every pin, its value and where it came from\n");
    fw_printf("  d1 pins check          is this map assignable on this chip\n");
    fw_printf("  d1 pins board [<id>]   list or choose a board profile\n");
    fw_printf("  d1 pins set <n> <g>    override one pin\n");
    fw_printf("  d1 pins clear <n>      drop an override\n");
}

// --- d1 scan -------------------------------------------------------------------

const char *presence_word(nova::Presence p) {
    switch (p) {
        case nova::MOD_PRESENT: return "answered";
        case nova::MOD_ABSENT:  return "no answer";
        case nova::MOD_UNWIRED: return "not wired";
        default:                return "not checked";
    }
}

int cmd_scan(void) {
    nova::modules_scan();
    fw_printf("%-11s %-10s %-7s %s\n", "module", "chip", "bus", "state");
    const nova::Module *m = nova::modules();
    for (unsigned i = 0; i < nova::module_count(); i++) {
        fw_printf("%-11s %-10s %-7s %s\n", m[i].id, m[i].chip,
                  nova::bus_name(m[i].bus), presence_word(nova::module_presence(m[i])));
    }
    fw_printf("\n");
    // The pins each unwired module is waiting on, because "not wired" without
    // saying WHICH wire is a message that sends somebody back to the table.
    for (unsigned i = 0; i < nova::module_count(); i++) {
        if (nova::module_presence(m[i]) != nova::MOD_UNWIRED) continue;
        fw_printf("%s needs:", m[i].id);
        for (unsigned j = 0; j < m[i].npins; j++) {
            if (nova::board::pin(m[i].pins[j]) == PIN_NONE)
                fw_printf(" %s", nova::board::name(m[i].pins[j]));
        }
        fw_printf("\n");
    }
    return 0;
}

// --- d1 gui --------------------------------------------------------------------

int gui_task(void *) {
    nova::gui::run();
    return 0;
}

int cmd_gui(int argc, char **argv) {
    if (nova::gui::running()) { fw_printf("Already running.\n"); return 1; }

    bool panel = nova::gui::begin();
    if (!panel) {
        // Said out loud rather than left as a dark screen. A device with no
        // panel answering on I2C is the ordinary state of a half-built one, and
        // the two pins it is looking at are the useful part of the message.
        fw_printf("No panel answered on I2C (SDA %d, SCL %d).\n",
                  nova::board::pin(nova::board::PIN_SDA),
                  nova::board::pin(nova::board::PIN_SCL));
        fw_printf("Check the wiring, then `d1 scan`. `i2cscan` lists what is there.\n");
        return 1;
    }
    fw_printf("Panel: %s at 0x%02x.\n", nova::display().kind_name(), nova::display().address());

    bool bg = (argc > 2 && (!strcmp(argv[2], "--bg") || !strcmp(argv[2], "-b")));
    if (!bg) {
        fw_printf("Running. BACK on the home screen does nothing; Ctrl+C here stops it.\n");
        nova::gui::run();
        return 0;
    }
    int pid = fw_task_spawn("novagui", gui_task, nullptr, 4096);
    if (pid < 0) { fw_printf("Could not start the screen task.\n"); return 1; }
    fw_printf("Running as task %d.\n", pid);
    return 0;
}

int cmd_perf(void) {
    const nova::gui::Perf &p = nova::gui::perf();
    if (!nova::gui::running()) fw_printf("The screen is not running; these are from last time.\n");
    fw_printf("  frames     %u in the last second\n", p.frames);
    fw_printf("  compose    %u us  (worst %u)\n", p.draw_us, p.draw_us_max);
    fw_printf("  push       %u us, %u page%s of 8\n", p.push_us, p.pages, p.pages == 1 ? "" : "s");
    // The one that says whether the page diff is earning its keep: a full frame
    // is eight pages, and most updates should be one or two.
    fw_printf("\nA full frame is 8 pages. Fewer means the diff is working.\n");
    return 0;
}

int cmd_display(int argc, char **argv) {
    if (argc < 3) {
        fw_printf("Panel: %s\n", nova::display().kind_name());
        fw_printf("These controllers share an I2C address and cannot be told apart\n");
        fw_printf("in software, so this is a setting rather than a guess.\n");
        fw_printf("  d1 display sh1106 | ssd1306 | ssd1309\n");
        return 0;
    }
    if (nova::panel_from_name(argv[2]) == nova::PANEL_AUTO && !nova::ieq(argv[2], "auto")) {
        fw_printf("Not a panel this knows: %s\n", argv[2]);
        return 1;
    }
    nova::reg_set(NOVA_KEY_PREFIX "Display", nova::ieq(argv[2], "auto") ? "" : argv[2]);
    nova::reg_save();
    fw_printf("Panel set to %s. Restart the screen for it to take effect.\n", argv[2]);
    return 0;
}

int cmd_status(void) {
    char b[24];
    fw_board(b, sizeof(b));
    fw_printf("Nova D1 0.88.0 on %s\n", b);
    fw_printf("  profile   %s (%s)\n", nova::board::board_id(), nova::board::board_name());
    fw_printf("  display   %s\n", nova::board::display_bus());
    fw_printf("  storage   %s\n", fw_file_exists(NOVA_ROOT) ? NOVA_ROOT : "not created yet");

    static char report[1024];
    unsigned bad = nova::board::check(report, sizeof(report));
    fw_printf("  pin map   %s\n", bad ? "has problems — `d1 pins check`" : "valid");

    fw_printf("  heap      %u KB free, largest block %u KB\n",
              fw_heap_free() / 1024u, fw_heap_largest() / 1024u);
    return 0;
}

int cmd_d1(int argc, char **argv) {
    if (argc < 2) { usage(); return 0; }
    const char *sub = argv[1];

    if (!strcmp(sub, "help") || !strcmp(sub, "-h") || !strcmp(sub, "--help") || !strcmp(sub, "?")) {
        usage();
        return 0;
    }
    if (!strcmp(sub, "status"))  return cmd_status();
    if (!strcmp(sub, "scan"))    return cmd_scan();
    if (!strcmp(sub, "perf"))    return cmd_perf();
    if (!strcmp(sub, "display")) return cmd_display(argc, argv);
    if (!strcmp(sub, "gui"))     return cmd_gui(argc, argv);
    if (!strcmp(sub, "stop")) {
        if (!nova::gui::running()) { fw_printf("Not running.\n"); return 1; }
        nova::gui::stop();
        fw_printf("Stopping.\n");
        return 0;
    }
    if (!strcmp(sub, "pins"))    return cmd_pins(argc - 1, argv + 1);

    fw_printf("d1: don't know '%s'. Try `d1 help`.\n", sub);
    return 1;
}

}  // namespace

extern "C" int app_main(int arg) {
    (void)arg;
    nova::paths_init();
    rpc_register_command("d1", "Nova D1 - the handheld multi-tool", cmd_d1);
    // Both spellings, because the MicroPython suite answered to both and
    // people's notes and scripts have whichever one they learned first.
    rpc_register_command("novad1", "Nova D1 - the handheld multi-tool", cmd_d1);
    return 0;
}
