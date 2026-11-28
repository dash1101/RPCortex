#include "advice.h"

#include <stdio.h>
#include <string.h>

static const char *const kIds[] = {
    "nopass", "crash", "storage", "update", "clock", "wifi", "governor",
    "wifipower", "idle", "bootclock",
};

bool advice_known(const char *id) {
    if (!id) return false;
    for (const char *k : kIds) if (!strcmp(k, id)) return true;
    return false;
}

static bool dismissed_has(const char *list, const char *id) {
    if (!list) return false;
    size_t n = strlen(id);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, id, n)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

struct Builder {
    Advice *out;
    unsigned n;
    const char *dismissed;
    void add(const char *id, AdviceLevel lvl, const char *action, const char *fmt, ...)
        __attribute__((format(printf, 5, 6)));
};

#include <stdarg.h>
void Builder::add(const char *id, AdviceLevel lvl, const char *action, const char *fmt, ...) {
    if (n >= ADVICE_MAX || dismissed_has(dismissed, id)) return;
    Advice &a = out[n++];
    a.id = id;
    a.level = lvl;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a.text, sizeof(a.text), fmt, ap);
    va_end(ap);
    snprintf(a.action, sizeof(a.action), "%s", action ? action : "");
}

unsigned advice_build(const AdviceFacts *f, const char *dismissed, Advice *out) {
    Builder b{out, 0, dismissed};

    // Important: about safety, or about something that already went wrong.
    if (f->admins_without_password)
        b.add("nopass", ADV_IMPORTANT, "passwd",
              f->admins_without_password == 1 ? "An admin account has no password."
                                              : "%u admin accounts have no password.",
              f->admins_without_password);
    if (f->crashed_last_run)
        b.add("crash", ADV_IMPORTANT, "logdump", "The last run ended in a crash or a hang.");
    if (f->storage_used_pct >= 90)
        b.add("storage", ADV_IMPORTANT, "pkg list",
              "Storage is %u%% full; writes stop at 98%%. Remove packages or files you do not need.",
              f->storage_used_pct);

    // Suggested: things that make the device work better.
    if (f->update_available && f->update_available[0])
        b.add("update", ADV_SUGGEST, "sudo update install", "Firmware %s is available.",
              f->update_available);
    if (f->storage_used_pct >= 75 && f->storage_used_pct < 90)
        b.add("storage", ADV_SUGGEST, "pkg list", "Storage is %u%% full.", f->storage_used_pct);
    if (f->has_wifi && !f->wifi_saved)
        b.add("wifi", ADV_SUGGEST, "wifi connect",
              "Join a Wi-Fi network for updates, packages and the time.");
    if (!f->clock_set)
        b.add("clock", ADV_SUGGEST, f->has_wifi && f->wifi_saved ? "ntp sync" : "date set",
              "The date is not set, so file times and logs say 1970.");
    if (f->power == 0 && f->governor_off)
        b.add("governor", ADV_SUGGEST, "sudo power governor auto",
              "On battery with the clock governor off: auto lowers the clock while idle.");
    if (f->power == 0 && f->wifi_power_off)
        b.add("wifipower", ADV_SUGGEST, "wifi power auto",
              "On battery with Wi-Fi power saving off: auto saves power on battery.");
    if (f->idle_sleep_off)
        b.add("idle", ADV_SUGGEST, "sudo power idle on",
              "Idle sleep is off, so idle cores spin at full clock.");

    // For information.
    if (f->boot_clock_mhz > 250)
        b.add("bootclock", ADV_INFO, "pulse boot off",
              "The boot clock is %u MHz, above the tested 250 MHz.", f->boot_clock_mhz);
    return b.n;
}
