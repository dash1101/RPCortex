// tips: recommended actions for this device (core/advice.cpp decides them).
//
//   tips                  what is worth doing, most important first, with the command for each
//   tips dismiss <id>     stop showing one (per person: User.Tips_Dismissed)
//   tips reset            show every one again
//
// A one-line count is shown at login and at the top of `settings`, so nobody
// has to remember to ask.
#include "command.h"
#include "out.h"
#include "registry.h"
#include "users.h"
#include "storage.h"
#include "task.h"
#include "kernel.h"
#include "repoindex.h"
#include "imagefeatures.h"
#include "clockuse.h"
#include "advice.h"
#include "tips.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DISMISS_KEY "User.Tips_Dismissed"

static bool g_crashed_last_run;
void tips_note_crash(bool crashed) { g_crashed_last_run = crashed; }

static bool reg_is(const char *key, const char *dflt, const char *want) {
    return strcmp(reg_get(key, dflt), want) == 0;
}

static void gather(AdviceFacts *f) {
    memset(f, 0, sizeof(*f));
    uint64_t total = storage_total_bytes(), free_b = storage_free_bytes();
    f->storage_used_pct = total ? (unsigned)((total - free_b) * 100 / total) : 0;
    f->clock_set = reg_is("System.Clock_Set", "false", "true");
    f->has_wifi = RPC_HAS_WIFI;
    f->wifi_saved = reg_get("WiFi.Active", "")[0] != 0;
    char name[USR_NAME_MAX + 1];
    for (uint32_t i = 0; users_name_copy(i, name, sizeof(name)); i++)
        if (users_is_admin(name) && users_is_nopass(name)) f->admins_without_password++;
    f->crashed_last_run = g_crashed_last_run;
    // Remembered by `update check`; stale once this firmware has caught up.
    static char upd[24];
    snprintf(upd, sizeof(upd), "%s", reg_get("System.Update_Available", ""));
    if (upd[0] && repo_version_cmp(upd, RPC_OS_BUILDVER) <= 0) upd[0] = 0;
    f->update_available = upd;
    f->power = power_source();
    f->governor_off = reg_is("Power.Governor", "auto", "off");
    f->wifi_power_off = RPC_HAS_WIFI && reg_is("WiFi.Power", "auto", "off");
    f->idle_sleep_off = !task_idle_sleep_on();
    if (!reg_is("Hardware.Boot_Clock_On", "true", "false"))
        f->boot_clock_mhz = (unsigned)atoi(reg_get("Hardware.Boot_Clock", "0"));
}

unsigned tips_count(void) {
    AdviceFacts f;
    gather(&f);
    Advice a[ADVICE_MAX];
    return advice_build(&f, reg_get(DISMISS_KEY, ""), a);
}

void tips_login_line(void) {
    unsigned n = tips_count();
    if (n) out_info("%u recommended action%s: 'tips' lists %s.", n, n == 1 ? "" : "s", n == 1 ? "it" : "them");
}

static int cmd_tips(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "dismiss")) {
        if (argc < 3 || !advice_known(argv[2])) {
            out_err("Usage: tips dismiss <id>  (the id is shown in brackets by 'tips')");
            return 1;
        }
        char list[160];
        snprintf(list, sizeof(list), "%s", reg_get(DISMISS_KEY, ""));
        if (!strstr(list, argv[2])) {
            size_t n = strlen(list);
            snprintf(list + n, sizeof(list) - n, "%s%s", n ? "," : "", argv[2]);
            reg_set(DISMISS_KEY, list);
        }
        out_ok("'%s' will not be shown again. 'tips reset' brings it back.", argv[2]);
        return 0;
    }
    if (!strcmp(sub, "reset")) {
        reg_set(DISMISS_KEY, "");
        out_ok("Every recommendation will be shown again.");
        return 0;
    }
    if (sub[0]) { out_multi("Usage: tips [dismiss <id> | reset]"); return 1; }

    AdviceFacts f;
    gather(&f);
    Advice a[ADVICE_MAX];
    unsigned n = advice_build(&f, reg_get(DISMISS_KEY, ""), a);
    if (!n) { out_ok("Nothing to recommend. This device is in good shape."); return 0; }
    out_info("Recommended actions");
    for (unsigned i = 0; i < n; i++) {
        const char *tag = a[i].level == ADV_IMPORTANT ? C_WARN "important" C_RESET
                        : a[i].level == ADV_SUGGEST   ? "suggested" : "note";
        out_multi("  %s [%s]  %s", tag, a[i].id, a[i].text);
        if (a[i].action[0]) out_multi("      run: %s%s%s", C_CYAN, a[i].action, C_RESET);
    }
    out_multi("  'tips dismiss <id>' hides one.");
    return 0;
}

void tips_register(void) {
    static const Command c{"tips", "recommended actions for this device", cmd_tips, nullptr, LEVEL_USER};
    cmd_register(&c);
}
