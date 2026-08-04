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
// Stack sizes, measured rather than guessed.
//
// The deepest frames in this firmware are __sbprintf at 1128 bytes and
// storage_copy at 440, and they NEST: a task that prints inside a filesystem
// operation needs both at once plus littlefs's own 500-odd. Anything under 2 KB
// that touches either is a stack overflow waiting to happen — and an overflow
// here is not a crash, it is silent corruption of whatever malloc put below the
// stack, which on this device was littlefs's cache. That got written to flash
// and left a board that would not boot until it was erased.
//
// So the minimum is 2 KB, not 512 bytes. A task that genuinely needs less is not
// worth the risk of being wrong about it.
#define TASK_STACK_MIN   2048
#define TASK_STACK_DEF   3072
// The shell nests deepest of ALL: read_line, run_line, run_segment, run_one,
// apps_launch, app_main, then whatever a package does — and __sbprintf alone
// wants 1128 bytes somewhere in there. It gets 8 KB because it is the one task
// every command runs inside, and because the alternative was found the hard way.
#define TASK_STACK_SHELL 8192

// What a task needs if it will touch the wireless driver or lwIP. Those call
// deep and do not yield on the way, so the guard cannot catch an overflow part
// way — the size has to be right up front. The boot WiFi join learnt this by
// being given 3 KB and corrupting memory until it hard faulted somewhere else
// entirely.
#define TASK_STACK_NET   8192

// Bytes at the low end of every stack kept as a tripwire. Checked at every
// yield: if they have changed, the task has run off the end and the OS says so
// by name instead of corrupting something and carrying on.
//
// This is the software half. It survives alongside the hardware guard rather
// than being replaced by it, for two reasons: it is what the host tests can
// exercise, and it catches a wild write into the bottom of the stack, which is
// not the same event as the stack pointer crossing a line.
#define TASK_GUARD_BYTES 16

// Every stack starts on a boundary of this, so the hardware guard can be placed
// on it. Both architectures protect memory in 32-byte units at minimum, so one
// number serves both — and a stack that is merely 8-byte aligned, which is all
// malloc promises, cannot carry a guard at all.
#define TASK_STACK_ALIGN 32u

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

// Clear the current task's stop request.
//
// A stop request is aimed at a COMMAND, but it lands on the task running it —
// and the shell is a long-lived task that runs every command. Left set, it made
// task_should_stop true forever, and since intr_check folds that in, every
// interruptible loop afterwards gave up instantly: WiFi scans found nothing,
// pings timed out immediately, and the device looked broken while the shell
// itself carried on fine. The shell clears it when it returns to the prompt.
void task_clear_stop(void);

// Which slot in the task table the running task occupies, or -1.
//
// A stable small integer, for anything that needs per-task storage of its own
// without adding a field to every task in the system — the sandbox keeps four
// words per task this way. The slot is reused when a task is reaped, so
// whatever is kept against it has to be cleared then; task_slot_recycled is
// the notification for that.
int task_slot_index(void);

// The shell's pid, or 0 before it starts.
//
// The watchdog and task_kill both refuse to terminate the shell, since ending
// it leaves a device with no way to type anything. That used to be a hardcoded
// pid 1, which stopped being the shell the moment main became the idle task and
// the shell was spawned — so the guard was protecting the idle task and happily
// signalling the shell.
void task_mark_shell(void);
int  task_shell_pid(void);

// Finish the current task with a status. Does not return.
void task_exit(int code);

int  task_self(void);

// Whether the running task is holding a lock, and so must not be force-exited
// part-way through. Counted per task because a task may migrate cores while
// holding one; see lock.cpp.
// Change where a task is allowed to run. Changing it does NOT move a task that
// is already running — see task_migrate_to.
bool         task_set_affinity(int pid, TaskAffinity aff);
TaskAffinity task_affinity(int pid);

// Move the CURRENT task onto `core` and return once it is there. False if it
// could not be moved, which a caller must handle rather than assume.
bool task_migrate_to(uint32_t core);

void task_crit_enter(void);
void task_crit_leave(void);
bool task_crit_active(void);
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
// Park the current context and resume another.
//
// `live_out`, when given, is cleared the instant after the outgoing stack
// pointer has been stored — with a barrier between the two, because the other
// core observing the clear before the new sp would defeat the point entirely.
// It has to happen in here: from a task's own side this call does not return
// until it is resumed, so there is no moment in C between "sp is safely away"
// and "another core may take me". Null for the scheduler's own context, which
// no task can be scheduled onto.
void  task_ctx_switch(void **save_sp, void *to_sp, volatile bool *live_out);

// Milliseconds since boot, for sleep deadlines and CPU accounting.
uint32_t task_now_ms(void);

// Microseconds since boot, 32-bit and wrapping every ~71 minutes. For rate
// limiting only, where a wrap costs one early call and nothing else. It exists
// because task_now_ms needs a 64-bit division, which is far too expensive for
// something on the hot path of every ABI call.
uint32_t task_now_us(void);

// How many cores the scheduler may use. 1 on a single-core part, or when
// multicore is disabled. Called once by task_init.
uint32_t task_core_count(void);

// The core this code is running on, 0-based.
uint32_t task_this_core(void);

// Called when a task's stack tripwire has been breached. Does not return: the
// damage is already done, and continuing would spread it. The platform reports
// and reboots.
void task_stack_overflow(const char *name, uint32_t size) __attribute__((noreturn));

// The watchdog. Started once the shell is up, fed by the scheduler. A device
// that stops yielding has stopped making progress, and rebooting beats sitting
// dark until someone unplugs it.
// Arm the timer that force-terminates a task which has stopped yielding
// entirely. Separate from the watchdog: the watchdog reboots the device, this
// ends one task and leaves everything else running. Call once, after task_init.
void task_preempt_start(void);

void task_watchdog_start(void);
void task_watchdog_feed(void);

// "Still working." For code that legitimately runs a long time WITHOUT yielding
// — a benchmark loop, a flash write, a long computation inside a package.
//
// Liveness cannot depend on the running code choosing to yield. A package doing
// real work looks exactly like a hang to a watchdog that is only fed by the
// scheduler, and that is why `bench` — which never yields at all — was killed
// after eight seconds while doing precisely what it was asked to do.
//
// So every ABI entry point calls this. A package doing anything at all is
// calling fw_printf, fw_millis or fw_file_* regularly, which makes it a natural
// liveness signal that costs a package nothing to provide. It also checks the
// stack, which the yield path could not do for code that never yields.
void task_alive(void);

// Bytes still available below the current stack pointer on the MAIN stack. pid 1
// runs on the C startup stack, which this scheduler never allocated and so
// cannot paint with a tripwire — this is the equivalent check for it.
uint32_t task_main_stack_headroom(void);
// Total size of that stack, from the linker.
uint32_t task_main_stack_size(void);

// Fill the unused part of the main stack with a pattern, so how deep the boot
// sequence actually went can be counted afterwards. Called once, from main,
// before anything else runs. Nothing else may call it: it writes over memory
// that is only unused because the caller is main.
void task_main_stack_paint(void);
// How much of the main stack has been touched since. Zero if it was never
// painted, which is not the same as "all of it" and must not be reported as
// such.
uint32_t task_main_stack_used(void);

// --- hardware memory protection ---------------------------------------------

// Point this core's hardware stack guard at the stack it is now running on.
//
// Called at every place execution RESUMES on a core — after each context
// switch returns, and at the top of a new task — and nowhere else. That is the
// only correct place, for a reason that is easy to miss: the protection
// hardware is per-core, and a task can park on one core and be resumed on the
// other. Programming the guard when a task is created, or from the core that
// parked it, leaves the guard describing a stack that is no longer the one
// being used. Nothing fails; it simply protects the wrong task, forever.
//
// A null `bottom` means the core's own boot stack — which both the adopted
// main task and the scheduler loop run on, and which the platform can locate
// from the linker without being told.
//
// The mechanism differs by architecture and this is where that stops
// mattering. ARMv8-M has a dedicated stack limit register, which costs one
// instruction and leaves all eight protection regions free. ARMv6-M has no
// such register, so it spends a region on it.
void task_stack_guard_set(const void *bottom, uint32_t size);

// A task table slot is being reused, so anything kept AGAINST that slot is now
// about to describe a different task.
//
// The sandbox keeps four words per slot, one of which is where a package will
// resume and another the gate in its veneer pool. A slot handed on with those
// still set gives the next task a return address into a package that has been
// unloaded and freed — and it is reached from assembly, so there is nothing to
// notice it. Slots are recycled by design here: a finished task keeps its slot
// so its exit status stays readable, and task_spawn reclaims the oldest when
// the table is full.
void task_slot_recycled(int slot);

// The memory a loaded package occupies, described for the protection hardware.
//
// Two blocks, because they want opposite permissions: code must be executable
// and must not be writable, data must be writable and must never be executed.
// A package where those are the same block cannot have either.
struct TaskAppMem {
    const void *text;        // code and read-only data
    uint32_t    text_size;
    void       *data;        // data and bss
    uint32_t    data_size;
    const void *veneer;      // the loader's call trampolines: code
    uint32_t    veneer_size;
    // The two a SANDBOXED package also needs. Zero when it runs with the OS's
    // own privileges, because then it reaches everything anyway.
    void       *stack;       // its own, not the shell's
    uint32_t    stack_size;
    void       *arena;       // what fw_malloc hands it
    uint32_t    arena_size;
};

// Apply, or withdraw, the package regions for the CALLING task.
//
// Held per task rather than per core, and re-applied on resume alongside the
// stack guard, because a package yields: any ABI call it makes can put another
// task on the core. Left global, a second package would overwrite the first's
// regions — and when that second package unloaded, the regions would still be
// describing heap that had been freed and handed out again. The first write
// into the reused block would then fault in code that had done nothing wrong.
void task_app_mem_set(const TaskAppMem *mem);
void task_app_mem_clear(void);

// The heap the running task's package allocates from, or null when it is not in
// a sandboxed package and fw_malloc should use the ordinary one.
//
// Per task for the same reason the regions are: a package yields on every ABI
// call it makes, and the next task on the core has a different answer.
struct Arena;
Arena *task_arena(void);
void   task_arena_set(Arena *a);

// Read back what this task holds, so an entry into package code can put the
// previous description back rather than clearing it. Nothing today can nest —
// no ABI call lets a package invoke a command — but "clear on the way out" is
// only correct while that stays true, and it would fail by leaving the OUTER
// package running with no protection at all. Returns false if there was none.
bool task_app_mem_get(TaskAppMem *out);

// The pid of a task, other than this one, that is currently inside the package
// whose code starts at `text` — or -1 if none is.
//
// Asked before a package is unloaded. A task can be parked in the middle of a
// package's command while something else removes that package: the shell yields
// on every ABI call the package makes, and a background job can run `unload` in
// the meantime. Freeing the image then leaves the parked task about to resume
// into code that has been handed back to the heap, and — since the protection
// description is held per task — about to mark a reused block read-only, so the
// next allocation out of it faults in something unrelated.
//
// Finished tasks are ignored. They never resume, so what they were holding
// cannot matter, and counting them would make a package permanently unremovable
// after any command that ended by exiting its task.
int task_app_mem_holder(const void *text);

// The platform half of the two above: program the hardware for whatever the
// resuming task holds. Null means "no package is running here".
void task_app_mem_apply(const TaskAppMem *mem);

}  // extern "C"

#endif  // RPC_TASK_H
