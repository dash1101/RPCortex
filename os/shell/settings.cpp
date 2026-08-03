// settings — v1's panel, on the TUI layer.
//
// The point of the framework is that this is short. Everything about drawing a
// list, tracking a selection, keeping the cursor on screen and turning a click
// into a row lives in core/tuilist.cpp; what is left here is a table of rows and
// what each one does.
//
// Rows are declarative on purpose. Adding a setting means adding a line to the
// table, not writing another screen.

#include "command.h"
#include "out.h"
#include "tui.h"
#include "tuilist.h"
#include "tuiterm.h"
#include "registry.h"
#include "persist.h"
#include "task.h"
#include "users.h"
#include "session.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum RowKind { ROW_BOOL, ROW_TEXT, ROW_INT, ROW_HEAD, ROW_INFO };

struct Row {
    RowKind     kind;
    const char *label;
    const char *key;      // registry key, or null for a heading
    const char *def;
    const char *help;
};

// The order here is the order on screen. Headings are rows too, which keeps the
// list arithmetic to one kind of thing.
static const Row kRows[] = {
    {ROW_HEAD, "SYSTEM",        nullptr,               nullptr,  nullptr},
    {ROW_TEXT, "Device name",   "System.Device_ID",    "vela",   "Shown in the prompt"},
    {ROW_TEXT, "Owner",         "System.Owner",        "",       "Shown in sysinfo"},
    {ROW_BOOL, "Verbose boot",  "System.Verbose_Boot", "false",  "Print each step at boot"},

    {ROW_HEAD, "TIME",          nullptr,               nullptr,  nullptr},
    {ROW_INT,  "UTC offset",    "System.TZ_Offset",    "0",      "Whole hours, -12 to 14"},
    {ROW_BOOL, "NTP at boot",   "System.NTP_Boot",     "false",  "Set the clock over the network"},

    {ROW_HEAD, "NETWORK",       nullptr,               nullptr,  nullptr},
    {ROW_BOOL, "WiFi autoconnect", "WiFi.Auto",        "false",  "Rejoin a saved network at boot"},

    {ROW_HEAD, "SHELL",         nullptr,               nullptr,  nullptr},
    {ROW_BOOL, "Colour",        "Shell.Color",         "true",   "ANSI colour in output"},
    {ROW_INT,  "History size",  "Shell.History",       "50",     "Lines of command history"},
};
#define N_ROWS ((int)(sizeof(kRows) / sizeof(kRows[0])))

static bool row_selectable(int i) { return kRows[i].kind != ROW_HEAD; }

static void row_value(int i, char *out, uint32_t cap) {
    const Row &r = kRows[i];
    if (!r.key) { out[0] = 0; return; }
    snprintf(out, cap, "%s", reg_get(r.key, r.def));
}

static void label_cb(void *, int i, char *out, uint32_t cap) {
    const Row &r = kRows[i];
    if (r.kind == ROW_HEAD) { snprintf(out, cap, "%s", r.label); return; }

    char val[REG_VAL_MAX];
    row_value(i, val, sizeof(val));
    if (r.kind == ROW_BOOL) {
        bool on = strcmp(val, "true") == 0;
        snprintf(out, cap, "  %-18s [%s]", r.label, on ? "x" : " ");
    } else {
        snprintf(out, cap, "  %-18s %s", r.label, val[0] ? val : "(not set)");
    }
}

// Editing a value happens on the bottom line rather than in a popup: one line
// of screen, no second layout to get right, and the panel stays visible.
static bool edit_value(TuiScreen *s, const Row &r, char *buf, uint32_t cap) {
    uint32_t n = 0;
    snprintf(buf, cap, "%s", reg_get(r.key, r.def));
    n = (uint32_t)strlen(buf);

    while (true) {
        char line[TUI_MAX_W + 1];
        snprintf(line, sizeof(line), " %s: %s", r.label, buf);
        tui_fill(s, 0, s->h - 1, s->w, 1, ' ', TUI_REVERSE, TUI_DEFAULT);
        tui_text(s, 0, s->h - 1, line, TUI_REVERSE, TUI_DEFAULT);
        tui_text_clip(s, s->w - 24, s->h - 1, "enter saves  esc cancels", 24,
                      TUI_REVERSE, TUI_DEFAULT);
        tuiterm_present(s);

        TuiEvent e;
        if (!tuiterm_poll(&e)) { task_sleep_ms(5); continue; }
        if (e.kind != TUI_EV_KEY) continue;

        if (e.key == '\r' || e.key == '\n') return true;
        if (e.key == TUI_KEY_ESCAPE || e.key == 3) return false;
        if ((e.key == 8 || e.key == 0x7f) && n) { buf[--n] = 0; continue; }
        if (e.key >= 32 && e.key < 127 && n + 1 < cap) { buf[n++] = (char)e.key; buf[n] = 0; }
    }
}

static void toggle(const Row &r) {
    bool on = strcmp(reg_get(r.key, r.def), "true") == 0;
    reg_set(r.key, on ? "false" : "true");
}

static int cmd_settings(int, char **) {
    if (!users_is_admin(session_user())) {
        out_err("Only an admin can change settings.");
        return 1;
    }

    static TuiScreen s;
    tuiterm_begin();
    if (!tuiterm_active()) { out_err("Could not start the settings panel."); return 1; }

    uint16_t tw = 80, th = 24;
    tuiterm_size(&tw, &th);
    tui_resize(&s, tw, th);

    TuiList list;
    tuilist_init(&list, N_ROWS, th - 5);
    list.wrap = false;
    // Never start on a heading, which cannot be actioned.
    while (list.sel < N_ROWS && !row_selectable(list.sel)) list.sel++;

    bool dirty = true, running = true, changed = false;
    while (running) {
        if (dirty) {
            dirty = false;
            tui_clear(&s);
            tui_box(&s, 0, 0, s.w, s.h - 1, "Settings", TUI_NORMAL, TUI_CYAN);
            tuilist_draw(&list, &s, 1, 2, s.w - 2, label_cb, nullptr, TUI_DEFAULT);

            // The help for whatever is selected, so the panel explains itself
            // rather than needing documentation beside it.
            const Row &sel = kRows[list.sel];
            tui_fill(&s, 0, s.h - 1, s.w, 1, ' ', TUI_REVERSE, TUI_DEFAULT);
            if (sel.help) tui_text(&s, 1, s.h - 1, sel.help, TUI_REVERSE, TUI_DEFAULT);
            tui_text_clip(&s, s.w - 30, s.h - 1,
                          "enter changes   q saves and quits", 30, TUI_REVERSE, TUI_DEFAULT);
            tuiterm_present(&s);
        }

        TuiEvent e;
        bool busy = false;
        while (tuiterm_poll(&e)) {
            busy = true;
            dirty = true;

            int step = 0;
            if (e.kind == TUI_EV_KEY) {
                switch (e.key) {
                    case TUI_KEY_UP:   step = -1; break;
                    case TUI_KEY_DOWN: step =  1; break;
                    case TUI_KEY_PGUP: tuilist_page(&list, -1); break;
                    case TUI_KEY_PGDN: tuilist_page(&list,  1); break;
                    case TUI_KEY_HOME: tuilist_home(&list); break;
                    case TUI_KEY_END:  tuilist_end(&list);  break;
                    case 'q': case 3: case TUI_KEY_ESCAPE: running = false; break;
                    case '\r': case '\n': case ' ': {
                        const Row &r = kRows[list.sel];
                        if (r.kind == ROW_BOOL) { toggle(r); changed = true; }
                        else if (r.key) {
                            char buf[REG_VAL_MAX];
                            if (edit_value(&s, r, buf, sizeof(buf))) {
                                reg_set(r.key, buf);
                                changed = true;
                            }
                        }
                        break;
                    }
                    default: break;
                }
            } else if (e.kind == TUI_EV_MOUSE) {
                if (e.mouse == TUI_MOUSE_WHEEL_UP)        tuilist_scroll(&list, -2);
                else if (e.mouse == TUI_MOUSE_WHEEL_DOWN) tuilist_scroll(&list,  2);
                else if (e.mouse == TUI_MOUSE_DOWN) {
                    int idx = tuilist_item_at_row(&list, (int)e.y - 2);
                    if (idx >= 0 && row_selectable(idx)) {
                        list.sel = idx;
                        tuilist_clamp(&list);
                    }
                }
            }

            // Skip over headings in whichever direction the move was going, so
            // arrow keys never land somewhere that does nothing.
            if (step) {
                int n = list.sel;
                do { n += step; } while (n >= 0 && n < N_ROWS && !row_selectable(n));
                if (n >= 0 && n < N_ROWS) { list.sel = n; tuilist_clamp(&list); }
            }
        }
        if (!busy) task_sleep_ms(5);
    }

    tuiterm_end();
    if (changed) {
        persist_save_registry();
        out_ok("Settings saved.");
    } else {
        out_info("No changes.");
    }
    return 0;
}

void settings_register(void) {
    static const Command c{"settings", "change system settings in a panel", cmd_settings,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
