// core/advice: which recommendations a set of facts gives, in what order, and
// that dismissing one sticks without hiding the others.
#include "../core/advice.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", what); }
}

static AdviceFacts healthy(void) {
    AdviceFacts f;
    memset(&f, 0, sizeof(f));
    f.storage_used_pct = 40;
    f.clock_set = true;
    f.has_wifi = true;
    f.wifi_saved = true;
    f.update_available = "";
    f.power = 1;
    return f;
}

static bool has(const Advice *a, unsigned n, const char *id) {
    for (unsigned i = 0; i < n; i++) if (!strcmp(a[i].id, id)) return true;
    return false;
}

int main(void) {
    Advice a[ADVICE_MAX];
    AdviceFacts f = healthy();
    ck(advice_build(&f, nullptr, a) == 0, "a healthy device: nothing to say");

    f = healthy(); f.admins_without_password = 2; f.crashed_last_run = true;
    f.update_available = "2.0.800"; f.clock_set = false;
    unsigned n = advice_build(&f, nullptr, a);
    ck(n == 4, "four things wrong: four items");
    ck(!strcmp(a[0].id, "nopass") && a[0].level == ADV_IMPORTANT, "no password comes first");
    ck(strstr(a[0].text, "2 admin accounts") != nullptr, "and says how many");
    ck(!strcmp(a[1].id, "crash"), "a crash second");
    ck(!strcmp(a[2].id, "update") && strstr(a[2].text, "2.0.800") && !strcmp(a[2].action, "sudo update install"),
       "the update, naming it, with its command");
    ck(!strcmp(a[3].id, "clock") && !strcmp(a[3].action, "ntp sync"), "the clock, by NTP when Wi-Fi is set up");

    // Dismissing.
    n = advice_build(&f, "crash,clock", a);
    ck(n == 2 && !has(a, n, "crash") && !has(a, n, "clock") && has(a, n, "nopass"), "dismissed ids are left out, the rest stay");
    n = advice_build(&f, "cras,lock", a);
    ck(n == 4, "a dismissal matches the whole id, not a prefix or a suffix");
    n = advice_build(&f, ",,update,", a);
    ck(n == 3 && !has(a, n, "update"), "stray commas are harmless");

    // Storage: one id, two levels.
    f = healthy(); f.storage_used_pct = 80;
    n = advice_build(&f, nullptr, a);
    ck(n == 1 && !strcmp(a[0].id, "storage") && a[0].level == ADV_SUGGEST, "80% full: a suggestion");
    f.storage_used_pct = 95;
    n = advice_build(&f, nullptr, a);
    ck(n == 1 && a[0].level == ADV_IMPORTANT && strstr(a[0].text, "98%"), "95% full: important, and says where writes stop");

    // Power: only on battery, and only for what is switched off.
    f = healthy(); f.governor_off = true; f.wifi_power_off = true;
    ck(advice_build(&f, nullptr, a) == 0, "plugged in: power settings are nobody's business");
    f.power = -1;
    ck(advice_build(&f, nullptr, a) == 0, "power not known: no power advice either");
    f.power = 0;
    n = advice_build(&f, nullptr, a);
    ck(n == 2 && has(a, n, "governor") && has(a, n, "wifipower"), "on battery: both");

    // No Wi-Fi in the image: no Wi-Fi advice, and the clock by hand.
    f = healthy(); f.has_wifi = false; f.wifi_saved = false; f.clock_set = false;
    n = advice_build(&f, nullptr, a);
    ck(n == 1 && !strcmp(a[0].id, "clock") && !strcmp(a[0].action, "date set"), "no Wi-Fi: date set, and no 'join Wi-Fi'");
    f.has_wifi = true;
    n = advice_build(&f, nullptr, a);
    ck(n == 2 && has(a, n, "wifi") && !strcmp(a[1].action, "date set"), "Wi-Fi never joined: join it, and date set until then");

    f = healthy(); f.boot_clock_mhz = 300;
    n = advice_build(&f, nullptr, a);
    ck(n == 1 && a[0].level == ADV_INFO, "a boot clock over 250: information");
    f.boot_clock_mhz = 250;
    ck(advice_build(&f, nullptr, a) == 0, "250 itself is fine");

    ck(advice_known("update") && advice_known("bootclock") && !advice_known("updat") && !advice_known(nullptr),
       "known ids, and nothing else");

    printf("  advice: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
