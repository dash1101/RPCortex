// core/governor: every rule, in the order governor.h gives them, and a day in
// the life (quiet, a burst, quiet again, plugged in, a peripheral opened).
#include "../core/governor.h"

#include <stdio.h>

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", what); }
}

static GovIn in(GovMode m, int power, bool held, unsigned load, uint32_t now, unsigned cur) {
    GovIn i;
    i.mode = m; i.power = power; i.held = held; i.load = load; i.now_ms = now;
    i.base_mhz = 150; i.floor_mhz = 48; i.cur_mhz = cur;
    return i;
}

int main(void) {
    GovState st = {false, 0};
    GovReason why;
    GovIn i;

    // 1. Off, and held.
    i = in(GOV_OFF, 0, false, 0, 0, 48);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_OFF, "off: back to the base, even quiet on battery");
    i = in(GOV_ON, 0, true, 0, 100000, 48);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_HELD, "held: the base, however quiet");

    // 2. Auto follows the power source; not known counts as plugged in.
    st = {false, 0};
    for (uint32_t t = 0; t <= 20000; t += 1000) { i = in(GOV_AUTO, 1, false, 0, t, 150); gov_decide(&i, &st, &why); }
    ck(why == GOVR_PLUGGED && gov_decide(&i, &st, nullptr) == 150, "auto on USB: never lower");
    i = in(GOV_AUTO, -1, false, 0, 30000, 150);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_PLUGGED, "auto, power not known: treated as plugged in");

    // 3-4. Quiet on battery: settles, then the floor; busy: straight back.
    st = {false, 0};
    i = in(GOV_AUTO, 0, false, 5, 1000, 150);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_SETTLING, "quiet: waits first");
    i = in(GOV_AUTO, 0, false, 5, 5999, 150);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_SETTLING, "...not one millisecond early");
    i = in(GOV_AUTO, 0, false, 5, 6000, 150);
    ck(gov_decide(&i, &st, &why) == 48 && why == GOVR_QUIET, "quiet for 5 s: the floor");
    i = in(GOV_AUTO, 0, false, 80, 7000, 48);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_BUSY, "busy: back to the base at once");
    i = in(GOV_AUTO, 0, false, 5, 8000, 150);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_SETTLING, "after a burst the 5 s starts again");

    // 5. In between: unchanged, and it resets the quiet wait.
    st = {false, 0};
    i = in(GOV_ON, 1, false, 5, 0, 150);     gov_decide(&i, &st, &why);
    i = in(GOV_ON, 1, false, 30, 3000, 150);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_STEADY, "between the thresholds: stays");
    i = in(GOV_ON, 1, false, 5, 6000, 150);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_SETTLING, "a middling second restarts the wait");
    i = in(GOV_ON, 1, false, 5, 11000, 150);
    ck(gov_decide(&i, &st, &why) == 48, "on: lowers even on USB power");
    i = in(GOV_ON, 1, false, 30, 12000, 48);
    ck(gov_decide(&i, &st, &why) == 48 && why == GOVR_STEADY, "between the thresholds at the floor: stays at the floor");

    // A peripheral opened at the floor puts it straight back.
    i = in(GOV_ON, 1, true, 0, 13000, 48);
    ck(gov_decide(&i, &st, &why) == 150 && why == GOVR_HELD, "held at the floor: the base");

    // Bounds.
    i = in(GOV_ON, 0, false, 0, 0, 150); i.base_mhz = 40;
    st = {true, 0}; i.now_ms = 10000;
    ck(gov_decide(&i, &st, &why) == 40, "a base below the floor: the base is the floor too");
    i = in(GOV_ON, 0, false, 30, 0, 200);
    ck(gov_decide(&i, &st, &why) == 150, "never above the base, whatever it was told");

    GovMode m;
    ck(gov_parse_mode("auto", &m) && m == GOV_AUTO, "parse auto");
    ck(gov_parse_mode("on", &m) && m == GOV_ON && gov_parse_mode("off", &m) && m == GOV_OFF, "parse on, off");
    ck(!gov_parse_mode("fast", &m) && !gov_parse_mode(nullptr, &m), "anything else is refused");

    printf("  governor: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
