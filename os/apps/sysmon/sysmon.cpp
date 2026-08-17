// SysMon — the live system monitor, brought across from v1. `sysmon`, `htop`.
//
// Same panels, same keys, same registry setting. Two things are genuinely
// different, and both are the OS having grown a way to answer.
//
// THE CPU FIGURE IS NOW A MEASUREMENT. v1 ran a busy loop a few thousand times
// and inferred load from how much slower it was than its own best-ever run.
// That was not a design, it was the only thing available: MicroPython had no
// scheduler counter to ask. fw_cpu_percent is the scheduler's own accounting,
// per core and sampled since anything last looked — so this reports load
// instead of estimating it, and costs nothing to read.
//
// AND THE HEAP REPORTS ITS LARGEST FREE BLOCK. On this OS free bytes do not
// predict whether the next allocation succeeds; the largest single block does,
// and a device can sit at 90 KB free and still refuse 20 KB. A monitor that
// showed only the percentage would be showing the number that does not answer
// the question anybody opens it to ask.
//
// The bars are ASCII. A screen cell here holds ONE byte, so v1's block-drawing
// characters do not fit in one — and half a UTF-8 sequence per cell renders as
// mojibake rather than as a bar.
#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

RPC_APP_VER("sysmon", "2.0");

#define SM_REG_REFRESH "Apps.SysMon_Refresh"
#define SM_DEFAULT_MS  750
#define SM_MIN_MS      100
#define SM_MAX_MS      10000

#define SM_COLS_MAX    104
#define SM_BAR_W       22
#define SM_COL2        40          // where the second column of the System panel starts

#define SM_C_HEAD  6      // cyan
#define SM_C_OK    2      // green
#define SM_C_WARN  3      // yellow
#define SM_C_BAD   1      // red

// The log ring is 48 lines of at most 96 characters, plus each line's timestamp
// and level. One buffer, taken when the panel is first opened rather than at
// start-up, because most runs never open it.
#define SM_LOG_BYTES  6144
#define SM_LOG_ROWS   6
// Narrower than a row on purpose, so the two spaces the panel indents by
// cannot push a full-width log line past the end of the line buffer.
#define SM_LOG_COLS   (SM_COLS_MAX - 4)

typedef struct {
    int  w, h;
    int  refresh_ms;
    int  show_log;
    int  show_net;
    char *logbuf;
} SmState;

static SmState sm;

// --- small helpers ----------------------------------------------------------

static int sm_streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int sm_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Digits only, and no answer at all for anything else — v1 caught a ValueError
// and carried on with the saved interval, which is the same behaviour.
static int sm_atoi(const char *s, int fallback) {
    if (!s || !*s) return fallback;
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return fallback;
        v = v * 10 + (*p - '0');
        if (v > 999999) return fallback;
    }
    return v;
}

// The step v1 used: coarse once the interval is long, fine when it is short, so
// the same number of presses covers the range at either end.
static int sm_step(int ms, int faster) {
    int step = ms >= 500 ? 250 : 100;
    return sm_clamp(faster ? ms - step : ms + step, SM_MIN_MS, SM_MAX_MS);
}

static void sm_fmt_refresh(char *out, unsigned cap, int ms) {
    if (ms >= 1000) snprintf(out, cap, "%d.%02ds", ms / 1000, (ms % 1000) / 10);
    else            snprintf(out, cap, "%dms", ms);
}

// Uptime since the machine came up, which is what the shell's `uptime` reports
// too — NOT since this app was opened.
static void sm_fmt_uptime(char *out, unsigned cap, unsigned long ms) {
    unsigned long s = ms / 1000, m = s / 60, h = m / 60, d = h / 24;
    s %= 60; m %= 60; h %= 24;
    if (d)      snprintf(out, cap, "%lud %luh %lum", d, h, m);
    else if (h) snprintf(out, cap, "%luh %lum %lus", h, m, s);
    else if (m) snprintf(out, cap, "%lum %lus", m, s);
    else        snprintf(out, cap, "%lus", s);
}

static void sm_fmt_kb(char *out, unsigned cap, unsigned long kb) {
    if (kb >= 1024) snprintf(out, cap, "%lu.%lu MB", kb / 1024, (kb % 1024) * 10 / 1024);
    else            snprintf(out, cap, "%lu KB", kb);
}

static int sm_pct(unsigned long part, unsigned long whole) {
    if (!whole) return 0;
    return (int)(part * 100 / whole);
}

// A bar of `w` cells, `#` filled and `-` empty. Returns the colour it should be
// drawn in, which is the whole reason a caller wants the two separately.
static unsigned char sm_bar(char *out, unsigned cap, int pct, int w) {
    pct = sm_clamp(pct, 0, 100);
    int filled = pct * w / 100;
    unsigned n = 0;
    for (int i = 0; i < w && n + 1 < cap; i++) out[n++] = i < filled ? '#' : '-';
    out[n] = 0;
    return pct < 60 ? SM_C_OK : (pct < 85 ? SM_C_WARN : SM_C_BAD);
}

// v1's scale, kept exactly: -90 dBm is nothing and -30 is everything.
static int sm_rssi_pct(int rssi) { return sm_clamp((rssi + 90) * 100 / 60, 0, 100); }

static const char *sm_rssi_word(int rssi) {
    int p = sm_rssi_pct(rssi);
    if (p >= 70) return "Excellent";
    if (p >= 45) return "Good";
    if (p >= 20) return "Fair";
    return "Poor";
}

// Two labelled values on one line, the way v1's panel was laid out. The second
// column starts at a fixed offset so the columns line up down the panel rather
// than following whatever the first value happened to be.
static void sm_row(char *out, unsigned cap, const char *l1, const char *v1,
                   const char *l2, const char *v2) {
    unsigned n = (unsigned)snprintf(out, cap, "  %-9s%s", l1, v1);
    if (!l2) return;
    while (n < SM_COL2 && n + 1 < cap) out[n++] = ' ';
    out[n] = 0;
    snprintf(out + n, cap - n, "%-9s%s", l2, v2 ? v2 : "");
}

// The last `want` non-blank lines of a captured reply, oldest first. The log
// panel needs the END of the log and a capture fills from the START, so this is
// what turns one into the other.
static int sm_tail(const char *text, int want, char *out, unsigned cap, unsigned stride) {
    const char *starts[SM_LOG_ROWS];
    // Clamped to what `starts` can hold. There is one caller and it passes the
    // right number, which is exactly the situation in which the second caller
    // gets it wrong - and the failure would be writing past a stack array.
    if (want > SM_LOG_ROWS) want = SM_LOG_ROWS;
    if (want < 1) return 0;
    int n = 0;
    for (const char *p = text; *p; ) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        int blank = 1;
        for (const char *q = line; q < p; q++) if (*q != ' ' && *q != '\t') blank = 0;
        if (!blank) {
            if (n < want) starts[n++] = line;
            else {
                for (int i = 1; i < want; i++) starts[i - 1] = starts[i];
                starts[want - 1] = line;
            }
        }
        if (*p) p++;
    }
    for (int i = 0; i < n; i++) {
        char *dst = out + (unsigned)i * stride;
        unsigned k = 0;
        const char *s = starts[i];
        while (*s == ' ' || *s == '\t') s++;
        while (s[k] && s[k] != '\n' && k + 1 < stride && k + 1 < cap) { dst[k] = s[k]; k++; }
        dst[k] = 0;
    }
    return n;
}

// --- what the device says about itself --------------------------------------

static void sm_reg(const char *key, char *out, unsigned cap, const char *def) {
    if (!fw_reg_get(key, out, cap) || !out[0]) snprintf(out, cap, "%s", def);
}

// The die sensor, converted exactly as `gpio temp` converts it. Two packages
// disagreeing about the temperature is the sort of thing nobody notices for
// months, so the arithmetic is copied rather than re-derived.
static void sm_temp(char *out, unsigned cap) {
    unsigned ch = fw_adc_temp_channel();
    if (fw_adc_init(ch) != 0) { snprintf(out, cap, "n/a"); return; }
    int raw = fw_adc_read(ch);
    if (raw < 0) { snprintf(out, cap, "n/a"); return; }
    double volts = raw * 3.3 / 4096.0;
    double c = 27.0 - (volts - 0.706) / 0.001721;
    int whole = (int)c;
    int tenth = (int)((c - whole) * 10);
    if (tenth < 0) tenth = -tenth;
    snprintf(out, cap, "%d.%d C", whole, tenth);
}

static void sm_clock_now(char *out, unsigned cap) {
    FwTime t;
    // Zero-filled and 0 returned when the clock has never been set, so a year of
    // 1970 is never invented for a device that simply does not know.
    if (!fw_time_get(&t) || t.year < 2000) { snprintf(out, cap, "not set"); return; }
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
}

// --- drawing ----------------------------------------------------------------

// One resource row: a label, a bar, the percentage, and whatever detail belongs
// after it.
//
// The bar is written TWICE — once as part of the line, and once in colour over
// the top, because a cell carries one colour and the numbers beside it are not
// meant to change colour with the bar. So the column it lands in has to be
// computed rather than counted off the format string by hand. Counting it by
// hand put the coloured copy one cell right of the plain one, which quietly ate
// the closing bracket and left the last cell of a full bar uncoloured — visible
// only if you were looking for it.
static void sm_meter(int y, const char *label, int pct, int w, const char *detail) {
    char bar[SM_BAR_W + 2], line[SM_COLS_MAX];
    unsigned char colour = sm_bar(bar, sizeof(bar), pct, w);
    int at = snprintf(line, sizeof(line), "  %-7s[", label);
    if (at < 0 || at >= (int)sizeof(line)) return;
    snprintf(line + at, sizeof(line) - (unsigned)at, "%s]  %3d%%   %s", bar, pct, detail);
    fw_tui_text(0, y, line, 0, 0);
    fw_tui_text(at, y, bar, FW_ATTR_BOLD, colour);
}

// A section rule with its name in it, the way v1's panels were separated.
static int sm_section(int y, const char *name) {
    char line[SM_COLS_MAX];
    unsigned n = (unsigned)snprintf(line, sizeof(line), "-- %s ", name);
    while (n + 1 < sizeof(line) && (int)n < sm.w - 1) line[n++] = '-';
    line[n] = 0;
    fw_tui_text(0, y, line, FW_ATTR_DIM, SM_C_HEAD);
    return y + 1;
}

static void sm_draw(void) {
    // Sized to what they hold, not generously: an over-large scratch buffer
    // makes the compiler's own truncation analysis useless, and that analysis
    // is the only thing watching these format strings.
    char a[24], b[24], line[SM_COLS_MAX];
    fw_tui_clear();

    // --- the title bar ------------------------------------------------------
    {
        char ver[24], dev[24], user[24], hint[56];
        sm_reg("Settings.Version", ver, sizeof(ver), "?");
        sm_reg("System.Device_ID", dev, sizeof(dev), "vela");
        sm_reg("System.Active_User", user, sizeof(user), "?");
        sm_fmt_refresh(a, sizeof(a), sm.refresh_ms);
        snprintf(line, sizeof(line), " RPCortex Monitor   %s  %s@%s", ver, user, dev);
        fw_tui_fill(0, 0, sm.w, 1, ' ', FW_ATTR_REVERSE, 0);
        fw_tui_text(0, 0, line, FW_ATTR_REVERSE, 0);
        snprintf(hint, sizeof(hint), "%s  [+/-] [l]og [n]et [q]uit ", a);
        int x = sm.w - (int)strlen(hint);
        if (x > (int)strlen(line)) fw_tui_text(x, 0, hint, FW_ATTR_REVERSE, 0);
    }

    int y = sm_section(1, "System");
    {
        char board[24], uid[24], up[32], temp[16], now[32], cores[8];
        if (!fw_board(board, sizeof(board))) snprintf(board, sizeof(board), "?");
        if (!fw_unique_id(uid, sizeof(uid)))  snprintf(uid, sizeof(uid), "?");
        sm_fmt_uptime(up, sizeof(up), fw_millis());
        sm_temp(temp, sizeof(temp));
        sm_clock_now(now, sizeof(now));
        snprintf(cores, sizeof(cores), "%u", (unsigned)fw_cores());
        snprintf(a, sizeof(a), "%u MHz", (unsigned)(fw_clock_hz() / 1000000u));

        sm_row(line, sizeof(line), "Board", board, "Clock", a);   fw_tui_text(0, y++, line, 0, 0);
        sm_row(line, sizeof(line), "Uptime", up, "Temp", temp);   fw_tui_text(0, y++, line, 0, 0);
        sm_row(line, sizeof(line), "Cores", cores, "Time", now);  fw_tui_text(0, y++, line, 0, 0);
        sm_row(line, sizeof(line), "UID", uid, 0, 0);             fw_tui_text(0, y++, line, 0, 0);
    }

    y = sm_section(y, "Resources");
    {
        snprintf(a, sizeof(a), "across %u core%s",
                 (unsigned)fw_cores(), fw_cores() == 1 ? "" : "s");
        sm_meter(y++, "CPU", (int)fw_cpu_percent(), SM_BAR_W, a);

        unsigned long total = fw_heap_total(), freeb = fw_heap_free();
        unsigned long used = total > freeb ? total - freeb : 0;
        sm_fmt_kb(a, sizeof(a), used / 1024);
        sm_fmt_kb(b, sizeof(b), total / 1024);
        snprintf(line, sizeof(line), "%s of %s", a, b);
        sm_meter(y++, "RAM", sm_pct(used, total), SM_BAR_W, line);

        // The number that actually predicts whether the next allocation works.
        sm_fmt_kb(a, sizeof(a), fw_heap_largest() / 1024);
        snprintf(line, sizeof(line),
                 "         largest free block %s   (free bytes do not predict this)", a);
        fw_tui_text(0, y++, line, FW_ATTR_DIM, 0);

        FwStorageRoot roots[2];
        int nroots = fw_storage_roots(roots, 2);
        for (int i = 0; i < nroots && i < 2; i++) {
            if (!roots[i].present || !roots[i].total_kb) continue;
            unsigned long ft = roots[i].total_kb, ff = roots[i].free_kb;
            unsigned long fu = ft > ff ? ft - ff : 0;
            sm_fmt_kb(a, sizeof(a), fu);
            sm_fmt_kb(b, sizeof(b), ft);
            snprintf(line, sizeof(line), "%s of %s", a, b);
            sm_meter(y++, roots[i].kind == FW_ROOT_SD ? "Card" : "Flash",
                     sm_pct(fu, ft), SM_BAR_W, line);
        }
    }

    y = sm_section(y, "Network");
    {
        if (!fw_net_connected()) {
            fw_tui_text(0, y++, "  not connected   (wifi connect)", FW_ATTR_DIM, 0);
        } else {
            char ssid[FW_NET_SSID_MAX], ip[FW_NET_ADDR_MAX];
            if (!fw_net_ssid(ssid, sizeof(ssid))) snprintf(ssid, sizeof(ssid), "?");
            if (!fw_net_ip(ip, sizeof(ip)))       snprintf(ip, sizeof(ip), "?");
            sm_row(line, sizeof(line), "SSID", ssid, "IP", ip);
            fw_tui_text(0, y++, line, 0, 0);

            // Cached by the driver, so asking every frame costs nothing and — the
            // part that matters — never brings the radio up to answer. A status
            // panel that activates hardware to draw itself has taken a board down
            // here before.
            int rssi = fw_net_rssi();
            if (rssi) {
                snprintf(line, sizeof(line), "%d dBm  %s", rssi, sm_rssi_word(rssi));
                sm_meter(y++, "Signal", sm_rssi_pct(rssi), 10, line);
            } else if (sm.show_net) {
                fw_tui_text(0, y++, "  Signal   no reading", FW_ATTR_DIM, 0);
            }
            if (sm.show_net) {
                // Everything else v1 showed here — gateway, DNS, netmask, MAC —
                // has no door in the package ABI. Saying so beats a row of
                // question marks that reads like a fault.
                fw_tui_text(0, y++,
                            "  gateway, DNS, netmask and MAC are not reachable from a package",
                            FW_ATTR_DIM, 0);
            }
        }
    }

    if (sm.show_log && sm.logbuf) {
        y = sm_section(y, "Recent log");
        char rows[SM_LOG_ROWS][SM_LOG_COLS];
        int n = sm_tail(sm.logbuf, SM_LOG_ROWS, rows[0], sizeof(rows), SM_LOG_COLS);
        if (!n) fw_tui_text(0, y++, "  (nothing logged yet)", FW_ATTR_DIM, 0);
        for (int i = 0; i < n && y < sm.h - 1; i++) {
            snprintf(line, sizeof(line), "  %s", rows[i]);
            fw_tui_text(0, y++, line, FW_ATTR_DIM, 0);
        }
    }

    fw_tui_fill(0, sm.h - 1, sm.w, 1, ' ', FW_ATTR_DIM, 0);
    fw_tui_text(0, sm.h - 1,
                " r refresh   +/- speed   l log   n network   ^L redraw   q quit",
                FW_ATTR_DIM, 0);
    fw_tui_present();
}

// --- the loop ---------------------------------------------------------------

static void sm_save_refresh(int ms) {
    char v[12];
    snprintf(v, sizeof(v), "%d", ms);
    fw_reg_set(SM_REG_REFRESH, v);
    // Explicit, because a write is not a flash write until something says so —
    // and somebody holding '+' should cost one save at the end of it, not ten.
    fw_reg_save();
}

// Refill the log panel's buffer. There is no fw_log_* in the ABI, so this asks
// the shell for `logdump` with its output captured, which keeps it off the
// screen the monitor is drawing on.
static void sm_read_log(void) {
    if (!sm.logbuf) return;
    sm.logbuf[0] = 0;
    fw_shell_run("logdump", sm.logbuf, SM_LOG_BYTES);
}

static void sm_relayout(void) {
    fw_tui_size(&sm.w, &sm.h);
    if (sm.w < 20) { sm.w = 80; sm.h = 24; }
    if (sm.w > SM_COLS_MAX) sm.w = SM_COLS_MAX;
}

static void sm_usage(void) {
    fw_printf("sysmon / htop - the live system monitor.\n\n");
    fw_printf("  sysmon         open it at the saved refresh interval\n");
    fw_printf("  sysmon <ms>    open it at that interval, just for this run\n\n");
    fw_printf("  r refresh now   +/- faster or slower (saved)   l log tail\n");
    fw_printf("  n network detail   ^L redraw   q or Esc quit\n\n");
    fw_printf("The interval lives in %s, %d-%d ms.\n",
              SM_REG_REFRESH, SM_MIN_MS, SM_MAX_MS);
}

static int sm_cmd(int argc, char **argv) {
    if (argc > 1 && (sm_streq(argv[1], "help") || sm_streq(argv[1], "-h") ||
                     sm_streq(argv[1], "--help") || sm_streq(argv[1], "?"))) {
        sm_usage();
        return 0;
    }

    sm.refresh_ms = sm_clamp(fw_reg_get_int(SM_REG_REFRESH, SM_DEFAULT_MS),
                             SM_MIN_MS, SM_MAX_MS);
    if (argc > 1) {
        int want = sm_atoi(argv[1], 0);
        // For this run only. v1 did the same: an argument overrides without
        // touching what was saved, so a one-off look does not change the setting.
        if (want) sm.refresh_ms = sm_clamp(want, SM_MIN_MS, SM_MAX_MS);
    }
    sm.show_log = 0;
    sm.show_net = 0;
    sm.logbuf = 0;

    fw_tui_begin();
    sm_relayout();

    unsigned long next = 0;
    int running = 1;
    while (running) {
        unsigned long now = fw_millis();
        if (now >= next) {
            if (sm.show_log) sm_read_log();
            sm_draw();
            next = now + (unsigned long)sm.refresh_ms;
        }

        FwTuiEvent e;
        int got = fw_tui_poll(&e);
        if (!got) {
            if (fw_task_should_stop()) break;    // Ctrl+C, or a kill
            fw_task_sleep_ms(5);
            continue;
        }
        if (e.kind != 1) continue;
        switch (e.key) {
            case 'q': case 'Q': case 3: case 4: case FW_KEY_ESC: running = 0; break;
            case 'r': case 'R': case 13: case 10: next = 0; break;
            case '+': case '=':
                sm.refresh_ms = sm_step(sm.refresh_ms, 1);
                sm_save_refresh(sm.refresh_ms);
                next = 0;
                break;
            case '-': case '_':
                sm.refresh_ms = sm_step(sm.refresh_ms, 0);
                sm_save_refresh(sm.refresh_ms);
                next = 0;
                break;
            case 'l': case 'L':
                sm.show_log = !sm.show_log;
                // Taken when it is first wanted, not at start-up: most runs
                // never open this panel and 6 KB is worth not holding.
                if (sm.show_log && !sm.logbuf) sm.logbuf = (char *)fw_malloc(SM_LOG_BYTES);
                next = 0;
                break;
            case 'n': case 'N': sm.show_net = !sm.show_net; next = 0; break;
            case 12: if (fw_tui_refresh()) sm_relayout(); next = 0; break;
            default: break;
        }
    }

    // ALWAYS. A terminal left in mouse-reporting mode sends escape sequences to
    // the shell for every click afterwards, which looks like the device has
    // started typing by itself.
    fw_tui_end();
    if (sm.logbuf) { fw_free(sm.logbuf); sm.logbuf = 0; }
    fw_printf("sysmon closed.\n");
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    rpc_register_command("sysmon", "the live system monitor", sm_cmd);
    // v1 registered both, and htop is what anyone coming from a desktop tries.
    rpc_register_command("htop", "the live system monitor", sm_cmd);
    return 0;
}
