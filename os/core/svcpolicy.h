// Whether a service that has stopped should be started again, and when.
//
// A service is a line in /etc/services.cfg, started at boot as a background
// task. Before this nothing watched them: one that crashed stayed dead until the
// next reboot, and on a device left running for days that is the same as never.
//
// What a stop MEANS decides it, and only one kind is a reason to restart:
//
//   * it FAILED (a non-zero exit, which is also what a contained fault ends as):
//     start it again, after a pause that doubles each time, and give up after a
//     few in a row so a service that dies on start cannot spin forever;
//   * it was STOPPED on purpose (killed): leave it, somebody wanted it off;
//   * it FINISHED cleanly (exit 0): it was a one-shot, and starting it again
//     would run it twice;
//   * it is GONE, its slot reused before anyone looked: why it ended is not
//     known, and restarting on a guess could undo a kill. Left alone and said so.
//
// A service that then runs for a while has recovered, and its count of failures
// starts again from nothing, so one crash a week never adds up to giving up.
//
// Pure: the caller says what it saw and when, and is told what to do. Every
// sequence of crashes, waits and recoveries can be walked through on a host.
#ifndef RPC_SVCPOLICY_H
#define RPC_SVCPOLICY_H

#include <stdint.h>

#define SVC_MAX_RESTARTS   5          // in a row, before giving up
#define SVC_BACKOFF_MS     2000u      // the first wait; each after it doubles
#define SVC_HEALTHY_MS     60000u     // running this long clears the failure count

enum SvcSeen   { SVC_RUNNING, SVC_FAILED, SVC_KILLED, SVC_FINISHED, SVC_GONE };
enum SvcAction { SVC_NOTHING,         // keep watching
                 SVC_WAIT,            // failed; the pause before the restart is running
                 SVC_RESTART,         // start it now
                 SVC_GIVE_UP,         // failed too often; stop watching (said once)
                 SVC_LET_GO };        // stopped for a reason that is not a failure

struct SvcState {
    uint32_t started_ms;     // when the current run began
    uint32_t retry_at_ms;    // when a pending restart is due; 0 when none is
    uint8_t  failures;       // restarts in a row since it last ran healthily
    bool     retired;        // given up on, or let go: no more decisions
};

// A service just started (at boot, or by a restart the caller made).
void svc_started(SvcState *s, uint32_t now_ms);

// Decide, from what the task looks like now.
SvcAction svc_decide(SvcState *s, SvcSeen seen, uint32_t now_ms);

#endif  // RPC_SVCPOLICY_H
