// Detached shell runs — the bookkeeping behind fw_shell_run_detached.
//
// A package that wants to run a shell command it cannot run on its own task
// asks for one of these. The firmware spawns a task, runs the command there,
// and this is the record that connects the two: which package asked, whether it
// has finished, what it returned, and where its output goes.
//
// It is here rather than in api.cpp because none of it needs a CPU. The slot
// cap, the generation on a handle, the ownership test and the rule about who
// gets the output buffer are exactly the parts where a mistake is a package
// reading somebody else's answer, and all of them are ordinary testable code.
//
// --- what the handle is for ---------------------------------------------------
//
// A small integer, never a pointer, the same way a TCP handle is: the firmware
// looks it up in a table it owns, so the worst a wrong one can do is come back
// refused. It carries a GENERATION as well as a slot, so a handle kept past the
// run it named does not quietly land on whichever run got the slot next — which
// on a screen firing a command per keypress is not a hypothetical.
#ifndef RPC_DETACH_H
#define RPC_DETACH_H

#include <stdint.h>

// How many detached runs at once.
//
// Two, and it is a real limit rather than a round number. Each one is a task out
// of the twelve the scheduler has and a stack the size the shell's own is, held
// for as long as the command takes. The shape this exists for is "fire a command
// and wait for it", not parallelism — and a screen that needs three at once has
// a design problem the OS should not be papering over.
#define DETACH_MAX      2

// The longest line, matching RPC_SHELL_LINE_MAX.
#define DETACH_LINE_MAX 256

// The captured output, and there is ONE of these shared between the slots.
//
// Not per slot, and not an allocation. The OS has a single output capture — see
// out.cpp — so only one run can be collecting output at any moment anyway;
// giving every slot a buffer would be reserving memory for a thing that cannot
// happen. Making it structural means the rule is enforced by there being one
// buffer rather than by a check somebody can forget.
//
// 1024 bytes is what the Nova D1's own command screen uses, which is about
// fifty lines — enough for every command a screen sensibly runs, and a command
// that prints more than that is one whose output nobody was going to read on an
// OLED.
#define DETACH_OUT_MAX  1024

struct DetachRun {
    uint8_t     used;
    uint8_t     done;
    uint8_t     gen;          // bumped on every claim of this slot
    uint8_t     capturing;    // this run holds the shared output buffer
    const void *owner;        // the package's image; null once it has unloaded
    int         pid;          // the task running it, for `ps` and for a report
    int         status;       // what the command returned, once done
    uint32_t    seq;          // claim order, so "the oldest finished" has meaning
    char        line[DETACH_LINE_MAX];
    // Where the output goes when the run is COLLECTED. This is package memory
    // and it is never touched by the detached task — see fw_shell_done.
    char       *pkg_out;
    uint32_t    pkg_cap;
};

// Claim a slot for `owner` and copy `line` into it. `pkg_out`/`pkg_cap` describe
// the package's own buffer, or null/0 for a run whose output nobody wants.
//
// Returns a handle >= 0, or -1 when there is no room: every slot is taken and
// none of them has finished. A slot that HAS finished and was never collected is
// reclaimed here — oldest first — so a package that fires runs and never polls
// costs a bounded amount rather than jamming the table for ever.
//
// A run that wants output when another capturing run is still going is refused
// rather than started, because there is one capture in the OS and the second one
// would come back with an empty buffer and no way to tell why.
int detach_claim(const void *owner, const char *line,
                 char *pkg_out, uint32_t pkg_cap);

// The run a handle names, checked against the package that claimed it. Null for
// a handle that is out of range, stale, or somebody else's.
DetachRun *detach_find(int handle, const void *owner);

// The same without the ownership test, for the firmware's own side of the run.
DetachRun *detach_of(int handle);

// The shared output buffer, if this run is the one holding it. Null otherwise,
// which is how the detached task knows whether to capture at all.
char *detach_capture_buffer(int handle);

// The run has finished. Called from the detached task, once.
void detach_finish(int handle, int status);

// Give the slot back. Called when a poll has handed the answer over.
void detach_release(int handle);

// A package has been unloaded. Anything it started can no longer be collected —
// the handle belonged to code that is not there any more — but a run still in
// flight keeps its slot until it ends, because the task is real and the slot is
// what it reports into. Nothing here points into the package's memory except
// pkg_out, and that is only ever read by a poll, which can no longer happen.
void detach_forget_owner(const void *owner);

// How many slots are in use, for a report.
uint32_t detach_active(void);

// Start again. For the host test only; nothing on the device calls it.
void detach_reset(void);

#endif  // RPC_DETACH_H
