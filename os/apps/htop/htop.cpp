// htop: the interactive process viewer, laid out the way the real one is.
//
//   htop [ms]       open it (refresh every ms, 1500 by default); also `top`
//
//   Up/Down PgUp/PgDn Home/End   move          F6 or < >   sort by the next column
//   P  sort by CPU    M  by stack    T  by time    N  by pid    I  reverse
//   F9 or k  stop the selected task (asks first)   + -  refresh faster or slower
//   F1 or h  help     F10, q or Esc  quit          Ctrl+L  redraw
//
// The meters at the top are each core's share of the last interval, then memory
// and flash. The list is every task (fw_task_list): its CPU share over the same
// interval, the stack it has reached, its run time and where it came from.
// Stopping a task follows `kill`'s rule, so it stops only what the person
// running htop could stop.
#include "rpc_app.h"

#include <stdio.h>
#include <string.h>

RPC_APP_VER_FLAGS("htop", "1.1", RPC_APP_F_ON_DEMAND);

#define MAXT    32
#define C_RED   1
#define C_GRN   2
#define C_YEL   3
#define C_BLU   4
#define C_MAG   5
#define C_CYN   6

enum Sort { S_CPU, S_STACK, S_TIME, S_PID, S_NAME, S_COUNT };
static const char *const kSortName[S_COUNT] = {"CPU%", "STACK", "TIME+", "PID", "NAME"};

struct Row {
    FwTaskInfo t;
    unsigned   cpu10;          // tenths of a percent of one core, the last interval
};

static struct {
    int w, h;
    FwTaskInfo prev[MAXT];
    int nprev;
    Row rows[MAXT];
    int n;
    uint32_t last_ms;
    unsigned core10[2];        // tenths of a percent, per core
    Sort sort;
    bool rev;
    int sel, top;
    int refresh_ms;
    bool help;
    int confirm_pid;           // a stop being asked about, or 0
    char note[80];             // the bottom line's message, after an action
} H;

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

// --- reading -------------------------------------------------------------------

static void sample(void) {
    FwTaskInfo now[MAXT];
    int n = fw_task_list(now, MAXT);
    uint32_t t = fw_millis();
    uint32_t dt = t - H.last_ms;
    if (!dt) dt = 1;
    unsigned busy[2] = {0, 0};
    H.n = 0;
    for (int i = 0; i < n; i++) {
        uint32_t before = now[i].cpu_ms;
        for (int j = 0; j < H.nprev; j++)
            if (H.prev[j].pid == now[i].pid) { before = H.prev[j].cpu_ms; break; }
        uint32_t d = now[i].cpu_ms >= before ? now[i].cpu_ms - before : 0;
        Row &r = H.rows[H.n++];
        r.t = now[i];
        // pid 1 is the idle task: its time is the time nothing else wanted the
        // core, so it is shown but counts for nothing in the meters.
        r.cpu10 = now[i].pid == 1 ? 0 : (unsigned)((uint64_t)d * 1000 / dt);
        if (r.cpu10 > 1000) r.cpu10 = 1000;
        if (now[i].pid != 1 && now[i].core < 2) busy[now[i].core] += d;
    }
    for (int c = 0; c < 2; c++) {
        unsigned v = (unsigned)((uint64_t)busy[c] * 1000 / dt);
        H.core10[c] = v > 1000 ? 1000 : v;
    }
    memcpy(H.prev, now, sizeof(FwTaskInfo) * (unsigned)n);
    H.nprev = n;
    H.last_ms = t;
}

static int cmp(const Row &a, const Row &b) {
    int r = 0;
    switch (H.sort) {
        case S_CPU:   r = (int)b.cpu10 - (int)a.cpu10; break;
        case S_STACK: r = (int)b.t.stack_used - (int)a.t.stack_used; break;
        case S_TIME:  r = (b.t.cpu_ms > a.t.cpu_ms) - (b.t.cpu_ms < a.t.cpu_ms); break;
        case S_PID:   r = a.t.pid - b.t.pid; break;
        case S_NAME:  r = strcmp(a.t.name, b.t.name); break;
        default: break;
    }
    if (!r) r = a.t.pid - b.t.pid;
    return H.rev ? -r : r;
}

static void sort_rows(void) {                // insertion sort: never more than 32
    for (int i = 1; i < H.n; i++) {
        Row v = H.rows[i];
        int j = i - 1;
        while (j >= 0 && cmp(H.rows[j], v) > 0) { H.rows[j + 1] = H.rows[j]; j--; }
        H.rows[j + 1] = v;
    }
}

// --- drawing -------------------------------------------------------------------

static void fmt_kb(char *out, unsigned cap, uint32_t bytes) {
    if (bytes >= 10 * 1024) snprintf(out, cap, "%luK", (unsigned long)(bytes / 1024));
    else snprintf(out, cap, "%lu.%luK", (unsigned long)(bytes / 1024), (unsigned long)(bytes % 1024 * 10 / 1024));
}

// An htop meter: "label[|||||      text]" with the bars coloured by level.
static void meter(int x, int y, int w, const char *label, unsigned permille, const char *text) {
    fw_tui_text(x, y, label, FW_ATTR_BOLD, C_CYN);
    int lx = x + (int)strlen(label);
    fw_tui_text(lx, y, "[", FW_ATTR_BOLD, 0);
    int inner = w - (int)strlen(label) - 2;
    if (inner < 4) return;
    int bars = (int)((uint64_t)permille * (unsigned)inner / 1000);
    int tlen = (int)strlen(text);
    char cell[2] = {0, 0};
    for (int i = 0; i < inner; i++) {
        bool in_text = i >= inner - tlen;
        unsigned char fg = permille < 500 ? C_GRN : permille < 800 ? C_YEL : C_RED;
        if (in_text) { cell[0] = text[i - (inner - tlen)]; fw_tui_text(lx + 1 + i, y, cell, FW_ATTR_DIM, 0); }
        else if (i < bars) fw_tui_text(lx + 1 + i, y, "|", FW_ATTR_BOLD, fg);
        else fw_tui_text(lx + 1 + i, y, " ", 0, 0);
    }
    fw_tui_text(lx + 1 + inner, y, "]", FW_ATTR_BOLD, 0);
}

static void temp_c(char *out, unsigned cap) {
    unsigned ch = fw_adc_temp_channel();
    if (fw_adc_init(ch) != 0) { snprintf(out, cap, "n/a"); return; }
    int raw = fw_adc_read(ch);
    if (raw < 0) { snprintf(out, cap, "n/a"); return; }
    double c = 27.0 - (raw * 3.3 / 4096.0 - 0.706) / 0.001721;      // as sysmon and gpio
    snprintf(out, cap, "%d C", (int)(c + (c < 0 ? -0.5 : 0.5)));
}

static void draw_help(void) {
    static const char *const lines[] = {
        "htop for RPCortex",
        "",
        "  Up/Down PgUp/PgDn Home/End    move the selection",
        "  F6, < or >                    sort by the next column",
        "  P  CPU    M  stack    T  time    N  pid    I  reverse",
        "  F9 or k                       stop the selected task (asks first)",
        "  + / -                         refresh faster / slower",
        "  F1 or h                       this help",
        "  F10, q or Esc                 quit       Ctrl+L  redraw",
        "",
        "  CPU% is each task's share of one core over the last refresh.",
        "  The idle task (pid 1) is listed but counts for nothing.",
        "  Stopping follows kill: what is not yours needs an admin.",
        "",
        "  Any key to go back.",
    };
    fw_tui_clear();
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    for (int i = 0; i < n && i + 1 < H.h; i++)
        fw_tui_text(2, i + 1, lines[i], i == 0 ? FW_ATTR_BOLD : 0, i == 0 ? C_CYN : 0);
    fw_tui_present();
}

static void draw(void) {
    if (H.help) { draw_help(); return; }
    fw_tui_clear();
    int half = H.w / 2;
    int y = 0;
    char text[48], a[16], b[16];

    // Left column: a meter per core, then memory and flash.
    unsigned cores = fw_cores();
    for (unsigned c = 0; c < cores && c < 2; c++) {
        char label[8];
        snprintf(label, sizeof(label), "%3u", c);
        snprintf(text, sizeof(text), "%u.%u%%", H.core10[c] / 10, H.core10[c] % 10);
        meter(1, y++, half - 2, label, H.core10[c], text);
    }
    unsigned long tot = fw_heap_total(), fre = fw_heap_free(), used = tot > fre ? tot - fre : 0;
    fmt_kb(a, sizeof(a), (uint32_t)used); fmt_kb(b, sizeof(b), (uint32_t)tot);
    snprintf(text, sizeof(text), "%s/%s", a, b);
    meter(1, y++, half - 2, "Mem", tot ? (unsigned)(used * 1000 / tot) : 0, text);
    FwStorageRoot roots[2];
    int nr = fw_storage_roots(roots, 2);
    for (int i = 0; i < nr; i++) {
        if (!roots[i].present || !roots[i].total_kb) continue;
        unsigned long t = roots[i].total_kb, f = roots[i].free_kb, u = t > f ? t - f : 0;
        snprintf(text, sizeof(text), "%luK/%luK", u, t);
        meter(1, y++, half - 2, i == 0 ? "Dsk" : "SD ", (unsigned)(u * 1000 / t), text);
        break;                                 // the internal one; sysmon shows both
    }

    // Right column: the summary htop puts beside its meters.
    int ry = 0, running = 0;
    for (int i = 0; i < H.n; i++) if (H.rows[i].t.state == 'R') running++;
    char line[96];
    snprintf(line, sizeof(line), "Tasks: %d, %d running", H.n, running);
    fw_tui_text(half, ry++, line, FW_ATTR_BOLD, 0);
    unsigned load = (H.core10[0] + H.core10[1]) / (cores ? cores : 1);
    snprintf(line, sizeof(line), "Load: %u.%u%%   Clock: %lu MHz", load / 10, load % 10,
             (unsigned long)(fw_clock_hz() / 1000000u));
    fw_tui_text(half, ry++, line, FW_ATTR_BOLD, 0);
    uint32_t s = fw_millis() / 1000;
    temp_c(a, sizeof(a));
    int p = fw_power_source();
    snprintf(line, sizeof(line), "Uptime: %02lu:%02lu:%02lu   %s   %s",
             (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60), a,
             p == FW_POWER_USB ? "USB" : p == FW_POWER_BATTERY ? "battery" : "");
    fw_tui_text(half, ry++, line, FW_ATTR_BOLD, 0);
    if (ry > y) y = ry;
    y++;

    // The header row, with the sort column marked.
    fw_tui_fill(0, y, H.w, 1, ' ', FW_ATTR_REVERSE, C_GRN);
    static const char *const cols = "  PID NAME            S CORE  CPU%  STACK    TIME+  FROM";
    fw_tui_text(0, y, cols, FW_ATTR_REVERSE, C_GRN);
    int list_y = y + 1;
    int list_h = H.h - list_y - 1;
    if (list_h < 1) list_h = 1;

    if (H.sel >= H.n) H.sel = H.n - 1;
    if (H.sel < 0) H.sel = 0;
    if (H.sel < H.top) H.top = H.sel;
    if (H.sel >= H.top + list_h) H.top = H.sel - list_h + 1;

    for (int i = 0; i < list_h && H.top + i < H.n; i++) {
        const Row &r = H.rows[H.top + i];
        uint32_t cs = r.t.cpu_ms / 10;          // centiseconds
        char stk[16];
        fmt_kb(stk, sizeof(stk), r.t.stack_used);
        snprintf(line, sizeof(line), "%5d %-15.15s %c %4u %3u.%u %6s %3lu:%02lu.%02lu  %s",
                 r.t.pid, r.t.name, r.t.state, (unsigned)r.t.core,
                 r.cpu10 / 10, r.cpu10 % 10, stk,
                 (unsigned long)(cs / 6000), (unsigned long)(cs / 100 % 60), (unsigned long)(cs % 100),
                 r.t.path);
        bool selected = H.top + i == H.sel;
        if (selected) fw_tui_fill(0, list_y + i, H.w, 1, ' ', FW_ATTR_REVERSE, C_CYN);
        unsigned char fg = selected ? C_CYN : r.t.state == 'R' ? C_GRN : r.t.pid == 1 ? 0 : 0;
        fw_tui_text(0, list_y + i, line, selected ? FW_ATTR_REVERSE : (r.t.pid == 1 ? FW_ATTR_DIM : 0), fg);
    }

    // The function-key bar, or a question, or what the last action did.
    int by = H.h - 1;
    fw_tui_fill(0, by, H.w, 1, ' ', 0, 0);
    if (H.confirm_pid) {
        snprintf(line, sizeof(line), "Stop pid %d? y to stop, any other key to keep it", H.confirm_pid);
        fw_tui_text(0, by, line, FW_ATTR_REVERSE, C_RED);
    } else if (H.note[0]) {
        fw_tui_text(0, by, H.note, FW_ATTR_BOLD, C_YEL);
    } else {
        static const char *const keys[][2] = {
            {"F1", "Help  "}, {"F6", "SortBy"}, {"F9", "Kill  "}, {"F10", "Quit  "},
        };
        int x = 0;
        for (auto &k : keys) {
            fw_tui_text(x, by, k[0], FW_ATTR_BOLD, 0);
            x += (int)strlen(k[0]);
            fw_tui_text(x, by, k[1], FW_ATTR_REVERSE, C_CYN);
            x += (int)strlen(k[1]) + 1;
        }
        snprintf(line, sizeof(line), "sorted by %s%s, every %d ms", kSortName[H.sort],
                 H.rev ? " (reversed)" : "", H.refresh_ms);
        if (x + (int)strlen(line) + 1 < H.w) fw_tui_text(H.w - (int)strlen(line) - 1, by, line, FW_ATTR_DIM, 0);
    }
    fw_tui_present();
}

// --- the command ----------------------------------------------------------------

static int htop_cmd(int argc, char **argv) {
    if (argc > 1 && (streq(argv[1], "-h") || streq(argv[1], "--help") || streq(argv[1], "help"))) {
        fw_printf("htop [ms]   the interactive process viewer (also: top)\n");
        fw_printf("  F1 help   F6 sort   F9 stop a task   F10 or q quit   + - refresh rate\n");
        return 0;
    }
    memset(&H, 0, sizeof(H));
    H.refresh_ms = (int)fw_reg_get_int("Apps.Htop_Refresh", 1500);
    if (argc > 1) {
        int v = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        if (v) H.refresh_ms = v;
    }
    if (H.refresh_ms < 250) H.refresh_ms = 250;
    if (H.refresh_ms > 10000) H.refresh_ms = 10000;

    fw_tui_begin();
    fw_tui_size(&H.w, &H.h);
    if (H.w < 40) { H.w = 80; H.h = 24; }
    H.last_ms = fw_millis();
    sample();                                   // the baseline for the first interval
    fw_task_sleep_ms(250);
    sample();
    sort_rows();

    uint32_t next = fw_millis() + (uint32_t)H.refresh_ms;
    bool dirty = true, running = true;
    while (running) {
        if ((int32_t)(fw_millis() - next) >= 0) {
            int keep = H.n && H.sel < H.n ? H.rows[H.sel].t.pid : 0;
            sample();
            sort_rows();
            for (int i = 0; i < H.n; i++) if (H.rows[i].t.pid == keep) H.sel = i;   // follow the task
            next = fw_millis() + (uint32_t)H.refresh_ms;
            dirty = true;
        }
        if (dirty) { draw(); dirty = false; }

        FwTuiEvent e;
        if (!fw_tui_poll(&e)) {
            if (fw_task_should_stop()) break;
            fw_task_sleep_ms(10);
            continue;
        }
        if (e.kind != 1) continue;
        dirty = true;
        int k = e.key;

        if (H.help) { H.help = false; continue; }
        if (H.confirm_pid) {
            int pid = H.confirm_pid;
            H.confirm_pid = 0;
            if (k == 'y' || k == 'Y') {
                int rc = fw_task_kill(pid);
                if (rc == 1)       snprintf(H.note, sizeof(H.note), "Asked pid %d to stop.", pid);
                else if (rc < 0)   snprintf(H.note, sizeof(H.note), "pid %d cannot be stopped (the shell, the idle task, or already done).", pid);
                else               snprintf(H.note, sizeof(H.note), "pid %d is not yours to stop. An admin can.", pid);
                next = fw_millis() + 300;      // show it go
            }
            continue;
        }
        H.note[0] = 0;
        int page = H.h - 8 > 1 ? H.h - 8 : 1;
        switch (k) {
            case 'q': case 'Q': case FW_KEY_F10: case FW_KEY_ESC: case 3: running = false; break;
            case FW_KEY_UP:   H.sel--; break;
            case FW_KEY_DOWN: H.sel++; break;
            case FW_KEY_PGUP: H.sel -= page; break;
            case FW_KEY_PGDN: H.sel += page; break;
            case FW_KEY_HOME: H.sel = 0; break;
            case FW_KEY_END:  H.sel = H.n - 1; break;
            case FW_KEY_F6: case '>': case '.': H.sort = (Sort)((H.sort + 1) % S_COUNT); sort_rows(); break;
            case '<': case ',': H.sort = (Sort)((H.sort + S_COUNT - 1) % S_COUNT); sort_rows(); break;
            case 'P': H.sort = S_CPU;   sort_rows(); break;
            case 'M': H.sort = S_STACK; sort_rows(); break;
            case 'T': H.sort = S_TIME;  sort_rows(); break;
            case 'N': H.sort = S_PID;   sort_rows(); break;
            case 'I': H.rev = !H.rev;   sort_rows(); break;
            case FW_KEY_F9: case 'k':
                if (H.n) H.confirm_pid = H.rows[H.sel].t.pid;
                break;
            case '+': case '=':
                H.refresh_ms = H.refresh_ms > 500 ? H.refresh_ms - 250 : 250;
                break;
            case '-': case '_':
                H.refresh_ms = H.refresh_ms < 10000 ? H.refresh_ms + 250 : 10000;
                break;
            case FW_KEY_F1: case 'h': case '?': H.help = true; break;
            case 12: if (fw_tui_refresh()) fw_tui_size(&H.w, &H.h); break;
            default: dirty = false; break;
        }
    }
    fw_tui_end();
    char v[12];
    snprintf(v, sizeof(v), "%d", H.refresh_ms);
    fw_reg_set("Apps.Htop_Refresh", v);
    fw_reg_save();
    return 0;
}

extern "C" int app_main(int arg) {
    (void)arg;
    // sysmon 2.1 and older also answered to `htop`. With one of those loaded the
    // name is taken, and saying so beats a command that silently is not there.
    if (rpc_register_command("htop", "the interactive process viewer", htop_cmd) != 1)
        fw_log(1, "htop: the name 'htop' is taken (sysmon 2.1 or older?). "
                  "'pkg upgrade sysmon' and restart, or use 'top'.");
    rpc_register_command("top", "the interactive process viewer (as htop)", htop_cmd);
    return 0;
}
