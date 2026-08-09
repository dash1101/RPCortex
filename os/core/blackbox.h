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

    // --- fields with exactly one writer each ---------------------------------
    //
    // `phase` above is a single shared slot and fw_millis writes it on EVERY
    // ABI call, which a drawing package makes dozens of times a frame. So a
    // note written by the core that stopped is erased a millisecond later by
    // whatever the OTHER core is still happily running — the two processors do
    // not stop together. The hardware-lock guard has been writing "hw lock
    // taken twice - deadlock" into that slot for as long as it has existed and
    // not one of those notes has ever survived to be read.
    //
    // These do not share a writer with anything, so they survive.
    uint32_t hw_twice;             // hardware spinlock taken twice on one core
    uint32_t hw_twice_pc;          // the caller that asked for it the second time
    uint8_t  hw_twice_core;
    uint8_t  stall_crit;           // was a lock held when the longest stall was seen
    uint32_t max_stall_ms;         // longest the preempt tick ever saw core 0 held
    uint32_t max_stall_pc;         // and the instruction it was sitting on
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
// What the last note said. For a diagnostic that wants to report where the
// machine got to without clearing it.
const char *bb_phase(void);

// The hardware spinlock was asked for by a core that already holds it. That is
// not a wait, it is a permanent stop with interrupts masked, so this is the
// last thing that core will ever do — recorded with the return address of
// whoever asked, since there is no other way to find out.
void bb_note_hw_twice(uint8_t core, uint32_t pc);

// The longest the preemption tick ever saw core 0 held without yielding, and
// whether a lock was held at the time. Together these say which KIND of hang
// happened: a tick that kept arriving and kept deferring means a task looping
// with a lock held, and a tick that never saw a long stall at all means the
// core stopped taking interrupts, which is a spinlock.
// The program counter comes from the frame the exception pushed, so it is the
// instruction the core was sitting on rather than the one that set the stall
// going. For a core spinning in a wait loop those are the same thing, and that
// is exactly the case this exists to name.
void bb_note_stall(uint32_t ms, bool crit, uint32_t pc);

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
