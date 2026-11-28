// Recommended actions: things worth doing about this device, worked out from a
// handful of facts about it, each with the command that does it.
//
// Pure, so it host-tests; shell/tips.cpp gathers the facts and shows the list
// (`tips`, and a one-line count at login and in `settings`). Each item has a
// short stable id, so one that does not apply to someone can be dismissed
// (`tips dismiss <id>`) and stays dismissed.
#ifndef RPC_ADVICE_H
#define RPC_ADVICE_H

#include <stdint.h>

struct AdviceFacts {
    unsigned storage_used_pct;    // the internal filesystem
    bool     clock_set;           // the date has been set (ntp, date set, GPS)
    bool     has_wifi;            // this image has Wi-Fi
    bool     wifi_saved;          // a network has been joined before
    unsigned admins_without_password;
    bool     crashed_last_run;    // the last run ended in a reset nobody asked for
    const char *update_available; // a newer release seen by `update check`, or ""
    int      power;               // 1 USB, 0 battery, -1 not known
    bool     governor_off;
    bool     wifi_power_off;      // Wi-Fi power saving switched off by hand
    bool     idle_sleep_off;
    unsigned boot_clock_mhz;      // 0 when none is set
};

enum AdviceLevel { ADV_INFO, ADV_SUGGEST, ADV_IMPORTANT };

struct Advice {
    const char *id;               // stable, for dismissing
    AdviceLevel level;
    char text[96];
    char action[64];              // the command, or "" when there is none to give
};

#define ADVICE_MAX 12

// Fills `out` (room for ADVICE_MAX), most important first, leaving out any id
// in `dismissed` (a comma-separated list, may be null). Returns how many.
unsigned advice_build(const AdviceFacts *f, const char *dismissed, Advice *out);

// Is `id` one advice_build can give? For refusing a typo in `tips dismiss`.
bool advice_known(const char *id);

#endif
