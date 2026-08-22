// Desc: The firmware, faked, for anything that drives the Nova D1 on a host.
// File: fakefw_d1.inc
//
// Shared by novagui_test, which asserts against the runner, and novashots,
// which photographs it. One copy because two would drift, and a host harness
// that disagrees with the other host harness is worse than one of them not
// existing — the frame_dt bug survived two passes precisely because the test
// kept its own copy of the runner's loop.
//
// Small and honest rather than clever: enough for the runner to run, with the
// pieces a caller wants to control exposed as globals it can assign.
//
// INCLUDED, not linked. Everything here is static or in one anonymous
// translation unit by construction: both users compile the whole package into a
// single unit so the runner can be driven directly, and a separate object would
// need the package's internals exported to reach them.
#ifndef RPC_FAKEFW_D1_INC
#define RPC_FAKEFW_D1_INC

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

#include "../include/rpc_app.h"

// --- the firmware, faked ----------------------------------------------------------
//
// Small and honest rather than clever: enough for the runner to run, with the
// pieces a test wants to control exposed as globals.

static uint32_t g_ms;
static char     g_reg[32][2][128];
static int      g_regn;
static bool     g_panel_present = true;
static int      g_spawned;
static int      g_run_spawned;      // see fw_task_spawn
static int      g_i2c_writes;

// Whether the faked btaudio has a speaker and a track. Two shapes rather than
// one, because the media screens have to render both and the not-connected one
// is what a device out of the box actually says.
static int      g_bta_playing;

extern "C" {

uint32_t fw_millis(void) { return g_ms; }
uint32_t fw_micros(void) { return g_ms * 1000u; }
void     fw_task_sleep_ms(uint32_t ms) { g_ms += ms; }
void     fw_task_yield(void) {}
// Scriptable, because a worker loop that does not check it is a worker that
// cannot be shut down and nothing else can see the difference.
static int g_should_stop;
int      fw_task_should_stop(void) { return g_should_stop; }
int      fw_task_self(void) { return 1; }
uint32_t fw_cores(void) { return 2; }
uint32_t fw_core_id(void) { return 0; }
uint32_t fw_cpu_percent(void) { return 7; }
void     fw_reboot(void) {}

// Spawning is COUNTED rather than performed. A second runner is what this
// harness is here to catch, and counting the attempt is how it catches it
// without needing threads.
//
// g_run_spawned makes it RUN the body instead, inline, on the caller's stack.
// Off by default, because every screen that spawns a worker would otherwise
// complete it before the frame it was asked on and no shot would ever show the
// spinner state a device really does spend seconds in. A test that wants the
// round trip — a command sent, a reply parsed, a screen updated — turns it on
// around itself and turns it off again.
//
// NEVER around gui::begin(). The input task's body is a loop that only ends
// when the runner stops, so running it inline never returns.
int fw_task_spawn(const char *, int (*fn)(void *), void *arg, uint32_t) {
    g_spawned++;
    if (g_run_spawned && fn) fn(arg);
    return g_spawned + 10;
}
int fw_task_kill(int) { return 1; }

// A command prints with fw_printf, and on a host that would just vanish to
// stdout — so a test of what `novad1 setup` or `service status` SAYS has nothing
// to read. This keeps the last few kilobytes of it, with a reset a test calls
// before the command it is about to inspect. Off nobody's path but the tests that
// ask: a command that does not print writes nothing here either.
static char g_printf_buf[4096];
static int  g_printf_n;
static void printf_reset(void) { g_printf_n = 0; g_printf_buf[0] = 0; }

int fw_printf(const char *fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    int room = (int)sizeof(g_printf_buf) - 1 - g_printf_n;
    if (room > 0) {
        int c = snprintf(g_printf_buf + g_printf_n, room + 1, "%s", tmp);
        g_printf_n += (c < room ? c : room);
    }
    return k;
}
void fw_log(int, const char *) {}
void fw_progress(const char *) {}
int  rpc_register_command(const char *, const char *, int (*)(int, char **)) { return 1; }
// A shell that answers, for the handful of commands the screens read back.
//
// It returned an empty buffer, which is a legal answer — the OS has ONE capture
// buffer and a command whose output could not be captured comes back empty — but
// it is the wrong DEFAULT. Every screen that reads a listing rendered as though
// the device had no tasks, no services and no packages, so a renderer driving
// them photographed empty columns and a test asserting on them asserted nothing.
//
// The text below is copied from a real Pico 2 W, spacing included, because the
// screens parse it. A prettier fake would pass while the real format failed.
// One scripted answer, overriding the table below for lines starting with
// g_shell_match. Here rather than in whichever test wanted it first, because
// the table IS one real device's output and a test that needed a different
// answer used to have to edit it — which changes what every other test sees.
//
// g_shell_rc is what fw_shell_run returns, scripted the same way: a command
// that printed something and still failed is a case worth being able to build.
static const char *g_shell_match;
static const char *g_shell_reply;
static int         g_shell_rc;

static void shell_says(const char *prefix, const char *reply, int rc = 0) {
    g_shell_match = prefix;
    g_shell_reply = reply;
    g_shell_rc    = rc;
}

// The last line asked for, so a test can check WHICH command a screen ran and
// not only what it did with the answer.
static char g_last_shell[128];

static const char *fake_shell(const char *line) {
    snprintf(g_last_shell, sizeof(g_last_shell), "%s", line ? line : "");
    if (g_shell_match && !strncmp(line, g_shell_match, strlen(g_shell_match)))
        return g_shell_reply ? g_shell_reply : "";
    // Copied from os/shell/pkgrepo.cpp do_update, os/shell/session.cpp
    // cmd_autonomy, os/shell/update.cpp do_check and os/shell/diag.cpp
    // cmd_safeboot. The Updates screen parses all four.
    if (!strncmp(line, "pkg update", 10))
        return "[:] Fetching package lists...\n"
               "[:] https://raw.githubusercontent.com/dash1101/RPCortex-repo/main/repo-v2/index.json\n"
               "[@]   15 packages\n"
               "[@] 15 packages from 1 repo.\n";
    if (!strncmp(line, "autonomy status", 15))
        return "[@] [autonomy] On, as 'root'.\n";
    // "<name>" or "<name>  (admin)", from cmd_whoami's one out_multi.
    if (!strncmp(line, "whoami", 6))
        return "root  (admin)\n";
    if (!strncmp(line, "update check", 12))
        return "  Installed  v2.0.0 (build 315)\n"
               "  Available  2.0.0.315\n"
               "  Size       955 KB\n"
               "[@] Already up to date.\n";
    if (!strncmp(line, "safeboot", 8))
        return "[:] Safe boot: restarting to run 'script /nova/update.rps' on its own.\n";
    if (!strncmp(line, "ps", 2))
        return "  PID  NAME         STATE  CORE STACK       CPU      \n"
               "  1    init         ready  0    0B/4.0K     1.6s     \n"
               "  2    usb          ready  0    432B/8.0K   1.2s     \n"
               "  3    shell        run    0    2.1K/8.0K   2.1s     \n"
               "  4    novad1       done   1    796B/3.0K   6ms      \n"
               "  5    novainput    sleep  1    372B/2.0K   5ms      \n"
               "  6    novagui      sleep  1    316B/2.0K   609ms    \n"
               "  6 tasks  ,  CPU 10%  ,  86 KB free\n";
    if (!strncmp(line, "service", 7))
        return "[:] Services:\n"
               "   1  novad1 gui --bg\n"
               "  Live state is in 'ps'; services run as ordinary tasks.\n";
    if (!strncmp(line, "pkg list", 8))
        return "[:] Installed packages:\n  novad1           1.1.0  \n";
    if (!strncmp(line, "pkg search", 10))
        return "  bench      1.0   RPCMark, the same workload as the MicroPython build\n"
               "  calc       1.0   Shell calculator\n"
               "  novad1     1.1.0 Nova D1 - the handheld multi-tool\n";
    if (!strncmp(line, "df", 2))
        return "  Total : 2048 KB\n  Used  : 260 KB  (12%)\n  Free  : 1788 KB\n";
    if (!strncmp(line, "free", 4) || !strncmp(line, "meminfo", 7))
        return "  Total : 301 KB\n  Used  : 217 KB  (72%)\n  Free  : 83 KB\n"
               "  Largest block : 29 KB\n";
    if (!strncmp(line, "radio", 5))
        return "  Radio  : on\n  Link   : connected\n  Network: dash_\n";
    // Before the generic "bt " below, because a classic inquiry finds speakers
    // and an LE scan does not — the media player asks for one and would
    // otherwise be handed the other, which is the mistake the ordering here
    // exists to stop somebody making again.
    if (!strncmp(line, "bt scan classic", 15))
        return "[:] Scanning for 10 seconds...\n"
               "[:] 2 devices\n"
               "  C8:7B:23:11:04:9A  BR    -62 dBm  JBL Flip 5\n"
               "  30:21:5B:9E:44:10  BR    -77 dBm  Soundcore 2\n";
    if (!strncmp(line, "bt ", 3))
        return "[:] 3 devices\n"
               "  D9:33:36:32:1B:59  LE    -78 dBm  Govee_H6199_1B59\n"
               "  37:96:7B:89:C2:EB  LE    -80 dBm  \n"
               "  53:0A:FB:D0:FF:87  LE    -92 dBm  Quest 3S\n";

    // btaudio, copied from the out_multi format strings in os/shell/btaudio.cpp
    // — the two-space indent and the column padding included, because the media
    // screens parse this and a prettier fake would pass while the real thing
    // failed.
    if (!strncmp(line, "btaudio status", 14))
        return g_bta_playing
            ? "[:] Bluetooth audio\n"
              "  Name       RPCortex\n"
              "  Speaker    C8:7B:23:11:04:9A\n"
              "  Playing    /nova/music/Ambient/drift.wav\n"
              "  Position   37%  (44100 Hz, 2 channels)\n"
              "  Volume     65%\n"
              "  Underruns  0\n"
            : "[:] Bluetooth audio\n"
              "  Name       RPCortex\n"
              "  Speaker    not connected\n"
              "  Playing    nothing\n"
              "  Volume     80%\n"
              "  Underruns  0\n";
    if (!strncmp(line, "btaudio connect", 15))
        return "[:] Connecting to C8:7B:23:11:04:9A...\n"
               "[@] Connected to C8:7B:23:11:04:9A.\n";
    if (!strncmp(line, "btaudio play", 12))
        return "[@] Playing /nova/music/Ambient/drift.wav (44100 Hz, 2 channels).\n";
    if (!strncmp(line, "btaudio stop", 12))
        return "[@] Stopped.\n";
    if (!strncmp(line, "btaudio volume", 14))
        return "[@] Volume 65%.\n";
    // The two contact readers. Column positions copied from the real commands
    // in shell/nfc.cpp and shell/ibutton.cpp — every value starts at column 12
    // — because the screens find a row by its NAME and then step over the
    // spacing, and a fake that was merely tidy would pass while the real
    // format failed.
    //
    // The iButton ROM below is a REAL one in the sense that matters: 3D is the
    // genuine Dallas CRC of the seven bytes in front of it, so a screen that
    // ever grows a checksum test of its own agrees with this rather than
    // rejecting it.
    if (!strncmp(line, "nfc status", 10))
        return "[:] NFC\n"
               "  Chip      PN532 on I2C0, GPIO 0/1 at 400 kHz\n"
               "  Address   0x24 answered\n"
               "  Shared    the same at the rate the panel uses\n"
               "  Firmware  PN532 v1.6\n";
    if (!strncmp(line, "nfc read", 8))
        return "[:] Tag\n"
               "  UID       04 A2 B3 C4 D5 E6 F7\n"
               "  Type      NTAG / Ultralight\n"
               "  ATQA      44 00\n"
               "  SAK       00\n";
    if (!strncmp(line, "ibutton read", 12))
        return "[:] iButton\n"
               "  ROM       01 A2 B3 C4 D5 E6 F7 3D\n"
               "  Family    01  DS1990A / DS2401\n"
               "  CRC       ok\n";
    // The two SPI radios. Copied byte for byte from the out_ok/out_info/out_multi
    // format strings in os/shell/cc1101.cpp and os/shell/sx1276.cpp — the tags
    // ([@]/[:]) are what fw_shell_run captures, the indented lines are the data
    // channel. radio_test asserts the screen parser against these exact strings,
    // so a prettier fake would pass while the real format failed.
    if (!strncmp(line, "subghz status", 13))
        return "[:] Sub-GHz  CC1101\n"
               "  Chip    present  (part 0x00 ver 0x14)\n"
               "  Freq    433.92 MHz\n"
               "  Mode    OOK/ASK fixed-code\n";
    if (!strncmp(line, "subghz rx", 9))
        return "[@] Captured 6 pulses at 433.92 MHz\n"
               "350,700,350,700,350,700\n";
    if (!strncmp(line, "subghz tx", 9))
        return "[@] Sent 6 pulses at 433.92 MHz\n";
    if (!strncmp(line, "subghz freq", 11))
        return "[@] Frequency set to 433.92 MHz\n";
    if (!strncmp(line, "lora status", 11))
        return "[:] LoRa  SX1276\n"
               "  Chip   present  (ver 0x12)\n"
               "  Freq   915.00 MHz\n"
               "  Modem  SF7 BW125 CR4/5\n";
    if (!strncmp(line, "lora config", 11))
        return "[@] LoRa 915.00 MHz SF7 BW125 CR4/5\n";
    if (!strncmp(line, "lora send", 9))
        return "[@] Sent 5 bytes\n";
    if (!strncmp(line, "lora recv", 9) || !strncmp(line, "lora rx", 7))
        return "[@] RX 48656c6c6f rssi -42 snr 9\n";
    return "";
}

// --- a mutable startup / service model, OFF by default -----------------------
//
// The scripted answers above are fixed strings, which is right for a screen that
// reads one listing back — and wrong for `novad1 setup`, which lists a joblist,
// removes an entry, and lists again expecting it gone. Proving that migration
// needs the list to actually change between calls, so this models the two
// joblists just enough — list, add, remove — and stays off until a test asks for
// it, so every screen test above still sees the one real device's output.
static bool g_jobs_live;
static char g_startup_jobs[8][160];  static int g_startup_n;
static char g_service_jobs[8][160];  static int g_service_n;

static void jobs_reset(void) {
    g_jobs_live = true;
    g_startup_n = g_service_n = 0;
}
static void jobs_seed(bool startup, const char *cmd) {
    char (*j)[160] = startup ? g_startup_jobs : g_service_jobs;
    int  *n        = startup ? &g_startup_n   : &g_service_n;
    if (*n < 8) snprintf(j[(*n)++], 160, "%s", cmd);
}
static int jobs_count(bool startup) { return startup ? g_startup_n : g_service_n; }
static bool jobs_has(bool startup, const char *needle) {
    char (*j)[160] = startup ? g_startup_jobs : g_service_jobs;
    int    n       = startup ? g_startup_n     : g_service_n;
    for (int i = 0; i < n; i++) if (strstr(j[i], needle)) return true;
    return false;
}

// The real joblist format, colour stripped: a header, then "  <1-based idx>
// <cmd>", then "(none)" when empty. 1-based because joblist_walk pre-increments,
// which is exactly what listing_index_of and `remove <n>` both count on. Mutates
// the model even when out is null, because the real shell runs the command
// whether or not its output is captured — and setup's remove passes null.
static int jobs_shell(const char *line, char *out, uint32_t cap) {
    bool startup   = !strncmp(line, "startup", 7);
    char (*j)[160] = startup ? g_startup_jobs : g_service_jobs;
    int  *n        = startup ? &g_startup_n   : &g_service_n;
    const char *label = startup ? "Startup commands" : "Services";

    const char *sub = line + 7;
    while (*sub == ' ') sub++;

    if (!*sub || !strncmp(sub, "list", 4)) {
        if (out && cap) {
            int w = snprintf(out, cap, "%s:\n", label);
            if (*n == 0 && w < (int)cap) snprintf(out + w, cap - w, "  (none)\n");
            for (int i = 0; i < *n && w < (int)cap; i++)
                w += snprintf(out + w, cap - w, "  %2d  %s\n", i + 1, j[i]);
        }
        return 0;
    }
    if (!strncmp(sub, "add", 3)) {
        const char *cmd = sub + 3; while (*cmd == ' ') cmd++;
        if (*cmd && *n < 8) snprintf(j[(*n)++], 160, "%s", cmd);
        if (out && cap) out[0] = 0;
        return 0;
    }
    if (!strncmp(sub, "remove", 6)) {
        int idx = atoi(sub + 6);                       // 1-based, as printed
        if (idx >= 1 && idx <= *n) {
            for (int i = idx - 1; i < *n - 1; i++) snprintf(j[i], 160, "%s", j[i + 1]);
            (*n)--;
            if (out && cap) out[0] = 0;
            return 0;
        }
        if (out && cap) snprintf(out, cap, "No entry %d.\n", idx);
        return 1;                                       // real remove refuses a bad index
    }
    if (out && cap) out[0] = 0;
    return 0;
}

// No network in the fake. novad1cmd.cpp reaches for this in `novad1 selfupdate`,
// which no host test drives; it is here so the file links.
int fw_http_download(const char *, const char *) { return -1; }

int fw_shell_run(const char *line, char *out, uint32_t cap) {
    snprintf(g_last_shell, sizeof(g_last_shell), "%s", line ? line : "");
    if (g_jobs_live && line &&
        (!strncmp(line, "startup", 7) || !strncmp(line, "service", 7)))
        return jobs_shell(line, out, cap);
    if (!out || !cap) return 0;
    snprintf(out, cap, "%s", line ? fake_shell(line) : "");
    return g_shell_rc;
}

void *fw_malloc(size_t n) { return malloc(n); }
void  fw_free(void *p) { free(p); }
uint32_t fw_heap_free(void) { return 240 * 1024; }
uint32_t fw_heap_total(void) { return 300 * 1024; }
uint32_t fw_heap_largest(void) { return 200 * 1024; }

int  fw_reg_get(const char *k, char *out, uint32_t cap) {
    for (int i = 0; i < g_regn; i++)
        if (!strcmp(g_reg[i][0], k)) { snprintf(out, cap, "%s", g_reg[i][1]); return 1; }
    if (cap) out[0] = 0;
    return 0;
}
int fw_reg_set(const char *k, const char *v) {
    for (int i = 0; i < g_regn; i++)
        if (!strcmp(g_reg[i][0], k)) { snprintf(g_reg[i][1], 128, "%s", v); return 1; }
    if (g_regn >= 32) return 0;
    snprintf(g_reg[g_regn][0], 128, "%s", k);
    snprintf(g_reg[g_regn][1], 128, "%s", v);
    g_regn++;
    return 1;
}
int32_t fw_reg_get_int(const char *k, int32_t d) {
    char b[128];
    if (!fw_reg_get(k, b, sizeof(b)) || !b[0]) return d;
    return (int32_t)atoi(b);
}
int  fw_reg_has(const char *k) { char b[4]; return fw_reg_get(k, b, sizeof(b)); }
void fw_reg_save(void) {}

int      fw_file_exists(const char *) { return 1; }
int      fw_mkdir(const char *) { return 1; }

// Writes are KEPT rather than discarded. The Updates screen's whole product is
// a file the next boot reads, so a test that could not see what was written
// could only check that something was.
static char g_write_path[64];
static char g_write_body[512];
static int  g_writes;
static int  g_write_fails;          // when set, the next write is refused
static char g_removed[64];

int fw_file_write(const char *path, const void *data, uint32_t len) {
    if (g_write_fails) { g_write_fails--; return 0; }
    g_writes++;
    snprintf(g_write_path, sizeof(g_write_path), "%s", path ? path : "");
    uint32_t n = len < sizeof(g_write_body) - 1 ? len : (uint32_t)sizeof(g_write_body) - 1;
    if (data) memcpy(g_write_body, data, n);
    g_write_body[n] = 0;
    return 1;
}
// A read is answered by the little filesystem further down, beside the media
// library it grew out of.
uint32_t fw_file_read(const char *path, void *buf, uint32_t cap);
int      fw_file_remove(const char *p) {
    snprintf(g_removed, sizeof(g_removed), "%s", p ? p : "");
    return 1;
}

int fw_board(char *out, unsigned cap) { snprintf(out, cap, "pico2_w"); return 1; }
unsigned fw_gpio_count(void) { return 30; }
int fw_gpio_usable(unsigned p) { return (p < 30 && p != 23 && p != 24 && p != 25 && p != 29) ? 1 : 0; }
// Scripted pin levels, OFF by default so every existing test still reads the
// resting 1 (pull-ups, nothing pressed). The encoder-direction test turns it on
// and walks the two phases through a detent, which is the only way to exercise
// the quadrature decode from a host — an injected gesture is post-decode.
static bool g_gpio_script = false;
static int  g_gpio_level[40];
int fw_gpio_init(unsigned, int) { return 0; }
int fw_gpio_pull(unsigned, int) { return 0; }
int fw_gpio_put(unsigned, int) { return 0; }
int fw_gpio_get(unsigned pin) {
    if (g_gpio_script && pin < 40) return g_gpio_level[pin];
    return 1;
}
int fw_gpio_watch(unsigned, int) { return 0; }
int fw_gpio_events(unsigned, int *level) { if (level) *level = 1; return 0; }
int fw_adc_init(unsigned) { return 0; }
int fw_adc_read(unsigned) { return -1; }
unsigned fw_adc_temp_channel(void) { return 4; }

int fw_i2c_init(unsigned, unsigned, unsigned, unsigned) { return 0; }
int fw_i2c_write(unsigned, unsigned, const void *, unsigned, int) {
    g_i2c_writes++;
    return g_panel_present ? 0 : -1;
}
int fw_i2c_read(unsigned, unsigned, void *, unsigned, int) { return -1; }
int fw_i2c_deinit(unsigned) { return 0; }

int fw_pwm_init(unsigned, unsigned) { return 1000; }
int fw_pwm_duty(unsigned, unsigned) { return 0; }
int fw_pwm_stop(unsigned) { return 0; }
void fw_busy_wait_us(uint32_t) {}

// --- the network, faked ------------------------------------------------------------
//
// OFFLINE BY DEFAULT, and that is the important half: a board out of the box has
// no network, so every screenshot novashots takes is of the state a device is
// actually in when somebody first opens the app. A test that wants the other
// state sets these and puts them back.
static int         g_net_up;                    // 0 -> not joined
static const char *g_net_ip = "192.168.1.42";
static const char *g_net_ssid_s = "Nova";

// Which addresses answer a ping, as a set of last octets, and how long each
// takes. A LIST rather than a predicate because the interesting cases are all
// "these three out of 254" and a predicate would have to be rewritten for each.
static uint8_t  g_ping_hosts[8];
// MICROSECONDS, and 32 bits of them. It was 16, which silently wrapped anything
// over 65 ms — so a test asking for a slow reply got a fast one and the row it
// checked was not the row it meant to check.
static uint32_t g_ping_us[8];
static int      g_ping_n;
static int      g_ping_calls;                   // how many addresses were probed
static char     g_ping_last[FW_NET_ADDR_MAX];   // and the last one asked for

static void ping_add(int last_octet, int us) {
    if (g_ping_n >= 8) return;
    g_ping_hosts[g_ping_n] = (uint8_t)last_octet;
    g_ping_us[g_ping_n]    = (uint32_t)us;
    g_ping_n++;
}

static void ping_clear(void) { g_ping_n = 0; g_ping_calls = 0; g_ping_last[0] = 0; }

int fw_net_connected(void) { return g_net_up; }
int fw_net_ssid(char *out, unsigned cap) {
    return (int)snprintf(out, cap, "%s", g_net_up ? g_net_ssid_s : "");
}
int fw_net_ip(char *out, unsigned cap) {
    if (!g_net_up) { if (cap) out[0] = 0; return 0; }
    return (int)snprintf(out, cap, "%s", g_net_ip);
}
int fw_net_rssi(void) { return g_net_up ? -52 : 0; }

// Real ICMP on the device; here, a lookup on the last octet. Returns
// microseconds or -2 for "no reply in time", which is what an empty address
// gives on a real network too.
int fw_net_ping(const char *host, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!g_net_up || !host) return -1;
    const char *p = host, *last = host;
    for (; *p; p++) if (*p == '.') last = p + 1;
    int n = 0;
    for (p = last; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
    g_ping_calls++;
    snprintf(g_ping_last, sizeof(g_ping_last), "%s", host);
    for (int i = 0; i < g_ping_n; i++)
        if (g_ping_hosts[i] == (uint8_t)n) return (int)g_ping_us[i];
    return -2;
}

int fw_net_resolve(const char *host, char *ip_out, unsigned cap) {
    if (!g_net_up || !host) return -1;
    return (int)snprintf(ip_out, cap, "93.184.216.34");
}

// A settable fake clock, OFF by default so every existing test still sees
// fw_time_get FAIL — which is what "--:--", "clock not set" and the boot-time
// checks all lean on. The status-bar refresh test turns it on and moves it a
// minute, because a frozen clock while the panel was dim is the exact regression
// that test guards.
static bool     g_fake_time_ok = false;
static FwTime   g_fake_time;
int fw_time_get(struct FwTime *out) {
    if (!g_fake_time_ok) return 0;
    if (out) *out = g_fake_time;
    return 1;
}

// The screens that arrived with the settings, wireless, files and operations
// groups reach further into the ABI than the runner alone ever did.
int fw_unique_id(char *out, unsigned cap) { snprintf(out, cap, "E6614104037A5E2F"); return 1; }
uint32_t fw_clock_hz(void) { return 150000000u; }
int fw_net_scan(FwNetAp *out, unsigned max) { (void)out; (void)max; return 0; }
// --- a little music library -------------------------------------------------------
//
// PATH-KEYED, deliberately. Every directory answered zero before and the file
// browser's shot is a picture of that; a fake filesystem that answered
// everywhere would change a screen this has nothing to do with. Only the paths
// below have anything in them, so /nova/music has music and / is still empty.
struct FakeEntry {
    const char *dir;
    const char *name;
    int         is_dir;
    uint32_t    size;
    uint32_t    rate;           // 0 for anything that is not a WAV
    uint16_t    channels;
    const char *inam;           // the RIFF INFO title, when the file carries one
    const char *iart;
    // What a read of this file returns. Null for the music, which is
    // synthesised from a real RIFF header instead; set for anything whose
    // CONTENT is the point, which is every third-party app manifest.
    const char *body;
};

static const FakeEntry g_fake_fs[] = {
    { "/nova/music", "Ambient",         1,       0,     0, 0, nullptr, nullptr },
    { "/nova/music", "beacon.wav",      0,  882088, 22050, 1, nullptr, nullptr },
    // Not a WAV, so it must not appear in the media browser at all. It is here
    // precisely to prove the filter is doing something.
    { "/nova/music", "readme.txt",      0,     240,     0, 0, nullptr, nullptr },
    { "/nova/music", "signal-lost.wav", 0, 2822444, 44100, 2, nullptr, nullptr },
    // The one file with an INFO chunk, so the metadata path is exercised rather
    // than merely compiled.
    { "/nova/music/Ambient", "drift.wav", 0, 3528044, 44100, 2, "Drift", "Nova Labs" },
    { "/nova/music/Ambient", "halo.wav",  0, 1764044, 44100, 2, nullptr, nullptr },
    { "/nova/music/Ambient", "tide.wav",  0,  882044, 44100, 2, nullptr, nullptr },

    // --- apps somebody else wrote ---------------------------------------------
    //
    // Two, on purpose. One that works, so the runner has a real installed app to
    // open and novashots has a screen to photograph; and one written the way
    // somebody porting a MicroPython app would write it, so the panel that
    // explains the refusal is photographed too. A harness with only the working
    // case proves half of a feature whose other half is the error message.
    { "/nova/apps", "devcheck.napp", 0, 0, 0, 0, nullptr, nullptr,
      "app.name: Device Check\n"
      "app.ver: 1.0.0\n"
      "app.category: System\n"
      "app.desc: Ask a board that is behaving oddly.\n"
      "\n"
      "Tasks = ps\n"
      "Storage = df\n"
      "Memory = free\n"
      "Packages = pkg list\n" },
    { "/nova/apps", "dice.napp", 0, 0, 0, 0, nullptr, nullptr,
      "app.name: Dice\n"
      "app.kind: py\n"
      "app.category: Tools\n"
      "Roll = nothing this build can run\n" },
    // Not an app. It is here to prove the scan's filter does something rather
    // than merely compiling.
    { "/nova/apps", "readme.txt", 0, 0, 0, 0, nullptr, nullptr, "not an app" },
};
static const unsigned FAKE_FS_N = sizeof(g_fake_fs) / sizeof(g_fake_fs[0]);

static const FakeEntry *fake_find(const char *path) {
    for (unsigned i = 0; i < FAKE_FS_N; i++) {
        char full[160];
        snprintf(full, sizeof(full), "%s/%s", g_fake_fs[i].dir, g_fake_fs[i].name);
        if (!strcmp(full, path)) return &g_fake_fs[i];
    }
    return nullptr;
}

// Files with a BODY read back; everything else reads as empty, which is what
// this returned for every path before there was anything worth reading.
uint32_t fw_file_read(const char *path, void *buf, uint32_t cap) {
    const FakeEntry *e = fake_find(path);
    if (!e || !e->body) return 0;
    uint32_t n = (uint32_t)strlen(e->body);
    if (n > cap) n = cap;
    memcpy(buf, e->body, n);
    return n;
}

int fw_dir_count(const char *path) {
    int n = 0;
    for (unsigned i = 0; i < FAKE_FS_N; i++)
        if (!strcmp(g_fake_fs[i].dir, path)) n++;
    return n;
}

int fw_dir_entry(const char *path, unsigned index, FwDirEntry *out) {
    unsigned n = 0;
    for (unsigned i = 0; i < FAKE_FS_N; i++) {
        if (strcmp(g_fake_fs[i].dir, path)) continue;
        if (n++ != index) continue;
        snprintf(out->name, FW_NAME_MAX, "%s", g_fake_fs[i].name);
        out->is_dir = g_fake_fs[i].is_dir;
        out->size   = g_fake_fs[i].size;
        return 1;
    }
    return 0;
}

uint32_t fw_file_size(const char *path) {
    const FakeEntry *e = fake_find(path);
    if (!e) return 0;
    return e->body ? (uint32_t)strlen(e->body) : e->size;
}

// A REAL RIFF header, built byte for byte, because the media screens parse one
// and a hand-written stub of the right length would prove nothing about the
// parser. Chunks in the awkward order too — LIST before data — since that is
// the order the tag actually turns up in and the order a naive walk gets wrong.
static uint32_t fake_wav_header(const FakeEntry *e, uint8_t *b, uint32_t cap) {
    uint32_t n = 0;
    auto put   = [&](const char *s) { for (int i = 0; i < 4; i++) if (n < cap) b[n++] = (uint8_t)s[i]; };
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) if (n < cap) b[n++] = (uint8_t)(v >> (8 * i)); };
    auto put16 = [&](uint16_t v) { for (int i = 0; i < 2; i++) if (n < cap) b[n++] = (uint8_t)(v >> (8 * i)); };
    auto putstr = [&](const char *s) {
        uint32_t len = (uint32_t)strlen(s) + 1;
        if (len & 1) len++;                         // padded to an even length
        put32(len);
        for (uint32_t i = 0; i < len; i++) if (n < cap) b[n++] = (uint8_t)(i < strlen(s) ? s[i] : 0);
    };

    put("RIFF"); put32(e->size + 36); put("WAVE");
    put("fmt "); put32(16);
    put16(1);                                       // PCM
    put16(e->channels);
    put32(e->rate);
    put32(e->rate * e->channels * 2);               // byte rate
    put16((uint16_t)(e->channels * 2));             // block align
    put16(16);                                      // bits
    if (e->inam || e->iart) {
        uint32_t body = 4;
        if (e->inam) body += 8 + (uint32_t)((strlen(e->inam) + 2) & ~1u);
        if (e->iart) body += 8 + (uint32_t)((strlen(e->iart) + 2) & ~1u);
        put("LIST"); put32(body); put("INFO");
        if (e->inam) { put("INAM"); putstr(e->inam); }
        if (e->iart) { put("IART"); putstr(e->iart); }
    }
    put("data"); put32(e->size);
    return n;
}

uint32_t fw_file_read_at(const char *path, uint32_t off, void *buf, uint32_t cap) {
    const FakeEntry *e = fake_find(path);
    if (!e || !e->rate) return 0;
    uint8_t head[256];
    const uint32_t n = fake_wav_header(e, head, sizeof(head));
    if (off >= n) return 0;
    uint32_t take = n - off;
    if (take > cap) take = cap;
    memcpy(buf, head + off, take);
    return take;
}

// Two storage roots, so the file browser's roots screen renders both cases the
// device has: on-board flash always present, and an SD card that here is present
// with a capacity. A harness that returned flash alone would never draw the SD
// row. The music library above lives under /nova/music; the roots browse "/" and
// "/sd", which are empty here, which is the correct picture for the Files shot.
static bool g_fake_sd = true;
int fw_storage_roots(FwStorageRoot *out, int max) {
    if (!out || max < 1) return -1;
    memset(out, 0, sizeof(*out) * (unsigned)(max < 2 ? max : 2));
    snprintf(out[0].label, sizeof(out[0].label), "On-Board");
    snprintf(out[0].path, sizeof(out[0].path), "/");
    out[0].kind = FW_ROOT_FLASH; out[0].present = 1;
    out[0].total_kb = 2048; out[0].free_kb = 1788;
    if (max < 2) return 1;
    snprintf(out[1].label, sizeof(out[1].label), "SD");
    snprintf(out[1].path, sizeof(out[1].path), "/sd");
    out[1].kind = FW_ROOT_SD; out[1].present = g_fake_sd ? 1 : 0;
    out[1].total_kb = g_fake_sd ? 31000000u : 0;   // ~32 GB, exercises human_kb's G path
    out[1].free_kb  = g_fake_sd ? 29500000u : 0;
    return 2;
}
unsigned long fw_random(void) { return 4; }
void fw_random_bytes(void *buf, unsigned len) { for (unsigned i = 0; i < len; i++) ((unsigned char *)buf)[i] = 0; }
int fw_power_sleep(unsigned, int, int) { return 0; }
int fw_power_dormant(unsigned, int, int) { return 0; }
unsigned fw_power_min_sleep_ms(void) { return 1; }
// What is powering the board. UNKNOWN is the honest default for a harness with
// no radio and no battery divider, and it is the state the status bar has to
// render as "no reading" rather than as empty.
static int g_power_source = FW_POWER_UNKNOWN;
int fw_power_source(void) { return g_power_source; }

}  // extern "C"

#endif  // RPC_FAKEFW_D1_INC
