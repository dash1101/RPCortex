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
static int      g_i2c_writes;

extern "C" {

uint32_t fw_millis(void) { return g_ms; }
uint32_t fw_micros(void) { return g_ms * 1000u; }
void     fw_task_sleep_ms(uint32_t ms) { g_ms += ms; }
void     fw_task_yield(void) {}
int      fw_task_should_stop(void) { return 0; }
int      fw_task_self(void) { return 1; }
uint32_t fw_cores(void) { return 2; }
uint32_t fw_core_id(void) { return 0; }
uint32_t fw_cpu_percent(void) { return 7; }
void     fw_reboot(void) {}

// Spawning is COUNTED rather than performed. A second runner is what this
// harness is here to catch, and counting the attempt is how it catches it
// without needing threads.
int fw_task_spawn(const char *, int (*)(void *), void *, uint32_t) {
    return ++g_spawned + 10;
}
int fw_task_kill(int) { return 1; }

int fw_printf(const char *, ...) { return 0; }
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
static const char *fake_shell(const char *line) {
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
    if (!strncmp(line, "bt ", 3))
        return "[:] 3 devices\n"
               "  D9:33:36:32:1B:59  LE    -78 dBm  Govee_H6199_1B59\n"
               "  37:96:7B:89:C2:EB  LE    -80 dBm  \n"
               "  53:0A:FB:D0:FF:87  LE    -92 dBm  Quest 3S\n";
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
    return "";
}

int fw_shell_run(const char *line, char *out, uint32_t cap) {
    if (!out || !cap) return 0;
    snprintf(out, cap, "%s", line ? fake_shell(line) : "");
    return 0;
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
int      fw_file_write(const char *, const void *, uint32_t) { return 1; }
uint32_t fw_file_read(const char *, void *, uint32_t) { return 0; }
int      fw_file_remove(const char *) { return 1; }

int fw_board(char *out, unsigned cap) { snprintf(out, cap, "pico2_w"); return 1; }
unsigned fw_gpio_count(void) { return 30; }
int fw_gpio_usable(unsigned p) { return (p < 30 && p != 23 && p != 24 && p != 25 && p != 29) ? 1 : 0; }
int fw_gpio_init(unsigned, int) { return 0; }
int fw_gpio_pull(unsigned, int) { return 0; }
int fw_gpio_put(unsigned, int) { return 0; }
int fw_gpio_get(unsigned) { return 1; }
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

int fw_net_connected(void) { return 0; }
int fw_net_ssid(char *out, unsigned cap) { if (cap) out[0] = 0; return 0; }
int fw_net_ip(char *out, unsigned cap) { if (cap) out[0] = 0; return 0; }
int fw_net_rssi(void) { return 0; }

int fw_time_get(struct FwTime *out) { (void)out; return 0; }

// The screens that arrived with the settings, wireless, files and operations
// groups reach further into the ABI than the runner alone ever did.
int fw_unique_id(char *out, unsigned cap) { snprintf(out, cap, "E6614104037A5E2F"); return 1; }
uint32_t fw_clock_hz(void) { return 150000000u; }
int fw_net_scan(FwNetAp *out, unsigned max) { (void)out; (void)max; return 0; }
int fw_dir_count(const char *) { return 0; }
int fw_dir_entry(const char *, unsigned, FwDirEntry *) { return 0; }
uint32_t fw_file_size(const char *) { return 0; }
uint32_t fw_file_read_at(const char *, uint32_t, void *, uint32_t) { return 0; }
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
