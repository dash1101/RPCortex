// fetch / neofetch — the ASCII-art system summary.
//
// v1 shipped this as the `picofetch` package and it was the first thing anyone
// screenshotted. It is built in here rather than left to a package because it
// costs a couple of hundred lines, needs nothing that is not already loaded, and
// is most of what makes a fresh boot feel like something rather than a prompt.
//
// The logo is the same RPC mark the boot banner draws, in the same gradient, so
// the two look like they came from the same place.

#include "command.h"
#include "out.h"
#include "kernel.h"
#include "storage.h"
#include "session.h"
#include "registry.h"
#include "users.h"
#include "fmt.h"

#include <stdio.h>
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

static int cmd_fetch(int, char **) {
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

    add("OS",     "RPCortex %s %s", RPC_OS_VERSION, RPC_OS_CODENAME);
    add("Host",   "%s (%s)", PICO_BOARD, FETCH_ARCH);
    add("Kernel", "C++ native  -  GCC %s", __VERSION__);

    uint64_t s = time_us_64() / 1000000ull;
    unsigned h = (unsigned)(s / 3600), m = (unsigned)((s / 60) % 60);
    if (h) add("Uptime", "%uh %um", h, m);
    else   add("Uptime", "%um %us", m, (unsigned)(s % 60));

    add("Shell",  "rpcsh  -  %u commands", (unsigned)cmd_count());
    add("CPU",    "%s @ %u MHz", FETCH_CORE, (unsigned)(clock_get_hz(clk_sys) / 1000000));

    uint32_t total = heap_total(), free = heap_free();
    char b[56];
    bar(b, sizeof(b), total - free, total);
    add("Memory", "%u / %u KB  %s", (unsigned)((total - free) / 1024),
        (unsigned)(total / 1024), b);

    uint32_t dtotal = storage_total_bytes(), dfree = storage_free_bytes();
    bar(b, sizeof(b), dtotal - dfree, dtotal);
    add("Disk",   "%u / %u KB  %s", (unsigned)((dtotal - dfree) / 1024),
        (unsigned)(dtotal / 1024), b);

    if (net_is_connected()) add("Network", "%s", net_active_ssid());
    else                    add("Network", "offline");

    add("Users",  "%u account%s", (unsigned)users_count(), users_count() == 1 ? "" : "s");

    // --- draw ---------------------------------------------------------------
    // Art on the left, data on the right, both padded to a fixed column so the
    // block stays rectangular whichever side runs out of rows first.
    out_blank();

    uint32_t lines = ART_ROWS > n + 1 ? ART_ROWS : n + 1;
    for (uint32_t i = 0; i < lines; i++) {
        if (i < ART_ROWS) printf("  %s%-*s%s ", kGradient[i % 7], ART_WIDTH, kArt[i], C_RESET);
        else              printf("  %-*s ", ART_WIDTH, "");

        if (i == 0) {
            // The header line: user@host, then a rule as wide as it is.
            printf("%s%s%s@%s%s%s\n", C_CYAN, user, C_RESET, C_CYAN, host, C_RESET);
        } else if (i - 1 < n) {
            const Row &r = rows[i - 1];
            printf("%s%-8s%s %s\n", C_HEADER, r.key, C_RESET, r.val);
        } else {
            printf("\n");
        }
    }
    out_blank();
    return 0;
}

void fetch_register(void) {
    static const Command c{"fetch", "system summary with the logo", cmd_fetch, nullptr};
    cmd_register(&c);
    cmd_alias("neofetch", "fetch");
    cmd_alias("picofetch", "fetch");
}
