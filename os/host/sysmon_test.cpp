// sysmon_test — the monitor's arithmetic, and the panel it produces.
//
// Everything a monitor gets wrong is quiet by construction: the point of it is
// to print numbers, and a wrong number prints exactly as convincingly as a right
// one. A bar rounded the wrong way, a percentage taken against the wrong total,
// an RSSI scale off by a band, an interval that saves the value it had before
// the keypress rather than after — none of those raise anything anywhere.
//
// So the package's source is compiled in with the device faked, its helpers are
// checked against known answers, and then the whole app is driven by a scripted
// keyboard and the screen it drew is read back as characters.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>
#include <map>
#include <deque>

#include "../include/rpc_app.h"

static int g_checks, g_fails;
static void ck(bool cond, const char *what) {
    g_checks++;
    if (!cond) { g_fails++; printf("  FAIL: %s\n", what); }
}

// --- a screen of characters -------------------------------------------------

#define SE_W 80
#define SE_H 24
struct SeCell { char ch; unsigned char attr; unsigned char fg; };
static SeCell g_cells[SE_H][SE_W];
static SeCell g_shown[SE_H][SE_W];

static void screen_clear(void) {
    for (int y = 0; y < SE_H; y++)
        for (int x = 0; x < SE_W; x++) g_cells[y][x] = SeCell{' ', 0, 0};
}
static std::string screen_row(int y) {
    std::string s;
    for (int x = 0; x < SE_W; x++) s += g_shown[y][x].ch;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
static bool screen_has(const char *needle) {
    for (int y = 0; y < SE_H; y++)
        if (screen_row(y).find(needle) != std::string::npos) return true;
    return false;
}
// The colour a piece of text was drawn in, or -1 if it is not on screen. The
// bars are coloured by threshold, and a bar that never turns red is a warning
// nobody gets.
static int screen_colour_of(const char *needle) {
    for (int y = 0; y < SE_H; y++) {
        std::string row = screen_row(y);
        size_t at = row.find(needle);
        if (at != std::string::npos) return g_shown[y][at].fg;
    }
    return -1;
}

// --- the faked device -------------------------------------------------------

static std::map<std::string, std::string> g_reg;
static int      g_reg_saves;
static unsigned long g_millis;
static unsigned g_cpu, g_heap_total, g_heap_free, g_heap_largest;
static int      g_connected, g_rssi;
static int      g_adc_raw;
static uint32_t g_flash_total_kb, g_flash_free_kb;
static int      g_sd_present;
static FwTime   g_now;
static std::string g_log_reply;
static std::deque<FwTuiEvent> g_keys;
static int      g_begin, g_end;
static char     g_out[8192];
static unsigned g_out_len;
static RpcCommandFn g_cmd;
static int      g_cmds;

static void key(int k) { FwTuiEvent e{}; e.kind = 1; e.key = k; g_keys.push_back(e); }

extern "C" {
int fw_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_out + g_out_len, sizeof(g_out) - g_out_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_out_len += (unsigned)n;
    return n;
}
void *fw_malloc(size_t n) { return malloc(n ? n : 1); }
void  fw_free(void *p)    { free(p); }
uint32_t fw_millis(void)  { return (uint32_t)g_millis; }

int fw_reg_get(const char *key, char *out, uint32_t cap) {
    auto it = g_reg.find(key);
    if (it == g_reg.end()) { if (cap) out[0] = 0; return 0; }
    snprintf(out, cap, "%s", it->second.c_str());
    return 1;
}
int fw_reg_set(const char *key, const char *value) { g_reg[key] = value; return 1; }
int32_t fw_reg_get_int(const char *key, int32_t def) {
    auto it = g_reg.find(key);
    return it == g_reg.end() ? def : (int32_t)atoi(it->second.c_str());
}
void fw_reg_save(void) { g_reg_saves++; }

uint32_t fw_cpu_percent(void)  { return g_cpu; }
uint32_t fw_heap_total(void)   { return g_heap_total; }
uint32_t fw_heap_free(void)    { return g_heap_free; }
uint32_t fw_heap_largest(void) { return g_heap_largest; }
uint32_t fw_cores(void)        { return 2; }
uint32_t fw_clock_hz(void)     { return 150000000u; }

int fw_board(char *out, unsigned cap)     { snprintf(out, cap, "pico2_w"); return 1; }
int fw_unique_id(char *out, unsigned cap) { snprintf(out, cap, "e6614c311b8d2f37"); return 1; }

unsigned fw_adc_temp_channel(void) { return 4; }
int fw_adc_init(unsigned) { return 0; }
int fw_adc_read(unsigned) { return g_adc_raw; }

int fw_time_get(FwTime *out) { *out = g_now; return g_now.year ? 1 : 0; }

int fw_net_connected(void) { return g_connected; }
int fw_net_ssid(char *out, unsigned cap) {
    if (!g_connected) return 0;
    return snprintf(out, cap, "Attic-5G");
}
int fw_net_ip(char *out, unsigned cap) {
    if (!g_connected) return 0;
    return snprintf(out, cap, "192.168.1.44");
}
int fw_net_rssi(void) { return g_rssi; }

int fw_storage_roots(FwStorageRoot *out, int max) {
    if (max < 1) return -1;
    memset(out, 0, sizeof(*out) * (unsigned)max);
    snprintf(out[0].label, sizeof(out[0].label), "On-Board");
    snprintf(out[0].path, sizeof(out[0].path), "/");
    out[0].kind = FW_ROOT_FLASH;
    out[0].present = 1;
    out[0].total_kb = g_flash_total_kb;
    out[0].free_kb  = g_flash_free_kb;
    if (max >= 2 && g_sd_present) {
        snprintf(out[1].label, sizeof(out[1].label), "SD");
        snprintf(out[1].path, sizeof(out[1].path), "/sd");
        out[1].kind = FW_ROOT_SD;
        out[1].present = 1;
        out[1].total_kb = 4096;
        out[1].free_kb  = 1024;
        return 2;
    }
    return 1;
}

int fw_shell_run(const char *line, char *out, uint32_t cap) {
    if (out && cap) snprintf(out, cap, "%s", strcmp(line, "logdump") ? "" : g_log_reply.c_str());
    return 0;
}

void fw_tui_begin(void) { g_begin++; screen_clear(); }
void fw_tui_end(void)   { g_end++; }
void fw_tui_size(int *w, int *h) { if (w) *w = SE_W; if (h) *h = SE_H; }
void fw_tui_clear(void) { screen_clear(); }
void fw_tui_text(int x, int y, const char *s, unsigned char attr, unsigned char fg) {
    if (y < 0 || y >= SE_H) return;
    for (int i = 0; s[i] && x + i < SE_W; i++)
        if (x + i >= 0) g_cells[y][x + i] = SeCell{s[i], attr, fg};
}
void fw_tui_box(int x, int y, int, int, const char *title, unsigned char attr, unsigned char fg) {
    if (title) fw_tui_text(x + 2, y, title, attr, fg);
}
void fw_tui_fill(int x, int y, int w, int h, char ch, unsigned char attr, unsigned char fg) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (y + j >= 0 && y + j < SE_H && x + i >= 0 && x + i < SE_W)
                g_cells[y + j][x + i] = SeCell{ch, attr, fg};
}
void fw_tui_present(void) { memcpy(g_shown, g_cells, sizeof(g_cells)); }
int  fw_tui_refresh(void) { return 0; }
int  fw_tui_poll(FwTuiEvent *out) {
    if (g_keys.empty()) return 0;
    *out = g_keys.front();
    g_keys.pop_front();
    return 1;
}
// Once the script is exhausted the wait unwinds the way Ctrl+C would, instead of
// spinning for ever.
int  fw_task_should_stop(void) { return g_keys.empty() ? 1 : 0; }
// Time only moves when the app sleeps, so a refresh interval is exact here.
void fw_task_sleep_ms(uint32_t ms) { g_millis += ms; }

int rpc_register_command(const char *, const char *, RpcCommandFn fn) { g_cmds++; g_cmd = fn; return 1; }
}  // extern "C"

#include "../apps/sysmon/sysmon.cpp"

// --- driving ----------------------------------------------------------------

static void device_reset(void) {
    g_reg.clear();
    g_reg["Settings.Version"]   = "v2.0.0";
    g_reg["System.Device_ID"]   = "vela";
    g_reg["System.Active_User"] = "dash";
    g_reg_saves = 0;
    g_millis = 3723000ul;                  // 1h 2m 3s
    g_cpu = 38;
    g_heap_total = 374 * 1024;
    g_heap_free  = 256 * 1024;
    g_heap_largest = 84 * 1024;
    g_connected = 1;
    g_rssi = -52;
    g_adc_raw = 900;
    g_flash_total_kb = 1904;
    g_flash_free_kb  = 1564;
    g_sd_present = 0;
    g_now = FwTime{2026, 8, 17, 1, 20, 5, 1};
    g_log_reply = "";
    g_keys.clear();
}

static int run(const char *arg = nullptr) {
    g_out_len = 0; g_out[0] = 0;
    char  buf[32];
    char *argv[2];
    int   argc = 1;
    argv[0] = (char *)"sysmon";
    if (arg) { snprintf(buf, sizeof(buf), "%s", arg); argv[argc++] = buf; }
    return g_cmd(argc, argv);
}

int main(void) {
    printf("sysmon_test - the arithmetic, and the panel it draws\n");

    app_main(0);
    ck(g_cmds == 2, "the package registers sysmon and htop");
    if (!g_cmd) return 1;

    // --- the refresh interval ----------------------------------------------
    {
        ck(sm_clamp(50, SM_MIN_MS, SM_MAX_MS) == SM_MIN_MS, "too fast is clamped up");
        ck(sm_clamp(99999, SM_MIN_MS, SM_MAX_MS) == SM_MAX_MS, "too slow is clamped down");

        // v1's step: 250 ms while the interval is long, 100 while it is short,
        // so the same number of presses covers the whole range.
        ck(sm_step(750, 1) == 500, "faster from 750 is 500");
        ck(sm_step(500, 1) == 250, "faster from 500 is 250");
        ck(sm_step(250, 1) == 150, "and the step gets finer below 500");
        ck(sm_step(150, 1) == SM_MIN_MS, "down to the floor");
        ck(sm_step(SM_MIN_MS, 1) == SM_MIN_MS, "and no further");
        ck(sm_step(9900, 0) == SM_MAX_MS, "and up to the ceiling");
        ck(sm_step(SM_MAX_MS, 0) == SM_MAX_MS, "and no further than that");

        char s[16];
        sm_fmt_refresh(s, sizeof(s), 750);  ck(strcmp(s, "750ms") == 0, "under a second in ms");
        sm_fmt_refresh(s, sizeof(s), 1000); ck(strcmp(s, "1.00s") == 0, "a second in seconds");
        sm_fmt_refresh(s, sizeof(s), 2250); ck(strcmp(s, "2.25s") == 0, "and a fraction of one");

        ck(sm_atoi("500", 7) == 500, "a number parses");
        ck(sm_atoi("5x0", 7) == 7, "and anything else falls back");
        ck(sm_atoi("", 7) == 7, "including nothing at all");
        ck(sm_atoi(nullptr, 7) == 7, "and a null");
    }

    // --- uptime -------------------------------------------------------------
    {
        char s[32];
        sm_fmt_uptime(s, sizeof(s), 45000);      ck(strcmp(s, "45s") == 0, "seconds alone");
        sm_fmt_uptime(s, sizeof(s), 125000);     ck(strcmp(s, "2m 5s") == 0, "minutes and seconds");
        sm_fmt_uptime(s, sizeof(s), 3723000);    ck(strcmp(s, "1h 2m 3s") == 0, "hours as well");
        sm_fmt_uptime(s, sizeof(s), 90061000ul); ck(strcmp(s, "1d 1h 1m") == 0,
                                                   "and days, which drop the seconds");
        sm_fmt_uptime(s, sizeof(s), 0);          ck(strcmp(s, "0s") == 0, "just booted");
    }

    // --- bars ---------------------------------------------------------------
    {
        char b[SM_BAR_W + 2];
        unsigned char c = sm_bar(b, sizeof(b), 0, 10);
        ck(strcmp(b, "----------") == 0, "an empty bar is all empty");
        ck(c == SM_C_OK, "and green");
        c = sm_bar(b, sizeof(b), 100, 10);
        ck(strcmp(b, "##########") == 0, "a full bar is all filled");
        ck(c == SM_C_BAD, "and red");
        sm_bar(b, sizeof(b), 50, 10);
        ck(strcmp(b, "#####-----") == 0, "half is half");
        ck((int)strlen(b) == 10, "the bar is exactly as wide as asked");

        // The thresholds, at their exact edges. A bar that never turns red is a
        // warning nobody ever gets.
        ck(sm_bar(b, sizeof(b), 59, 10) == SM_C_OK,   "59% is still green");
        ck(sm_bar(b, sizeof(b), 60, 10) == SM_C_WARN, "60% is yellow");
        ck(sm_bar(b, sizeof(b), 84, 10) == SM_C_WARN, "84% is still yellow");
        ck(sm_bar(b, sizeof(b), 85, 10) == SM_C_BAD,  "85% is red");

        // Out of range is clamped rather than drawn past the end of the buffer.
        sm_bar(b, sizeof(b), 150, 10);
        ck(strcmp(b, "##########") == 0, "over 100% is full, not overrun");
        sm_bar(b, sizeof(b), -20, 10);
        ck(strcmp(b, "----------") == 0, "and under zero is empty");
    }

    // --- percentages --------------------------------------------------------
    {
        ck(sm_pct(50, 100) == 50, "half is 50%");
        ck(sm_pct(0, 100) == 0, "none is 0%");
        // The one that would otherwise divide by zero on a board that reported
        // no total - which is exactly what an absent SD card looks like.
        ck(sm_pct(10, 0) == 0, "a zero total is 0%, not a crash");
    }

    // --- signal -------------------------------------------------------------
    {
        // v1's scale, kept exactly: -90 dBm is nothing, -30 is everything.
        ck(sm_rssi_pct(-90) == 0, "-90 dBm is the bottom");
        ck(sm_rssi_pct(-30) == 100, "-30 dBm is the top");
        ck(sm_rssi_pct(-60) == 50, "and -60 is the middle");
        ck(sm_rssi_pct(-120) == 0, "below the bottom clamps");
        ck(sm_rssi_pct(0) == 100, "and above the top clamps too");
        ck(strcmp(sm_rssi_word(-45), "Excellent") == 0, "-45 dBm is excellent");
        ck(strcmp(sm_rssi_word(-63), "Good") == 0, "-63 is good");
        ck(strcmp(sm_rssi_word(-77), "Fair") == 0, "-77 is fair");
        ck(strcmp(sm_rssi_word(-86), "Poor") == 0, "-86 is poor");
    }

    // --- sizes and rows -----------------------------------------------------
    {
        char s[24];
        sm_fmt_kb(s, sizeof(s), 512);  ck(strcmp(s, "512 KB") == 0, "kilobytes stay kilobytes");
        sm_fmt_kb(s, sizeof(s), 1536); ck(strcmp(s, "1.5 MB") == 0, "and become megabytes");

        char line[SM_COLS_MAX];
        sm_row(line, sizeof(line), "Board", "pico2_w", "Clock", "150 MHz");
        ck(strncmp(line, "  Board    pico2_w", 18) == 0, "the first column is padded");
        ck(strstr(line, "Clock    150 MHz") != nullptr, "and the second is there");
        // The columns have to line up down the panel, not follow whatever the
        // first value happened to be.
        ck(strstr(line, "Clock") - line == SM_COL2, "the second column starts where it should");
        sm_row(line, sizeof(line), "UID", "abc", 0, 0);
        ck(strcmp(line, "  UID      abc") == 0, "one column alone has no padding after it");

        // A long first value must not push the second column off or overlap it.
        sm_row(line, sizeof(line), "Board", "a-very-long-board-name-indeed-here", "Clock", "1 MHz");
        ck(strstr(line, "Clock") != nullptr, "a long first value still leaves the second readable");
    }

    // --- the log tail -------------------------------------------------------
    {
        char rows[SM_LOG_ROWS][SM_LOG_COLS];
        // The capture fills from the START of the log, so the panel has to take
        // the END of what came back. Showing the oldest six lines would look
        // exactly like showing the newest six.
        int n = sm_tail("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n",
                        SM_LOG_ROWS, rows[0], sizeof(rows), SM_LOG_COLS);
        ck(n == SM_LOG_ROWS, "six lines come back");
        ck(strcmp(rows[0], "three") == 0, "starting at the seventh from the end");
        ck(strcmp(rows[5], "eight") == 0, "and ending with the newest");

        n = sm_tail("only\ntwo\n", SM_LOG_ROWS, rows[0], sizeof(rows), SM_LOG_COLS);
        ck(n == 2, "a short log gives what there is");
        ck(strcmp(rows[0], "only") == 0, "oldest first");

        n = sm_tail("", SM_LOG_ROWS, rows[0], sizeof(rows), SM_LOG_COLS);
        ck(n == 0, "an empty log gives nothing");

        n = sm_tail("  indented\n\n   \n", SM_LOG_ROWS, rows[0], sizeof(rows), SM_LOG_COLS);
        ck(n == 1, "blank lines are not lines");
        ck(strcmp(rows[0], "indented") == 0, "and the indent comes off");

        // Asking for more rows than the scratch array holds must be clamped, not
        // written past. One caller today passes the right number, which is the
        // situation in which the second caller gets it wrong.
        n = sm_tail("a\nb\nc\nd\ne\nf\ng\nh\n", SM_LOG_ROWS + 40,
                    rows[0], sizeof(rows), SM_LOG_COLS);
        ck(n <= SM_LOG_ROWS, "asking for too many rows is clamped");
        ck(strcmp(rows[SM_LOG_ROWS - 1], "h") == 0, "and still ends with the newest");
        n = sm_tail("a\nb\n", 0, rows[0], sizeof(rows), SM_LOG_COLS);
        ck(n == 0, "asking for none gives none");
    }

    // --- the panel ----------------------------------------------------------
    {
        device_reset();
        key('q');
        int rc = run();
        ck(rc == 0, "the monitor opens and closes cleanly");
        ck(g_begin == g_end,
           "every fw_tui_begin was paired with an end - a terminal left in mouse "
           "mode types by itself afterwards");
        ck(screen_has("RPCortex Monitor"), "the title is there");
        ck(screen_has("v2.0.0"), "with the OS version out of the registry");
        ck(screen_has("dash@vela"), "and who is logged in, on which device");
        ck(screen_has("pico2_w"), "the board");
        ck(screen_has("150 MHz"), "the clock");
        ck(screen_has("1h 2m 3s"), "the uptime");
        ck(screen_has("2026-08-17 01:20"), "the wall clock");
        ck(screen_has("e6614c311b8d2f37"), "and the board's identity");
        ck(screen_has("CPU"), "the CPU bar");
        ck(screen_has(" 38%"), "with the figure the scheduler reported");
        ck(screen_has("RAM"), "the RAM bar");
        ck(screen_has("largest free block 84 KB"),
           "and the largest free block, which is the number that predicts a failure");
        ck(screen_has("Flash"), "the flash bar");
        ck(screen_has("Attic-5G"), "the network");
        ck(screen_has("192.168.1.44"), "its address");
        ck(screen_has("-52 dBm"), "and the signal");
        ck(screen_has("Good"), "described in words too - -52 dBm is 63% on v1's scale");
        ck(screen_has("q quit"), "the key hints are along the bottom");
        ck(!screen_has("Recent log"), "the log panel is off to begin with");
    }

    // --- a device that knows less about itself ------------------------------
    {
        device_reset();
        g_connected = 0;
        g_now = FwTime{};                   // never synchronised
        key('q');
        run();
        ck(screen_has("not connected"), "no network says so");
        ck(!screen_has("Attic-5G"), "and shows no stale name");
        ck(screen_has("not set"), "an unsynchronised clock says so");
        ck(!screen_has("1970"), "rather than inventing a date");
    }
    {
        // A connected network the radio has no reading for. 0 dBm is a real and
        // extraordinary value, so "no reading" must not be drawn as one.
        device_reset();
        g_rssi = 0;
        key('n');                            // network detail on
        key('q');
        run();
        ck(screen_has("no reading"), "a missing RSSI says so");
        ck(!screen_has("0 dBm"), "rather than showing 0 dBm");
        ck(screen_has("not reachable from a package"),
           "and the detail panel is honest about what the ABI cannot give it");
    }

    // --- a full machine -----------------------------------------------------
    {
        device_reset();
        g_cpu = 100;
        g_heap_free = 8 * 1024;              // 366 of 374 KB used
        key('q');
        run();
        ck(screen_has("100%"), "a pegged CPU reads 100");
        ck(screen_colour_of("######################") == SM_C_BAD,
           "and a bar past 85% is drawn red");

        // Where the coloured bar lands, not just what colour it is. It is
        // written twice - plain as part of the line, then in colour over the
        // top - and one cell of drift eats the closing bracket while still
        // looking like a bar. This is that bug, pinned.
        int row = -1;
        for (int y = 0; y < SE_H && row < 0; y++)
            if (screen_row(y).find("  CPU") == 0) row = y;
        ck(row >= 0, "the CPU row is where it should be");
        if (row >= 0) {
            std::string text = screen_row(row);
            size_t open_at = text.find('[');
            size_t close_at = text.find(']');
            ck(open_at != std::string::npos && close_at != std::string::npos,
               "the bar still has both its brackets");
            ck(close_at - open_at == SM_BAR_W + 1,
               "and exactly the bar between them - nothing was overwritten");
            ck(g_shown[row][open_at].fg == 0,
               "the opening bracket keeps the line's own colour");
            ck(g_shown[row][open_at + 1].fg == SM_C_BAD,
               "the first cell INSIDE it is where the coloured bar starts");
            ck(g_shown[row][close_at - 1].fg == SM_C_BAD,
               "and the last cell inside it is coloured too");
            ck(g_shown[row][close_at].fg == 0, "while the closing bracket is not");
        }
    }

    // --- an SD card ---------------------------------------------------------
    {
        device_reset();
        g_sd_present = 1;
        key('q');
        run();
        ck(screen_has("Card"), "a mounted card gets a bar of its own");
        ck(screen_has("3.0 MB of 4.0 MB"), "with its own usage");

        device_reset();
        key('q');
        run();
        ck(!screen_has("Card"), "and no card means no row, rather than an empty one");
    }

    // --- the keys -----------------------------------------------------------
    {
        device_reset();
        key('+');
        key('q');
        run();
        ck(g_reg["Apps.SysMon_Refresh"] == "500",
           "'+' steps the interval down and saves the NEW value");
        ck(g_reg_saves >= 1, "and asks for it to reach flash");

        device_reset();
        key('-');
        key('q');
        run();
        ck(g_reg["Apps.SysMon_Refresh"] == "1000", "'-' steps it the other way");

        device_reset();
        g_reg["Apps.SysMon_Refresh"] = "2000";
        key('q');
        run();
        ck(screen_has("2.00s"), "the saved interval is what it opens with");

        // An argument is for this run only: v1 did the same, so a one-off look
        // does not quietly change the setting.
        device_reset();
        g_reg["Apps.SysMon_Refresh"] = "2000";
        key('q');
        run("300");
        ck(screen_has("300ms"), "an argument overrides the interval");
        ck(g_reg["Apps.SysMon_Refresh"] == "2000", "without touching what was saved");

        device_reset();
        g_reg["Apps.SysMon_Refresh"] = "2000";
        key('q');
        run("banana");
        ck(screen_has("2.00s"), "a nonsense argument leaves the saved interval alone");

        device_reset();
        g_reg["Apps.SysMon_Refresh"] = "50";     // below the floor
        key('q');
        run();
        ck(screen_has("100ms"), "a stored interval out of range is clamped, not obeyed");
    }

    // --- the log panel ------------------------------------------------------
    {
        device_reset();
        g_log_reply = "boot ok\nwifi joined\nradio down\nradio up\npkg installed\n"
                      "task started\nsomething newest\n";
        key('l');
        key('q');
        run();
        ck(screen_has("Recent log"), "'l' opens the log panel");
        ck(screen_has("something newest"), "showing the newest line");
        ck(!screen_has("boot ok"), "and not the oldest, which fell off the end");

        device_reset();
        g_log_reply = "";
        key('l');
        key('q');
        run();
        ck(screen_has("nothing logged yet"), "an empty log says so");

        device_reset();
        g_log_reply = "a line\n";
        key('l');
        key('l');                            // and off again
        key('q');
        run();
        ck(!screen_has("Recent log"), "'l' twice closes it again");
    }

    // --- quitting -----------------------------------------------------------
    {
        device_reset();
        key(FW_KEY_ESC);
        ck(run() == 0, "Escape quits");
        device_reset();
        key(3);
        ck(run() == 0, "and so does Ctrl+C");
        device_reset();
        key('Q');
        ck(run() == 0, "and a capital Q");
    }

    // --- help ---------------------------------------------------------------
    {
        device_reset();
        ck(run("help") == 0, "'sysmon help' works");
        ck(strstr(g_out, "Apps.SysMon_Refresh") != nullptr,
           "and names the registry key, which is where the setting lives");
        ck(g_begin == g_end, "and never opens the screen at all");
    }

    printf("  %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
