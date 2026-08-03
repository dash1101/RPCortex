// The task scheduler.
//
// Tasks are green threads: each gets a real stack and is switched by saving and
// restoring registers, so a task can block ANYWHERE — halfway down a call chain,
// inside getchar, in the middle of a directory walk. That is what makes a second
// shell instance possible at all. A callback/state-machine scheduler is cheaper
// in RAM but cannot host a shell, because a shell is deeply nested blocking code
// and rewriting it as a state machine is not a refactor, it is a different
// program. v1 hit exactly this wall with uasyncio.
//
// Scheduling is COOPERATIVE: a task runs until it yields. That is a deliberate
// choice, not a limitation to fix later —
//
//   * littlefs is not reentrant and neither is most of this OS. Preemption would
//     mean auditing every shared structure for interruption at any instruction,
//     rather than at known points.
//   * the yield points already exist. Every intr_check() call added for Ctrl+C
//     sits exactly where a yield belongs, and the shell's own getchar is the
//     biggest one of all: a prompt waiting for a keystroke is 99% of a device's
//     idle time, and yielding there alone gives real concurrency.
//
// Preemption can be layered on later behind the same switch — the tick would
// simply call task_yield from an interrupt. Nothing here forecloses it.
//
// SMP: one scheduler per core. Core 0 always exists. Core 1 is used when the
// chip has one and the build enables it, but nothing REQUIRES it — a task with
// AFFINITY_ANY runs wherever there is room, so the same OS works on a
// single-core part with no code changes and no features missing.
#ifndef RPC_TASK_H
#define RPC_TASK_H

#include <stdint.h>

#define TASK_MAX        12
#define TASK_NAME_MAX   16
#define TASK_PATH_MAX   48
#define TASK_STACK_MIN  512
// The shell needs room for the line editor, path buffers and a command's own
// locals. Measured against the deepest path (tree recursion at depth 8).
#define TASK_STACK_SHELL 4096
#define TASK_STACK_DEF   2048

enum TaskState {
    TASK_FREE = 0,
    TASK_READY,        // runnable, waiting for a turn
    TASK_RUNNING,      // has the core right now
    TASK_SLEEPING,     // waiting for a deadline
    TASK_BLOCKED,      // waiting on something else to release it
    TASK_DONE,         // finished; kept until reaped so exit status is readable
};

// Which core a task may run on. ANY is the default and the reason a single-core
// build needs no special casing.
enum TaskAffinity { AFFINITY_ANY = 0, AFFINITY_CORE0 = 1, AFFINITY_CORE1 = 2 };

typedef int (*TaskFn)(void *arg);

struct TaskInfo {
    int         pid;
    char        name[TASK_NAME_MAX];
    char        path[TASK_PATH_MAX];
    TaskState   state;
    TaskAffinity affinity;
    uint8_t     core;             // the core it last ran on
    uint32_t    stack_size;
    uint32_t    stack_used;       // high-water mark, from the fill pattern
    uint32_t    heap_bytes;       // attributed allocations, if the task reports them
    uint32_t    cpu_ms;           // accumulated run time
    uint32_t    switches;         // how many times it has been scheduled
    int         exit_code;
    bool        kill_requested;
};

// --- lifecycle --------------------------------------------------------------

// Bring the scheduler up and adopt the CURRENT execution context as pid 1. The
// caller keeps running; it simply becomes a task from here on.
void task_init(const char *name);

// Start a task. Returns its pid, or -1 if the table is full or the stack could
// not be allocated. `path` is what it was loaded from, for the task manager;
// it may be null.
int task_spawn(const char *name, const char *path, TaskFn fn, void *arg,
               uint32_t stack_bytes, TaskAffinity affinity);

// Give up the core. Returns when this task is scheduled again.
void task_yield(void);

// Yield until at least `ms` has passed. The task is not scheduled meanwhile,
// which is the difference between this and a busy wait.
void task_sleep_ms(uint32_t ms);

// Ask a task to stop. This does NOT unwind it: a task is killed at its next
// yield point, where it is by definition not holding a lock or half-way through
// a flash write. A task that never yields cannot be killed, which is a property
// worth having over one that can be torn down mid-write.
bool task_kill(int pid);

// Whether the CURRENT task has been asked to stop. Long-running work checks this
// the same way it checks for Ctrl+C — in fact intr_check() folds it in.
bool task_should_stop(void);

// Finish the current task with a status. Does not return.
void task_exit(int code);

int  task_self(void);
const TaskInfo *task_current(void);

// --- introspection, for the task manager ------------------------------------

uint32_t        task_count(void);        // slots in use, including DONE
const TaskInfo *task_at(uint32_t i);
const TaskInfo *task_find(int pid);
// Drop a DONE task's slot once its status has been read.
bool task_reap(int pid);

// --- the platform seam ------------------------------------------------------
//
// Everything above is pure table and policy, so it host-tests. These three are
// the only parts that touch a CPU, and the host test supplies its own.

// C linkage throughout: task_ctx_switch is written in assembly and defines an
// unmangled symbol, so a C++ declaration would look for a name the assembler
// never emitted. The rest follow for consistency — a seam that is half-mangled
// is a trap for whoever ports it next.
typedef void (*TaskEntry)(void);
extern "C" {

// Prepare `stack_top` so that switching to it begins executing entry().
// Returns the initial stack pointer.
void *task_ctx_init(void *stack_top, TaskEntry entry);

// Save the current context into *save_sp and resume `to_sp`.
void  task_ctx_switch(void **save_sp, void *to_sp);

// Milliseconds since boot, for sleep deadlines and CPU accounting.
uint32_t task_now_ms(void);

// How many cores the scheduler may use. 1 on a single-core part, or when
// multicore is disabled. Called once by task_init.
uint32_t task_core_count(void);

// The core this code is running on, 0-based.
uint32_t task_this_core(void);

}  // extern "C"

#endif  // RPC_TASK_H
