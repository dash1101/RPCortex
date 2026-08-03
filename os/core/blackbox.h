// The black box.
//
// After a watchdog reboot the one question is "what was it doing?", and the log
// ring cannot answer it: a hang produces no log lines, because logging is
// something running code does and the code has stopped. logdump was empty for
// exactly that reason — there was nothing wrong to report until it was already
// too late to report it.
//
// So this records the answer CONTINUOUSLY, in memory the startup code does not
// clear, and it survives the reset that follows. It is written on every
// schedule, which means a few stores in the hot path and no allocation, and it
// is read once at the next boot.
//
// What it holds is chosen for one purpose: naming the culprit. The task, the
// command line it was running, how long since it last yielded, and how many
// times it had yielded before it stopped — which is the difference between "it
// hung immediately" and "it ran fine two thousand times and then stopped".
#ifndef RPC_BLACKBOX_H
#define RPC_BLACKBOX_H

#include <stdint.h>

#define BB_NAME_MAX 16
#define BB_CMD_MAX  72

struct BlackBox {
    uint32_t magic;
    int      pid;
    uint8_t  core;
    char     task[BB_NAME_MAX];    // what was scheduled
    char     cmd[BB_CMD_MAX];      // the command line being run, if any
    uint32_t last_yield_ms;        // when the scheduler last saw progress
    uint32_t yields;               // how many times it had yielded
    uint32_t stack_used;           // and how close it was to its limit
    uint32_t stack_size;
    char     phase[40];            // the last checkpoint a program reported
};

// Record the task about to run. Called from the scheduler; deliberately cheap.
void bb_note_task(int pid, uint8_t core, const char *name,
                  uint32_t stack_used, uint32_t stack_size);

// Record the command line the shell is about to execute, so a hang can name the
// command and not only the task it happened inside.
void bb_note_command(const char *line);

// Mark progress. Separate from bb_note_task because a long-running task keeps
// yielding without being rescheduled onto a different core.
void bb_note_yield(uint32_t now_ms);

// A checkpoint inside a long-running program. Printed output is lost when the
// device hangs — the terminal never receives what was in the USB buffer — but
// this survives the reboot, so the last checkpoint reached names the exact step
// that did not finish. Exposed to packages as fw_progress.
void bb_note_phase(const char *what);

// Read what the previous run left behind. Returns null when there is nothing —
// a cold boot, or a clean shutdown.
const BlackBox *bb_previous(void);

// Prepare for this run. Must be called before anything else touches it: it
// snapshots whatever the last run left, then resets for this one.
void bb_init(void);

// How long the running task has gone without yielding. The number the graded
// watchdog acts on.
uint32_t bb_stall_ms(uint32_t now_ms);

#endif  // RPC_BLACKBOX_H
