// System commands — uptime, date, sysinfo. The everyday v1 sys_sys.py set.

#include "command.h"
#include "kernel.h"
#include "storage.h"
#include "session.h"
#include "users.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "pico/stdlib.h"
#include "pico/aon_timer.h"

static int cmd_uptime(int, char **) {
    uint64_t s = time_us_64() / 1000000ull;
    unsigned d = (unsigned)(s / 86400); s %= 86400;
    unsigned h = (unsigned)(s / 3600);  s %= 3600;
    unsigned m = (unsigned)(s / 60);    unsigned sec = (unsigned)(s % 60);
    if (d) printf("up %ud %uh %um %us\n", d, h, m, sec);
    else   printf("up %uh %um %us\n", h, m, sec);
    return 0;
}

// date — read or set the always-on clock. The calendar API works the same on
// RP2040 (RTC) and RP2350 (powman AON timer), so this is portable.
static int cmd_date(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        // date set YYYY-MM-DD [HH:MM:SS]
        struct tm t; memset(&t, 0, sizeof(t));
        int Y, Mo, D, H = 0, Mi = 0, S = 0;
        if (sscanf(argv[2], "%d-%d-%d", &Y, &Mo, &D) != 3) {
            printf("usage: date set YYYY-MM-DD [HH:MM:SS]\n"); return 1;
        }
        if (argc >= 4) sscanf(argv[3], "%d:%d:%d", &H, &Mi, &S);
        t.tm_year = Y - 1900; t.tm_mon = Mo - 1; t.tm_mday = D;
        t.tm_hour = H; t.tm_min = Mi; t.tm_sec = S;
        if (!aon_timer_set_time_calendar(&t)) { printf("could not set clock\n"); return 1; }
        printf("clock set\n");
        return 0;
    }
    struct tm t;
    if (!aon_timer_get_time_calendar(&t)) { printf("clock not running\n"); return 1; }
    printf("%04d-%02d-%02d %02d:%02d:%02d\n",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return 0;
}

static int cmd_sysinfo(int, char **) {
    printf("RPCortex %s   (C++)\n", RPC_OS_VERSION);
    printf("  board  : %s\n", PICO_BOARD);
    printf("  user   : %s%s\n", session_user(),
           users_is_admin(session_user()) ? " (admin)" : "");
    printf("  accounts: %u\n", (unsigned)users_count());
    uint64_t s = time_us_64() / 1000000ull;
    printf("  uptime : %uh %um %us\n",
           (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
    printf("  heap   : %u / %u KB free\n",
           (unsigned)(heap_free() / 1024), (unsigned)(heap_total() / 1024));
    printf("  disk   : %u KB free\n", (unsigned)(storage_free_bytes() / 1024));
    struct tm t;
    if (aon_timer_get_time_calendar(&t))
        printf("  clock  : %04d-%02d-%02d %02d:%02d:%02d\n",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return 0;
}

void sys_register(void) {
    static const Command cmds[] = {
        {"uptime",  "time since boot",        cmd_uptime,  nullptr},
        {"date",    "date [set YYYY-MM-DD ..]", cmd_date,  nullptr},
        {"sysinfo", "system overview",        cmd_sysinfo, nullptr},
    };
    for (const auto &c : cmds) cmd_register(&c);
}
