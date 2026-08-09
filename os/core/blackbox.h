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

// Whether anything could have been done about the stall that ended the run.
//
// A watchdog reboot looks the same whether the OS tried everything and had
// nothing left, or never noticed. The preemption alarm knows which, and this is
// how it says so across the reset.
enum BbStuck {
    BB_STUCK_NO = 0,        // nothing to report: no stall, or one that was dealt with
    BB_STUCK_TASK,          // a task stopped responding and could not be ended
    BB_STUCK_PACKAGE,       // ...and it was a package's code that was running
};

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
    uint8_t  stuck;                // a BbStuck: why nothing saved the run
};

// Record the task about to run. Called from the scheduler; deliberately cheap.
void bb_note_task(int pid, uint8_t core, const char *name,
                  uint32_t stack_used, uint32_t stack_size);

// Record the command line the shell is about to execute, so a hang can name the
// command and not only the task it happened inside.
void bb_note_command(const char *line);

// Mark progress. Separate from bb_note_task because a long-running task keeps
// yielding without being rescheduled onto a different core.
//
// Also clears `stuck`: progress is the definition of not being stuck, and a
// flag left set by a stall the device recovered from would be reported against
// whatever went wrong next.
void bb_note_yield(uint32_t now_ms);

// Record that a stall could not be recovered from. Called FROM THE PREEMPTION
// ALARM, which is an interrupt that fired inside an arbitrary instruction — so
// this is one byte, written directly. No formatting, no lock, nothing that can
// need the stack of the task it is describing.
void bb_note_stuck(uint8_t why);

// A checkpoint inside a long-running program. Printed output is lost when the
// device hangs — the terminal never receives what was in the USB buffer — but
// this survives the reboot, so the last checkpoint reached names the exact step
// that did not finish. Exposed to packages as fw_progress.
void bb_note_phase(const char *what);
// What the last note said. For a diagnostic that wants to report where the
// machine got to without clearing it.
const char *bb_phase(void);

// Read what the previous run left behind. Returns null when there is nothing —
// a cold boot, or a clean shutdown.
const BlackBox *bb_previous(void);

// Prepare for this run. Must be called before anything else touches it: it
// snapshots whatever the last run left, then resets for this one.
void bb_init(void);

// Mark this run as ending on purpose. A deliberate reboot leaves exactly the
// same trace as a crash — the last thing the device was doing, frozen in memory
// the reset does not clear — so without this every `reboot` reported itself as
// an unclean shutdown. A crash detector that cries wolf on every restart is how
// a real crash gets scrolled past.
void bb_note_clean_exit(void);

// How long the running task has gone without yielding. The number the graded
// watchdog acts on.
uint32_t bb_stall_ms(uint32_t now_ms);

#endif  // RPC_BLACKBOX_H
