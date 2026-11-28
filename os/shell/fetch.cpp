// RPC Fetch (fetch, rpcfetch, neofetch, picofetch): the ASCII-art system summary.
//
//   fetch                 once
//   fetch -w [secs]       redrawn in place every few seconds (2 by default) until a key
//   fetch <colour>        another accent: cyan blue magenta green yellow red white
//
// v1 shipped this as the `picofetch` package and it was the first thing anyone
// screenshotted. It is built in here rather than left to a package because it
// costs a couple of hundred lines, needs nothing that is not already loaded, and
// is most of what makes a fresh boot feel like something rather than a prompt.
//
// The logo is the same RPC mark the boot banner draws, in the same gradient, so
// the two look like they came from the same place.

#include "clockuse.h"
#include "command.h"
#include "out.h"
#include "kernel.h"
#include "storage.h"
#include "session.h"
#include "registry.h"
#include "users.h"
#include "fmt.h"
#include "task.h"
#include "interrupt.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

// Through the output layer, not printf, so a redirect, a pipe or a remote
// login gets the picture rather than the serial port.
static void put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void put(const char *fmt, ...) {
    char line[200];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) out_write(line, (uint32_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#if PICO_RP2040
  #define FETCH_ARCH "RP2040"
  #define FETCH_CORE "2x Cortex-M0+"
#else
  #define FETCH_ARCH "RP2350"
  #define FETCH_CORE "2x Cortex-M33"
#endif

bool net_is_connected(void);
const char *net_active_ssid(void);

// A compact RPC mark. The boot banner's logo is 39 columns wide, which leaves
// no room for a data column beside it on an 80-column terminal — this one is 29,
// so the whole block lands at 78 and still fits. Same charset and same seven
// rows, so it reads as the same mark rather than a different one.
static const char *kArt[] = {
    ":::::::   :::::::   ::::::",
    ":+:   :+: :+:   :+: :+:",
    "+:+   +:+ +:+   +:+ +:+",
    "+#++:++:  +#++:++:  +#+",
    "+#+  +#+  +#+       +#+",
    "#+#   #+# #+#       #+#   #+#",
    "###    ## ###        ########",
};
#define ART_ROWS  (sizeof(kArt) / sizeof(kArt[0]))
#define ART_WIDTH 30

// The boot banner's gradient, so the two marks match.
static const char *kGradient[] = {
    "\033[96m", "\033[36m", "\033[36m", "\033[94m",
    "\033[34m", "\033[95m", "\033[35m",
};

struct Row { const char *key; char val[96]; };

static void bar(char *out, uint32_t cap, uint32_t used, uint32_t total) {
    // Ten cells, filled proportionally. Blocks rather than '#' because the
    // terminal this is read on is the same one that draws the boot gradient.
    const int cells = 10;
    int filled = total ? (int)((uint64_t)used * cells / total) : 0;
    if (filled > cells) filled = cells;
    int n = snprintf(out, cap, "[");
    for (int i = 0; i < cells && n < (int)cap - 8; i++)
        n += snprintf(out + n, cap - n, "%s", i < filled ? "█" : "░");
    snprintf(out + n, cap - n, "] %u%%",
             (unsigned)(total ? (uint64_t)used * 100 / total : 0));
}

// An accent replaces the gradient with one colour pair: bright for the top of
// the mark and the labels, plain for the bottom. v1's `fetch <colour>`.
struct Accent { const char *name; const char *hi; const char *lo; };
static const Accent kAccents[] = {
    {"cyan", "\033[96m", "\033[36m"},   {"blue", "\033[94m", "\033[34m"},
    {"magenta", "\033[95m", "\033[35m"}, {"green", "\033[92m", "\033[32m"},
    {"yellow", "\033[93m", "\033[33m"},  {"red", "\033[91m", "\033[31m"},
    {"white", "\033[97m", "\033[37m"},
};

static void draw_fetch(const Accent *ac, uint32_t every);

static int cmd_fetch(int argc, char **argv) {
    const Accent *ac = nullptr;
    uint32_t every = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || !strcmp(argv[i], "help")) {
            out_multi("RPC Fetch: what this device is and how it is doing.");
            out_multi("  fetch                once");
            out_multi("  fetch -w [secs]      redrawn every few seconds (2 by default) until a key");
            out_multi("  fetch <colour>       cyan blue magenta green yellow red white");
            return 0;
        }
        if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--watch") || !strcmp(argv[i], "watch")) {
            every = 2;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                every = (uint32_t)atoi(argv[++i]);
                if (every < 1) every = 1;
                if (every > 3600) every = 3600;
            }
            continue;
        }
        bool found = false;
        for (const Accent &a : kAccents) if (!strcmp(argv[i], a.name)) { ac = &a; found = true; }
        if (!found) { out_warn("fetch: '%s' is not an option or a colour. 'fetch -h' lists them.", argv[i]); return 1; }
    }
    if (!every) { draw_fetch(ac, 0); return 0; }

    // Watch: redrawn in place, until Ctrl+C or any key.
    intr_clear();
    while (true) {
        put("\033[2J\033[H");
        draw_fetch(ac, every);
        bool stop = false;
        for (uint32_t i = 0; i < every * 10 && !stop; i++) {
            task_sleep_ms(100);
            if (intr_check() || intr_getkey() >= 0) stop = true;
        }
        if (stop) break;
    }
    intr_clear();
    return 0;
}

static void draw_fetch(const Accent *ac, uint32_t every) {
    Row rows[16];
    uint32_t n = 0;

    auto add = [&](const char *key, const char *fmt, auto... args) {
        if (n >= 16) return;
        rows[n].key = key;
        snprintf(rows[n].val, sizeof(rows[n].val), fmt, args...);
        n++;
    };

    const char *user = session_user();
    const char *host = reg_get("System.Device_ID", "vela");

    add("OS",     "RPCortex %s %s", RPC_OS_LONG, RPC_OS_CODENAME);
    add("Host",   "%s (%s)", PICO_BOARD, FETCH_ARCH);
    add("Kernel", "C++ native  -  GCC %s", __VERSION__);

    uint64_t s = time_us_64() / 1000000ull;
    unsigned h = (unsigned)(s / 3600), m = (unsigned)((s / 60) % 60);
    if (h) add("Uptime", "%uh %um", h, m);
    else   add("Uptime", "%um %us", m, (unsigned)(s % 60));

    add("Shell",  "rpcsh  -  %u commands", (unsigned)cmd_count());
    add("CPU",    "%s @ %u MHz  (%u core%s in use)", FETCH_CORE,
        (unsigned)(clock_get_hz(clk_sys) / 1000000),
        (unsigned)task_core_count(), task_core_count() == 1 ? "" : "s");
    add("Tasks",  "%u running", (unsigned)task_count());

    uint32_t total = heap_total(), free = heap_free();
    char b[56];
    bar(b, sizeof(b), total - free, total);
    add("Memory", "%u / %u KB  %s", (unsigned)((total - free) / 1024),
        (unsigned)(total / 1024), b);

    uint32_t dtotal = storage_total_bytes(), dfree = storage_free_bytes();
    bar(b, sizeof(b), dtotal - dfree, dtotal);
    add("Disk",   "%u / %u KB  %s", (unsigned)((dtotal - dfree) / 1024),
        (unsigned)(dtotal / 1024), b);

    {
        int src = power_source();
        GovStatus g;
        governor_status(&g);
        const char *what = src == 1 ? "USB" : src == 0 ? "battery" : "not known yet";
        if (g.cur_mhz < g.base_mhz) add("Power", "%s  (idle: clock at %u of %u MHz)", what, g.cur_mhz, g.base_mhz);
        else                        add("Power", "%s", what);
    }
    if (net_is_connected()) add("Network", "%s", net_active_ssid());
    else                    add("Network", "offline");

    add("Users",  "%u account%s", (unsigned)users_count(), users_count() == 1 ? "" : "s");

    // --- draw ---------------------------------------------------------------
    // Art on the left, data on the right, both padded to a fixed column so the
    // block stays rectangular whichever side runs out of rows first.
    out_blank();

    uint32_t lines = ART_ROWS > n + 1 ? ART_ROWS : n + 1;
    for (uint32_t i = 0; i < lines; i++) {
        const char *col = ac ? (i < 4 ? ac->hi : ac->lo) : kGradient[i % 7];
        if (i < ART_ROWS) put("  %s%-*s%s ", col, ART_WIDTH, kArt[i], C_RESET);
        else              put("  %-*s ", ART_WIDTH, "");

        if (i == 0) {
            // The header line: user@host, then a rule as wide as it is.
            const char *hc = ac ? ac->hi : C_CYAN;
            put("%s%s%s@%s%s%s\n", hc, user, C_RESET, hc, host, C_RESET);
        } else if (i - 1 < n) {
            const Row &r = rows[i - 1];
            put("%s%-8s%s %s\n", ac ? ac->hi : C_HEADER, r.key, C_RESET, r.val);
        } else {
            put("\n");
        }
    }
    if (every) put("  %sRPC Fetch, every %us. Any key stops it.%s\n", C_GRAY, (unsigned)every, C_RESET);
    out_blank();
}

void fetch_register(void) {
    static const Command c{"fetch", "RPC Fetch: system summary with the logo", cmd_fetch, nullptr};
    cmd_register(&c);
    cmd_alias("rpcfetch", "fetch");
    cmd_alias("neofetch", "fetch");
    cmd_alias("picofetch", "fetch");
}
