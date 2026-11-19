// Whether a stopped service should be started again. See svcpolicy.h.
#include "svcpolicy.h"

void svc_started(SvcState *s, uint32_t now_ms) {
    s->started_ms  = now_ms;
    s->retry_at_ms = 0;
    s->retired     = false;
}

SvcAction svc_decide(SvcState *s, SvcSeen seen, uint32_t now_ms) {
    if (s->retired) return SVC_NOTHING;

    switch (seen) {
    case SVC_RUNNING:
        // Long enough up to count as recovered.
        if (s->failures && (uint32_t)(now_ms - s->started_ms) >= SVC_HEALTHY_MS) s->failures = 0;
        return SVC_NOTHING;

    case SVC_KILLED:
    case SVC_FINISHED:
    case SVC_GONE:
        s->retired = true;
        return SVC_LET_GO;

    case SVC_FAILED:
        break;
    }

    if (s->failures >= SVC_MAX_RESTARTS) {
        s->retired = true;
        return SVC_GIVE_UP;
    }
    if (!s->retry_at_ms) {
        // 2, 4, 8, 16, 32 seconds. The OR with 1 keeps a deadline that happens
        // to land on zero from reading as "none pending".
        s->retry_at_ms = (now_ms + (SVC_BACKOFF_MS << s->failures)) | 1u;
        return SVC_WAIT;
    }
    if ((int32_t)(now_ms - s->retry_at_ms) < 0) return SVC_WAIT;
    s->failures++;
    s->retry_at_ms = 0;
    return SVC_RESTART;
}
