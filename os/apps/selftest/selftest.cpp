// selftest — check the things a release has to get right, on the board itself,
// and print readings that can be compared from one build to the next.
//
//   selftest          every check; the last line says how many passed
//   selftest quick    the same without the network
//
// Each check prints one line, PASS or FAIL, with what it measured:
//
//   selftest  fs         PASS  write 16 KB 210 ms, read 31 ms, sha256 matched
//
// A check that cannot run here (no network on this board) says SKIP, and is not
// counted either way: a skip that counted as a pass is how a broken check
// ships. tools/pretest.py runs this on every attached board and keeps the lines.
//
// Everything it writes is under /tmp and removed again.

#include "rpc_app.h"
#include <stdio.h>

RPC_APP_VER("selftest", "1.0");

static int g_pass, g_fail, g_skip;
static char g_line[160];
static uint8_t g_buf[4096];          // static: a package gets 12 KB of fw_malloc per call
static uint8_t g_back[4096];

// The reading is in g_line already (snprintf: the ABI has no va_list form).
static void report(const char *name, int ok) {
    const char *word = ok > 0 ? "PASS" : ok == 0 ? "FAIL" : "SKIP";
    if (ok > 0) g_pass++; else if (ok == 0) g_fail++; else g_skip++;
    fw_printf("selftest  %-9s %s  %s\n", name, word, g_line);
}

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int s_has(const char *hay, const char *needle) {
    int n = s_len(needle);
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (j < n && hay[i + j] == needle[j]) j++;
        if (j == n) return 1;
    }
    return 0;
}
static int s_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// --- the checks ----------------------------------------------------------------

static void check_info(void) {
    char board[24] = "?", id[24] = "?";
    fw_board(board, sizeof(board));
    fw_unique_id(id, sizeof(id));
    snprintf(g_line, sizeof(g_line), "%s, %u core(s) at %u MHz, id %s", board, (unsigned)fw_cores(), (unsigned)(fw_clock_hz() / 1000000u), id);
    report("board", 1);
}

static void check_memory(void) {
    uint32_t total = fw_heap_total(), free_ = fw_heap_free(), big = fw_heap_largest();
    // The sandbox's own block: take most of it, fill, read back, give it back.
    uint8_t *p = (uint8_t *)fw_malloc(8192);
    int ok = p != nullptr;
    if (p) {
        for (int i = 0; i < 8192; i++) p[i] = (uint8_t)(i * 31 + 7);
        for (int i = 0; i < 8192 && ok; i++) ok = p[i] == (uint8_t)(i * 31 + 7);
        fw_free(p);
    }
    snprintf(g_line, sizeof(g_line), "heap %u KB free of %u, largest block %u KB; 8 KB package block %s", (unsigned)(free_ / 1024), (unsigned)(total / 1024), (unsigned)(big / 1024), ok ? "held its pattern" : "FAILED");
    report("memory", ok);
}

static void check_fs(void) {
    const char *path = "/tmp/selftest.bin";
    fw_file_remove(path);
    unsigned char want[32], got[32];
    // 16 KB in four appends, hashed as written.
    for (unsigned i = 0; i < sizeof(g_buf); i++) g_buf[i] = (uint8_t)(i * 13 + 5);
    uint32_t t0 = fw_millis();
    int ok = 1;
    for (int k = 0; k < 4 && ok; k++) {
        g_buf[0] = (uint8_t)k;                       // each piece different
        ok = fw_file_append(path, g_buf, sizeof(g_buf)) >= 0;
    }
    uint32_t t_write = fw_millis() - t0;
    if (!ok) {
        snprintf(g_line, sizeof(g_line), "could not write %s", path);
        report("fs", 0);
        fw_file_remove(path);
        return;
    }
    ok = fw_file_size(path) == 4 * sizeof(g_buf);
    // Read back piece by piece and compare.
    t0 = fw_millis();
    for (int k = 0; k < 4 && ok; k++) {
        g_buf[0] = (uint8_t)k;
        uint32_t n = fw_file_read_at(path, k * sizeof(g_back), g_back, sizeof(g_back));
        fw_sha256(g_buf, sizeof(g_buf), want);
        fw_sha256(g_back, n, got);
        for (int i = 0; i < 32 && ok; i++) ok = want[i] == got[i];
    }
    uint32_t t_read = fw_millis() - t0;
    int removed = fw_file_remove(path) >= 0 && !fw_file_exists(path);
    snprintf(g_line, sizeof(g_line), "write 16 KB %u ms, read back %u ms, sha256 %s, removed %s", (unsigned)t_write, (unsigned)t_read, ok ? "matched" : "DIFFERED", removed ? "yes" : "NO");
    report("fs", ok && removed);
}

static void check_sha(void) {
    // FIPS 180-2's "abc", then a speed reading.
    static const unsigned char abc_want[4] = {0xba, 0x78, 0x16, 0xbf};
    unsigned char d[32];
    fw_sha256("abc", 3, d);
    int ok = d[0] == abc_want[0] && d[1] == abc_want[1] && d[2] == abc_want[2] && d[3] == abc_want[3];
    uint32_t t0 = fw_micros();
    for (int i = 0; i < 64; i++) fw_sha256(g_buf, sizeof(g_buf), d);
    uint32_t us = fw_micros() - t0;
    snprintf(g_line, sizeof(g_line), "known answer %s, %u KB/s", ok ? "right" : "WRONG", us ? (unsigned)(256u * 1000000u / us) : 0u);
    report("sha256", ok);
}

static void check_timers(void) {
    uint32_t m0 = fw_millis(), u0 = fw_micros();
    fw_task_sleep_ms(100);
    uint32_t dm = fw_millis() - m0, du = fw_micros() - u0;
    int ok = dm >= 98 && dm <= 150 && du >= 98000 && du <= 150000;
    snprintf(g_line, sizeof(g_line), "a 100 ms sleep took %u ms (%u us)", (unsigned)dm, (unsigned)du);
    report("timers", ok);
}

static volatile int g_task_ran;
static int task_fn(void *) { g_task_ran = 1; return 0; }

static void check_tasks(void) {
    g_task_ran = 0;
    int pid = fw_task_spawn("selftest-t", task_fn, nullptr, 2048);
    for (int i = 0; i < 100 && !g_task_ran; i++) fw_task_sleep_ms(10);
    snprintf(g_line, sizeof(g_line), "spawned pid %d, %s", pid, g_task_ran ? "it ran" : "it NEVER ran");
    report("tasks", pid > 0 && g_task_ran);
}

static void check_shell(void) {
    static char out[512];
    int rc = fw_shell_run("echo selftest-echo", out, sizeof(out));
    int ok = rc == 0 && s_has(out, "selftest-echo");
    int rc2 = fw_shell_run("ver", out, sizeof(out));
    int ok2 = rc2 == 0 && s_has(out, "RPCortex");
    snprintf(g_line, sizeof(g_line), "echo %s, ver %s", ok ? "ok" : "FAILED", ok2 ? "ok" : "FAILED");
    report("shell", ok && ok2);
}

static void check_registry(void) {
    char v[64] = "";
    // Settings.Version is written at every boot to the running firmware's
    // version, so it is always there and always says this build.
    int found = fw_reg_get("Settings.Version", v, sizeof(v));
    static char ver[256];
    int rc = fw_shell_run("ver", ver, sizeof(ver));
    int same = found && rc == 0 && v[0] == 'v' && s_has(ver, v);
    snprintf(g_line, sizeof(g_line), "Settings.Version = %s%s", found ? v : "(missing)",
             same ? ", the running build" : ", NOT the running build");
    report("registry", same);
}

static void check_random(void) {
    unsigned char a[16], b[16];
    fw_random_bytes(a, sizeof(a));
    fw_random_bytes(b, sizeof(b));
    int same = 1, flat = 1;
    for (int i = 0; i < 16; i++) { if (a[i] != b[i]) same = 0; if (a[i] != a[0]) flat = 0; }
    snprintf(g_line, sizeof(g_line), "two draws %s", same ? "were IDENTICAL" : "differ");
    report("random", !same && !flat);
}

static int fetch(const char *url, int *status, char *detail, unsigned dcap) {
    static FwHttpRequest r;
    for (unsigned i = 0; i < sizeof(r); i++) ((char *)&r)[i] = 0;
    r.size = sizeof(r);
    r.url = url;
    r.path = "/tmp/selftest.http";
    r.max_bytes = 2048;
    int n = fw_http_request(&r);
    *status = r.status;
    unsigned k = 0;
    for (; k + 1 < dcap && r.detail[k]; k++) detail[k] = r.detail[k];
    detail[k] = 0;
    return n;
}

static void check_network(void) {
    char ip[48] = "";
    int via_wifi = fw_net_connected();
    if (via_wifi) {
        int r = fw_net_resolve("raw.githubusercontent.com", ip, sizeof(ip));
        snprintf(g_line, sizeof(g_line), "raw.githubusercontent.com is %s", r >= 0 ? ip : "UNRESOLVED");
        report("dns", r >= 0);
    }
    // A real file over HTTPS: on Wi-Fi the board checks the certificate, through
    // the bridge the computer does.
    int status = 0; char detail[96];
    uint32_t t0 = fw_millis();
    int n = fetch("https://raw.githubusercontent.com/dash1101/RPCortex/main/releases/latest.json",
                  &status, detail, sizeof(detail));
    uint32_t ms = fw_millis() - t0;
    if (n < 0 && !via_wifi && s_has(detail, "no network")) {
        snprintf(g_line, sizeof(g_line), "no network here (no Wi-Fi, no bridge)");
        report("https", -1);
        return;
    }
    int got = n > 0 ? (int)fw_file_read("/tmp/selftest.http", g_back, sizeof(g_back) - 1) : 0;
    g_back[got > 0 ? got : 0] = 0;
    int ok = n > 0 && status == 200 && s_has((const char *)g_back, "\"packages\"");
    snprintf(g_line, sizeof(g_line), "latest.json: HTTP %d, %d bytes in %u ms via %s%s%s", status, n, (unsigned)ms, via_wifi ? "Wi-Fi" : "the bridge", ok ? "" : " - ", ok ? "" : detail);
    report("https", ok);
    fw_file_remove("/tmp/selftest.http");
    // And a certificate nobody should trust has to be refused.
    n = fetch("https://self-signed.badssl.com/", &status, detail, sizeof(detail));
    snprintf(g_line, sizeof(g_line), "self-signed.badssl.com %s", n < 0 ? "refused, as it must be" : "was ACCEPTED");
    report("tls", n < 0);
    fw_file_remove("/tmp/selftest.http");
}

// --- the command -------------------------------------------------------------------

static int cmd_selftest(int argc, char **argv) {
    int quick = argc >= 2 && s_eq(argv[1], "quick");
    g_pass = g_fail = g_skip = 0;
    uint32_t t0 = fw_millis();
    check_info();
    check_memory();     if (fw_task_should_stop()) return 1;
    check_fs();         if (fw_task_should_stop()) return 1;
    check_sha();
    check_timers();
    check_tasks();
    check_shell();
    check_registry();
    check_random();
    if (!quick) check_network();
    fw_printf("selftest: %d passed, %d failed, %d skipped, in %u ms\n", g_pass, g_fail, g_skip,
              (unsigned)(fw_millis() - t0));
    return g_fail ? 1 : 0;
}

extern "C" int app_main(int) {
    rpc_register_command("selftest", "check this board and print readings (selftest [quick])", cmd_selftest);
    return 0;
}
