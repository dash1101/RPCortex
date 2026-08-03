// tuidemo — a worked example of a full-screen package, and a way to check that
// the TUI actually behaves on a real terminal.
//
// It exists for two audiences. Someone writing a package gets a complete app
// that is short enough to read in one sitting: draw, present, poll, repeat.
// Someone testing a board gets every input path exercised at once — arrows,
// page keys, Home and End, Escape, and the mouse, with what arrived shown on
// screen so a terminal that reports something unexpected says so immediately.
//
// The mouse is the part most worth seeing work. Clicking a row selects it, the
// wheel scrolls, and both come down the same serial line as the keys.

#include "rpc_app.h"

RPC_APP_VER("tuidemo", "1.0");

#define ITEMS 40

// --- a scrolling list, kept deliberately small ------------------------------
//
// The firmware has a list widget, but a package cannot call it — the ABI
// exposes drawing, not widgets. Doing the arithmetic here is the honest example
// of what writing a package involves today, and it is about fifteen lines.
typedef struct {
    int count, sel, top, rows;
} List;

static void list_clamp(List *l) {
    if (l->sel < 0) l->sel = l->count - 1;
    if (l->sel >= l->count) l->sel = 0;
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + l->rows) l->top = l->sel - l->rows + 1;
    int max_top = l->count - l->rows;
    if (max_top < 0) max_top = 0;
    if (l->top > max_top) l->top = max_top;
    if (l->top < 0) l->top = 0;
}

static void list_scroll(List *l, int d) {
    int max_top = l->count - l->rows;
    if (max_top < 0) max_top = 0;
    l->top += d;
    if (l->top < 0) l->top = 0;
    if (l->top > max_top) l->top = max_top;
}

// --- small helpers ----------------------------------------------------------

static void num_to_str(int v, char *out) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0;
    while (n > 0) out[i++] = tmp[--n];
    out[i] = 0;
}

static void str_copy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = 0;
}

static void str_append(char *dst, const char *src) {
    while (*dst) dst++;
    str_copy(dst, src);
}

// --- the app ----------------------------------------------------------------

static const char *KIND[] = {"press", "release", "drag", "wheel up", "wheel down"};

static int cmd_tuidemo(int, char **) {
    int w = 80, h = 24;
    char last_event[64];
    str_copy(last_event, "nothing yet - press a key or click");

    fw_tui_begin();
    fw_tui_size(&w, &h);
    if (w < 20) { w = 80; h = 24; }        // begin failed; draw something sane

    List list;
    list.count = ITEMS;
    list.sel = 0;
    list.top = 0;
    // Fill the window it was actually given: everything between the header and
    // the status bar. A fixed row count either wastes a maximised terminal or
    // draws past the bottom of a small one.
    list.rows = h - 6;
    if (list.rows < 1) list.rows = 1;

    // Measured, not guessed. A number on screen settles "is it fast now"
    // without anyone having to describe how it feels.
    unsigned frames = 0, fps = 0;
    unsigned fps_at = fw_millis();
    unsigned draw_ms = 0;

    int running = 1;
    int dirty = 1;                 // draw the first frame, then only on change
    while (running) {
        // Redraw only when something moved. An unchanged frame still costs a
        // full comparison of every cell, and at this loop rate that is the
        // difference between responsive and not.
        if (dirty) {
        dirty = 0;
        unsigned t0 = fw_millis();

        // --- draw -----------------------------------------------------------
        fw_tui_clear();
        fw_tui_box(0, 0, w, h - 1, "tuidemo - the TUI, exercised", FW_ATTR_NORMAL, 6);

        fw_tui_text(2, 1, "arrows move.  click a row.  wheel scrolls.  ^L after resizing.  q quits.",
                    FW_ATTR_DIM, 0);

        for (int r = 0; r < list.rows; r++) {
            int idx = list.top + r;
            int y = r + 3;
            if (idx >= list.count) continue;

            int selected = (idx == list.sel);
            unsigned char attr = selected ? FW_ATTR_REVERSE : FW_ATTR_NORMAL;

            // Fill first, then write over it, so the highlight reaches the full
            // width instead of stopping where the text does.
            fw_tui_fill(2, y, w - 4, 1, ' ', attr, 0);

            char line[64];
            str_copy(line, "  item ");
            char n[12]; num_to_str(idx, n);
            str_append(line, n);
            if (idx % 7 == 0) str_append(line, "   (every seventh, for colour)");
            fw_tui_text(2, y, line, attr, (unsigned char)(idx % 7 == 0 ? 3 : 0));
        }

        // A scrollbar, so the wheel visibly does something.
        if (list.count > list.rows) {
            int span = list.rows;
            int thumb = (list.top * span) / list.count;
            for (int r = 0; r < span; r++)
                fw_tui_text(w - 3, r + 3, r == thumb ? "#" : "|", FW_ATTR_DIM, 0);
        }

        char status[96];
        str_copy(status, " last: ");
        str_append(status, last_event);
        fw_tui_fill(0, h - 1, w, 1, ' ', FW_ATTR_REVERSE, 0);
        fw_tui_text(0, h - 1, status, FW_ATTR_REVERSE, 0);
        // Frames drawn in the last second, and how long the last one took.
        char perf[40];
        char n2[12];
        str_copy(perf, "");
        num_to_str(fps, n2);   str_append(perf, n2); str_append(perf, " fps  ");
        num_to_str(draw_ms, n2); str_append(perf, n2); str_append(perf, " ms   q quits");
        fw_tui_text(w - 26, h - 1, perf, FW_ATTR_REVERSE, 0);

        fw_tui_present();
        draw_ms = fw_millis() - t0;
        frames++;
        }   // if (dirty)

        // Once a second, so the figure is readable rather than flickering.
        unsigned now = fw_millis();
        if (now - fps_at >= 1000) {
            fps = frames;
            frames = 0;
            fps_at = now;
            dirty = 1;
        }

        // --- input ----------------------------------------------------------
        FwTuiEvent e;
        int busy = 0;
        while (fw_tui_poll(&e)) {
            busy = 1;
            dirty = 1;
            if (e.kind == 1) {                      // key
                char n[12];
                switch (e.key) {
                    case FW_KEY_UP:    list.sel--; list_clamp(&list); str_copy(last_event, "key up"); break;
                    case FW_KEY_DOWN:  list.sel++; list_clamp(&list); str_copy(last_event, "key down"); break;
                    case FW_KEY_PGUP:  list.sel -= list.rows - 1; if (list.sel < 0) list.sel = 0;
                                       list_clamp(&list); str_copy(last_event, "page up"); break;
                    case FW_KEY_PGDN:  list.sel += list.rows - 1; if (list.sel >= list.count) list.sel = list.count - 1;
                                       list_clamp(&list); str_copy(last_event, "page down"); break;
                    case FW_KEY_HOME:  list.sel = 0; list.top = 0; str_copy(last_event, "home"); break;
                    case FW_KEY_END:   list.sel = list.count - 1; list_clamp(&list); str_copy(last_event, "end"); break;
                    case FW_KEY_LEFT:  str_copy(last_event, "key left"); break;
                    case FW_KEY_RIGHT: str_copy(last_event, "key right"); break;
                    case FW_KEY_ESC:   str_copy(last_event, "escape (its timeout works)"); break;
                    case 'q': case 3:  running = 0; break;
                    case 12:           // Ctrl+L — after resizing the window
                        if (fw_tui_refresh()) {
                            fw_tui_size(&w, &h);
                            list.rows = h - 6;
                            if (list.rows < 1) list.rows = 1;
                            list_clamp(&list);
                        }
                        str_copy(last_event, "refreshed");
                        break;
                    default:
                        // Anything else: show the byte, so a terminal sending
                        // something unexpected reports itself instead of
                        // appearing to do nothing.
                        str_copy(last_event, "key 0x");
                        num_to_str(e.key, n);
                        str_append(last_event, n);
                        if (e.ctrl)  str_append(last_event, " +ctrl");
                        if (e.shift) str_append(last_event, " +shift");
                        if (e.alt)   str_append(last_event, " +alt");
                        break;
                }
            } else if (e.kind == 2) {               // mouse
                char n[12];
                str_copy(last_event, "mouse ");
                str_append(last_event, e.mouse < 5 ? KIND[e.mouse] : "?");
                str_append(last_event, " at ");
                num_to_str(e.x, n); str_append(last_event, n);
                str_append(last_event, ",");
                num_to_str(e.y, n); str_append(last_event, n);

                if (e.mouse == 3) list_scroll(&list, -3);
                else if (e.mouse == 4) list_scroll(&list, 3);
                else if (e.mouse == 0) {
                    // A click on a list row selects it. Rows start at y=3, which
                    // is the one piece of arithmetic a click has to get right.
                    int row = (int)e.y - 3;
                    if (row >= 0 && row < list.rows) {
                        int idx = list.top + row;
                        if (idx < list.count) {
                            list.sel = idx;
                            str_append(last_event, "  -> selected");
                        }
                    }
                }
            }
        }

        // Yield rather than spin. This is a foreground app on a cooperative
        // scheduler: without it nothing else on the device gets a turn, and the
        // watchdog would eventually have opinions.
        //
        // Short while keys are arriving so a held arrow moves smoothly, longer
        // when idle so the rest of the device gets the core back.
        fw_task_sleep_ms(busy ? 1 : 5);
    }

    // ALWAYS. A terminal left in mouse-reporting mode sends escape sequences to
    // the shell for every click afterwards, which looks like the device has
    // started typing by itself.
    fw_tui_end();
    fw_printf("tuidemo: finished.\n");
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("tuidemo", "a worked example of a full-screen package", cmd_tuidemo);
    return 0;
}
