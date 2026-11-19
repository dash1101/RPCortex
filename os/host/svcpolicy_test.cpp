// The service restart policy, walked through the sequences it has to handle.
#include "../core/svcpolicy.h"

#include <stdio.h>

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", what); fails++; }
}

// Run the policy against a task that fails every time it is started, and return
// the times at which it was restarted.
static int crash_loop(uint32_t *at, int max) {
    SvcState s = {};
    uint32_t now = 1000;
    svc_started(&s, now);
    int n = 0;
    for (int step = 0; step < 2000 && n < max; step++) {
        now += 500;                                   // checked twice a second
        SvcAction a = svc_decide(&s, SVC_FAILED, now);
        if (a == SVC_RESTART) { at[n++] = now; svc_started(&s, now); }
        if (a == SVC_GIVE_UP) return -n - 1;          // negative: gave up after n restarts
    }
    return n;
}

int main(void) {
    printf("svcpolicy_test - restarting a service that has stopped\n");

    // --- a crash loop backs off and then gives up ---
    {
        uint32_t at[16];
        int r = crash_loop(at, 16);
        ck(r < 0, "a service that fails on every start is given up on");
        int n = -r - 1;
        ck(n == SVC_MAX_RESTARTS, "after exactly the configured number of restarts");
        bool doubling = true;
        for (int i = 2; i < n; i++) {
            uint32_t prev = at[i - 1] - at[i - 2], gap = at[i] - at[i - 1];
            if (gap < prev * 2 - 1000 || gap > prev * 2 + 1000) doubling = false;
        }
        ck(doubling, "and the pause before each restart doubles");
        ck(at[0] - 1000 >= SVC_BACKOFF_MS && at[0] - 1000 <= SVC_BACKOFF_MS + 1000,
           "the first restart waits the base pause, not longer");
    }

    // --- one failure, then a long healthy run, clears the count ---
    {
        SvcState s = {};
        svc_started(&s, 0);
        uint32_t now = 100;
        while (svc_decide(&s, SVC_FAILED, now) != SVC_RESTART) now += 500;
        svc_started(&s, now);
        ck(s.failures == 1, "one restart is counted");
        ck(svc_decide(&s, SVC_RUNNING, now + SVC_HEALTHY_MS - 1) == SVC_NOTHING && s.failures == 1,
           "running a little under a minute does not clear it");
        svc_decide(&s, SVC_RUNNING, now + SVC_HEALTHY_MS);
        ck(s.failures == 0, "running a minute does");

        // So five crashes spread a week apart never add up to giving up.
        bool gave_up = false;
        uint32_t t = now + SVC_HEALTHY_MS;
        for (int week = 0; week < 10; week++) {
            t += 7u * 24u * 3600u * 1000u;       // a week, in ms (fits in 32 bits)
            SvcAction a;
            while ((a = svc_decide(&s, SVC_FAILED, t)) == SVC_WAIT) t += 500;
            if (a == SVC_GIVE_UP) { gave_up = true; break; }
            svc_started(&s, t);
            t += SVC_HEALTHY_MS;
            svc_decide(&s, SVC_RUNNING, t);
        }
        ck(!gave_up, "occasional crashes with recovery in between are never given up on");
    }

    // --- stops that are not failures are left alone ---
    {
        SvcSeen kinds[] = { SVC_KILLED, SVC_FINISHED, SVC_GONE };
        const char *what[] = { "a killed service is not restarted",
                               "a service that finished cleanly is not restarted",
                               "a service whose fate is unknown is not restarted" };
        for (int k = 0; k < 3; k++) {
            SvcState s = {};
            svc_started(&s, 0);
            ck(svc_decide(&s, kinds[k], 100) == SVC_LET_GO, what[k]);
            ck(svc_decide(&s, SVC_FAILED, 100000) == SVC_NOTHING, "and nothing seen afterwards changes that");
        }
    }

    // --- giving up is said once ---
    {
        SvcState s = {};
        svc_started(&s, 0);
        s.failures = SVC_MAX_RESTARTS;
        ck(svc_decide(&s, SVC_FAILED, 10) == SVC_GIVE_UP, "past the limit, it gives up");
        ck(svc_decide(&s, SVC_FAILED, 20) == SVC_NOTHING, "and only once");
    }

    // --- a restarted service is watched again from scratch ---
    {
        SvcState s = {};
        svc_started(&s, 0);
        svc_decide(&s, SVC_KILLED, 10);              // pkg install stops it...
        svc_started(&s, 20);                         // ...and starts it again
        ck(!s.retired, "a service started again is watched again");
        ck(svc_decide(&s, SVC_FAILED, 30) == SVC_WAIT, "and its next failure is handled");
    }

    // --- the clock wrapping does not stall a pending restart ---
    {
        // The pause starts a moment before the counter wraps and ends after it.
        SvcState s = {};
        uint32_t t0 = 0xFFFFFC00u;
        svc_started(&s, t0);
        uint32_t now = t0;
        SvcAction a;
        int guard = 0;
        while ((a = svc_decide(&s, SVC_FAILED, now)) == SVC_WAIT && guard++ < 100) now += 250;
        ck(a == SVC_RESTART, "a restart due across the millisecond counter wrapping still happens");
        ck((uint32_t)(now - t0) >= SVC_BACKOFF_MS, "and not before its pause is over");
    }

    printf("\n  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
