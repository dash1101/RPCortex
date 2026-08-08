#include "apps.h"
#include "command.h"
#include "out.h"
#include "storage.h"
#include "kernel.h"
#include "blackbox.h"
#include "mpu.h"
#include "arena.h"
#include "sandbox.h"
#include "lock.h"
#include "logring.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Set around app_main so a registered command is tagged with its app (api.cpp)
// and a fault names the culprit (fault.cpp).
extern "C" void api_set_current_app(void *owner);
extern "C" volatile const char *g_current_app;

// A fault inside a package prints nothing from the handler — see the note in
// fault.cpp — and this reads the report out afterwards, from task context.
extern "C" int fault_report_contained(void);

// Fixed table, no allocation for the bookkeeping itself. A device that somehow
// loads 16 resident packages has a bigger problem than a full table.
#define APPS_MAX 16

static LoadedApp g_apps[APPS_MAX];
static bool      g_used[APPS_MAX];

static LoadedApp *find(const char *name) {
    for (int i = 0; i < APPS_MAX; i++)
        if (g_used[i] && strcmp(g_apps[i].header.name, name) == 0) return &g_apps[i];
    return nullptr;
}

// --- entering package code --------------------------------------------------

static void describe(const LoadedApp *a, TaskAppMem *m) {
    // ZEROED FIRST, and it is not defensiveness.
    //
    // This fills text, data and veneer. The stack and arena are filled later by
    // sandbox_acquire — and only if that succeeds. Every caller declares its
    // TaskAppMem as a bare local, so when the sandbox could NOT be acquired the
    // two were left holding whatever was on the stack at the time, and
    // task_app_mem_set programmed the protection unit with it.
    //
    // Random bases, random sizes, overlapping whatever they landed on. ARMv8-M
    // calls overlapping regions UNPREDICTABLE, which is exactly what it
    // behaved like: a fault at an address inside a region that permitted it,
    // on a stack with kilobytes free, at intervals that moved with whatever
    // the heap and the stack happened to contain.
    memset(m, 0, sizeof(*m));
    m->text        = a->image;
    m->text_size   = a->text_size;
    m->data        = a->data;
    m->data_size   = a->data_size;
    m->veneer      = a->veneers;
    // Only the part actually written, rounded up to a whole block.
    //
    // The pool is sized for the worst case — one veneer per relocation — and a
    // typical package uses a third of it, so covering the whole allocation
    // would put a read-only region over bytes that are still ordinary free
    // heap. But a veneer is SIXTEEN bytes and a region is described in
    // thirty-twos, so the used length is a multiple of 32 only half the time,
    // and the other half the hardware would refuse the region outright — which
    // reads as "the veneers are simply not protected" and says nothing at all.
    // Rounding up stays inside the pool, because the allocation itself was
    // padded to a whole number of blocks.
    m->veneer_size = mpu_align_up(a->veneers_used, MPU_V8_GRAIN);
    if (m->veneer_size > a->veneer_size) m->veneer_size = a->veneer_size;
}

// --- the sandbox ------------------------------------------------------------
//
// A sandboxed package needs two things the OS has been lending it: a stack of
// its own, and a heap of its own. Both are allocated for the duration of a call
// into package code and given back after, rather than held for the lifetime of
// a resident package — a package spends nearly all of its time not running, and
// twelve kilobytes each is not worth holding for that.
//
// The stack has to be a real one. Firmware called through the ABI runs on it:
// the supervisor call raises privilege and returns to the firmware function on
// whatever stack the package was using, so fw_printf's 1128-byte frame lands
// here. That is why it is sized like any other task's stack and not like a
// scratch buffer.
#define PKG_STACK_BYTES  TASK_STACK_DEF

// What the FIRMWARE needs on top of whatever the package asked for.
//
// An ABI call does not switch stacks: sandbox_svc raises privilege and the
// firmware function runs on the package's own stack. So `fw_file_write` runs
// littlefs there, `fw_printf` runs vsnprintf there — and vsnprintf alone wants
// over a kilobyte. None of that is the package's to predict, and asking authors
// to budget for the internals of calls they make is asking them to guess.
//
// This was invisible until the sandbox gave the stack an MPU region. Before
// that a package overflowing its stack wrote quietly into whatever the heap had
// put below it and nothing complained; now the region ends and the hardware
// says so — which is the protection working, not a new fault. The crash it
// surfaced (MSTKERR: an interrupt arriving with no room to stack its frame) had
// been a silent heap corruption in every build before this one.
//
// Two kilobytes was not enough, which is why this is now measured rather than
// estimated again. See apps_stack_peak(): the sandbox stack is painted and the
// high-water mark reported by `mpu`, so the right number is an observation
// instead of a third guess.
//
// Four kilobytes covered vsnprintf (over one), littlefs's write path, the
// storage frames under it, and an exception frame arriving at the deepest
// point — which on a part with an FPU is 104 bytes, not 32.
//
// It did not cover TLS, and that list is why: it was written before any package
// could reach one. `websearch` is the first to call fw_http_get with an https
// URL, which runs the whole handshake HERE — certificate parsing, chain
// verification, the bignum and elliptic-curve work under the signature check,
// and the hash functions under that. Adding up the largest frames along that
// path in the shipped image gives well over three kilobytes before any of the
// lwIP or fetch frames beneath it, against a reserve of four that also has to
// hold everything else.
//
// Eight, then. The cost is real — the pool holds four of these — and it is
// still the right trade: the alternative is that any package touching HTTPS
// faults somewhere inside mbedtls, which tells its author nothing.
//
// DEVICE-UNCONFIRMED as a number. The stack is painted and `mpu` prints the
// high-water mark, so running the deepest thing a package can do and reading it
// back is what turns this into a measurement. That is the check to make before
// trusting it, not a rebuild with a bigger guess.
#define FW_CALL_RESERVE  8192

// What an untouched byte of sandbox stack looks like.
#define STACK_PAINT      0xA5
// Sized from what a package actually asks for, not from what felt tidy.
//
// 2 KB was a guess and `stress` broke it immediately: it allocates 24 blocks of
// 256 bytes to check that the allocator never hands the same block to two
// callers, which is 6 KB, and it got seven of them. A package that cannot
// allocate is a package that does not work, and the sandbox is not a reason for
// it to have less memory than it had before.
//
// Held for the duration of one call into package code and given back after —
// see the note in UNPRIV-DESIGN.md about whether that churn should become a
// per-task allocation instead.
// Bigger than the largest single request a package is expected to make, not
// equal to it: every block carries a header, so an arena of exactly N bytes
// cannot serve a request for N. `bench` asks for 8192 and was refused by an
// 8192-byte arena.
#define PKG_ARENA_BYTES  12288

// What a default-sized sandbox stack actually costs, reserve included.
//
// Named once because three places need the same answer and one of them had a
// different one: apps_pool_bytes planned from PKG_STACK_BYTES alone, so
// `meminfo` reported the pool as 60 KB when it was holding 76. A figure that
// exists to say where the memory went is worth more than the four thousand
// bytes it was quietly leaving out.
#define PKG_STACK_TOTAL  (PKG_STACK_BYTES + FW_CALL_RESERVE)

struct SandboxAlloc {
    void    *stack_raw;
    void    *stack;
    uint32_t stack_bytes;
    void  *arena_raw;
    void  *arena_mem;
    Arena  arena;
};

// A package's stack and heap, held for as long as the task that uses them.
//
// They used to be allocated and freed around every call: three kilobytes and
// twelve, taken and given back for a command that might run for a millisecond.
// On a device where fragmentation has been a hard stop that is the wrong shape
// — not because the allocator is slow, but because a repeating fifteen-kilobyte
// cycle is exactly what leaves a heap with plenty free and nowhere to put
// anything.
//
// So they are kept against the task instead. A shell running `calc` twenty
// times allocates once. A package task spawned with fw_task_spawn keeps its
// pair for its lifetime. The blocks come back when the task ends, which is what
// task_slot_recycled is for.
//
// The ARENA is still reset between calls even though the memory is not
// released. A pointer from one call was never valid in the next — the whole
// allocation used to be freed — and keeping the contents would quietly change
// that into something packages could come to rely on.
#define SANDBOX_POOL 4

// Keyed by TASK SLOT, not by pid.
//
// task_slot_recycled — the hook that says an owner has gone — is given the slot
// index, and by the time it runs the task is already dismantled, so there is no
// pid left to translate. Keying on the same thing the hook reports is the only
// version where the two cannot disagree.
//
// `used` rather than a zero-means-free slot number, because slot 0 is a real
// slot and this table is zero-initialised.
struct SandboxSlot {
    bool         used;
    int          slot;
    bool         lent;       // currently inside a call
    SandboxAlloc sa;
};
static SandboxSlot g_pool[SANDBOX_POOL];

// EVERY read-then-write of the table above is atomic across cores.
//
// `stress` runs three filesystem workers with AFFINITY_ANY, so two cores are
// genuinely inside these functions at the same instant — and "is this entry
// free?" followed by "then it is mine" is the classic pair that has to be one
// step. Split, two cores both saw a free entry and both claimed it: the first
// to finish cleared `lent` on a block the second was still running on, and
// apps_pool_reclaim was then free to hand that stack and arena back to the
// heap while a package was standing on them.
//
// lock_hw_enter is the right lock here and the only one that is: it is brief,
// it does not yield, and it excludes the other core rather than only other
// tasks. It is NOT recursive and it masks interrupts, so nothing below holds it
// across malloc, free or a heap query — the blocks are collected under the
// lock and released after it.
#define POOL_LOCK   lock_hw_enter()
#define POOL_UNLOCK lock_hw_exit()

static bool sandbox_alloc(SandboxAlloc *sa, TaskAppMem *m, uint32_t stack_bytes) {
    memset(sa, 0, sizeof(*sa));
    MpuBlockPlan sp, ap;
    if (stack_bytes < PKG_STACK_BYTES) stack_bytes = PKG_STACK_BYTES;
    // The reserve is added HERE rather than at each caller, so there is one
    // place that knows the firmware shares this stack.
    stack_bytes += FW_CALL_RESERVE;
    if (!mpu_v8_plan_block(stack_bytes, &sp)) return false;
    if (!mpu_v8_plan_block(PKG_ARENA_BYTES, &ap)) return false;

    sa->stack_raw = malloc(sp.alloc_bytes);
    sa->arena_raw = malloc(ap.alloc_bytes);
    if (!sa->stack_raw || !sa->arena_raw) {
        free(sa->stack_raw); free(sa->arena_raw);
        memset(sa, 0, sizeof(*sa));
        return false;
    }
    sa->stack = (void *)(uintptr_t)mpu_align_up((uint32_t)(uintptr_t)sa->stack_raw, sp.align);
    // Painted so the deepest the stack ever got can be read back afterwards.
    // Guessing at this size twice was enough.
    memset(sa->stack, STACK_PAINT, sp.region_bytes);
    sa->stack_bytes = sp.region_bytes;
    sa->arena_mem = (void *)(uintptr_t)mpu_align_up((uint32_t)(uintptr_t)sa->arena_raw, ap.align);
    arena_init(&sa->arena, sa->arena_mem, ap.region_bytes);

    m->stack      = sa->stack;
    m->stack_size = sp.region_bytes;
    m->arena      = sa->arena_mem;
    m->arena_size = ap.region_bytes;
    return true;
}

static void sandbox_free(SandboxAlloc *sa) {
    free(sa->stack_raw);
    free(sa->arena_raw);
    memset(sa, 0, sizeof(*sa));
}

// Fill `m` from a slot that is already allocated.
static void sandbox_describe_from(const SandboxAlloc *sa, TaskAppMem *m) {
    MpuBlockPlan sp, ap;
    // Must match what sandbox_alloc planned for a pooled block, reserve and all
    // — a region shorter than the allocation would put the end of the stack
    // outside it, which is the fault this reserve exists to prevent.
    mpu_v8_plan_block(PKG_STACK_TOTAL, &sp);
    mpu_v8_plan_block(PKG_ARENA_BYTES, &ap);
    m->stack      = sa->stack;
    m->stack_size = sp.region_bytes;
    m->arena      = sa->arena_mem;
    m->arena_size = ap.region_bytes;
}

// Borrow this task's stack and heap, allocating them the first time.
//
// `pooled` says whether the caller should give them back or free them: a task
// that arrives when every slot is taken still gets a sandbox, just a transient
// one. Refusing to sandbox because a table is full would be the wrong trade —
// the protection matters more than the churn it was costing.
uint32_t apps_pool_reclaim(void);

// `stack_bytes` is what the caller needs, which is NOT always the default.
//
// A package that spawns a task asks for a stack size, and it asks for a reason:
// stress gives its filesystem workers four kilobytes because littlefs needs
// them. Handing that task the pooled three-kilobyte stack instead overflowed it
// — the task had asked for what it needed and been given less, which is a
// stack overflow with a completely innocent-looking cause.
//
// So a request for anything other than the default is served with its own
// allocation and never pooled. Pooling exists to stop a shell running the same
// command repeatedly from cycling fifteen kilobytes; a task is created once, so
// there is nothing there to save.
static bool sandbox_acquire(SandboxAlloc *sa, TaskAppMem *m, bool *pooled,
                            uint32_t stack_bytes) {
    int slot = task_slot_index();
    *pooled = false;

    // Anything but the default size bypasses the pool entirely.
    if (stack_bytes > PKG_STACK_BYTES) return sandbox_alloc(sa, m, stack_bytes);

    // No slot means no owner to hold anything against — boot, or a context the
    // scheduler does not know. Transient, as it always was.
    if (slot < 0) return sandbox_alloc(sa, m, PKG_STACK_BYTES);

    // This task's own entry, if it has one. The test and the claim are one
    // step: two cores reaching the same entry together must not both take it.
    bool mine = false;
    POOL_LOCK;
    for (int i = 0; i < SANDBOX_POOL; i++) {
        if (!g_pool[i].used || g_pool[i].slot != slot || g_pool[i].lent) continue;
        g_pool[i].lent = true;
        *sa = g_pool[i].sa;
        mine = true;
        break;
    }
    POOL_UNLOCK;
    if (mine) {
        sandbox_describe_from(sa, m);
        // Fresh arena, same memory. See the note above. Only the caller's copy
        // needs it: the pool holds the BLOCKS, and the next acquire initialises
        // the arena again from them, so there is nothing to write back.
        arena_init(&sa->arena, sa->arena_mem, m->arena_size);
        *pooled = true;
        return true;
    }

    if (!sandbox_alloc(sa, m, PKG_STACK_BYTES)) {
        // Out of memory with idle slots still held is the one case the pool
        // must never cause. Give them all back and try once more before
        // reporting failure, since failing here means running a package
        // unprotected.
        if (!apps_pool_reclaim()) return false;
        if (!sandbox_alloc(sa, m, PKG_STACK_BYTES)) return false;
    }

    POOL_LOCK;
    for (int i = 0; i < SANDBOX_POOL; i++) {
        if (g_pool[i].used) continue;
        g_pool[i].used = true;
        g_pool[i].slot = slot;
        g_pool[i].lent = true;
        g_pool[i].sa = *sa;
        *pooled = true;
        break;
    }
    POOL_UNLOCK;
    // No free entry: the sandbox is real, just transient. `pooled` stays false
    // so the caller frees it rather than looking for a slot that never existed.
    return true;
}

// Below this share of the heap, the pool stops being worth having.
//
// Keeping a package's stack and heap between calls is a LUXURY: it saves a
// fifteen-kilobyte allocation cycle, which is worth having when there is room
// and worth nothing at all when there is not. So it evaporates under pressure
// rather than needing anyone to ask — a cache that has to be managed by hand is
// a cache that is still full when it matters.
//
// A share rather than a fixed number, because the part this has to survive on
// next has half the memory and a fixed threshold tuned here would be either
// meaningless or fatal there.
#define POOL_KEEP_ABOVE_FRACTION 4      // keep only while a quarter of the heap is free

static bool memory_is_tight(void) {
    uint32_t total = heap_total();
    return total && heap_free() < total / POOL_KEEP_ABOVE_FRACTION;
}

// Give back every slot nobody is inside. Returns the bytes released.
uint32_t apps_pool_reclaim(void) {
    uint32_t before = heap_free();
    // One at a time: the entry is taken out of the table under the lock and
    // freed outside it, so the allocator is never called with interrupts masked
    // and the other core can never find a half-released entry.
    for (;;) {
        SandboxAlloc dead{};
        bool got = false;
        POOL_LOCK;
        for (int i = 0; i < SANDBOX_POOL; i++) {
            if (!g_pool[i].used || g_pool[i].lent) continue;
            dead = g_pool[i].sa;
            memset(&g_pool[i], 0, sizeof(g_pool[i]));
            got = true;
            break;
        }
        POOL_UNLOCK;
        if (!got) break;
        sandbox_free(&dead);
    }
    uint32_t after = heap_free();
    return after > before ? after - before : 0;
}

// The deepest any package stack has been, across every one seen so far.
//
// Read by scanning up from the base for the first byte the paint no longer
// covers. It is the number that says whether FW_CALL_RESERVE is right, and it
// is why that constant is no longer a guess.
static uint32_t g_stack_peak;
static uint32_t g_stack_size;

static void note_stack_peak(const SandboxAlloc *sa) {
    if (!sa->stack || !sa->stack_bytes) return;
    const uint8_t *p = (const uint8_t *)sa->stack;
    uint32_t untouched = 0;
    while (untouched < sa->stack_bytes && p[untouched] == STACK_PAINT) untouched++;
    uint32_t used = sa->stack_bytes - untouched;
    if (used > g_stack_peak) { g_stack_peak = used; g_stack_size = sa->stack_bytes; }
}

void apps_stack_peak(uint32_t *used, uint32_t *size) {
    if (used) *used = g_stack_peak;
    if (size) *size = g_stack_size;
}

// Start the measurement again.
//
// The peak is a high-water mark across every package that has run, so ONE
// package that deliberately exhausts its stack sets it for the rest of the
// uptime — and `mpu` then reports that the reserve is too small on a device
// where nothing is wrong. Being able to clear it is what makes the number a
// measurement rather than a memory of the worst thing ever attempted.
void apps_stack_peak_reset(void) {
    g_stack_peak = 0;
    g_stack_size = 0;
}

static void sandbox_return(SandboxAlloc *sa, bool pooled) {
    note_stack_peak(sa);
    if (!pooled) { sandbox_free(sa); return; }
    // Held only while there is room to spare. Under pressure the block goes
    // straight back, which costs the next call an allocation and is exactly the
    // right trade at that point. Asked before the lock, because it reads the
    // heap and the lock is for the table only.
    bool tight = memory_is_tight();

    SandboxAlloc dead{};
    bool found = false, release = false;
    POOL_LOCK;
    for (int i = 0; i < SANDBOX_POOL; i++) {
        if (!g_pool[i].lent || g_pool[i].sa.stack_raw != sa->stack_raw) continue;
        found = true;
        g_pool[i].lent = false;
        if (tight) {
            dead = g_pool[i].sa;
            release = true;
            memset(&g_pool[i], 0, sizeof(g_pool[i]));
        }
        break;
    }
    POOL_UNLOCK;

    if (release)     sandbox_free(&dead);
    else if (!found) sandbox_free(sa);      // lent from no slot: transient after all
}

// The scheduler is reusing a task slot, so whatever it held is nobody's now.
//
// Without this the pool would hand a dead task's stack to whatever pid landed
// in the same slot next, and hold the memory for the life of the device.
extern "C" void apps_task_ended(int slot) {
    for (;;) {
        SandboxAlloc dead{};
        bool got = false, release = false;
        POOL_LOCK;
        for (int i = 0; i < SANDBOX_POOL; i++) {
            if (!g_pool[i].used || g_pool[i].slot != slot) continue;
            // Still lent means the task died inside a call. The memory cannot
            // be freed from here — the call may still be unwinding on another
            // core — so the slot is disowned and the block leaks rather than
            // being handed to somebody still using it. Rare, and the safe
            // direction.
            if (!g_pool[i].lent) { dead = g_pool[i].sa; release = true; }
            memset(&g_pool[i], 0, sizeof(g_pool[i]));
            got = true;
            break;
        }
        POOL_UNLOCK;
        if (!got) break;
        if (release) sandbox_free(&dead);
    }
}

// How much the pool is holding, for meminfo to report.
uint32_t apps_pool_bytes(void) {
    MpuBlockPlan sp, ap;
    if (!mpu_v8_plan_block(PKG_STACK_TOTAL, &sp)) return 0;
    if (!mpu_v8_plan_block(PKG_ARENA_BYTES, &ap)) return 0;
    uint32_t n = 0;
    for (int i = 0; i < SANDBOX_POOL; i++) if (g_pool[i].used) n++;
    return n * (sp.alloc_bytes + ap.alloc_bytes);
}

// Copy an argument vector into the package's own memory.
//
// A registered command is called with (argc, argv), and argv points into the
// SHELL's memory — its line buffer and the pointer array beside it. A sandboxed
// package cannot read that. Not "should not": the regions describe its own five
// blocks and unprivileged code gets no default memory map, so the first look at
// argv[1] is a data access violation inside the package, at whatever line
// happens to touch it first.
//
// That is why `bench` and `stress` ran and `calc 2 ^ 3 ^ 2` did not — the two
// that were given no arguments never read the vector.
//
// So the vector is rebuilt at the top of the package's own stack: the strings
// first, growing down, then the array of pointers to them. The package's stack
// top moves below all of it, which is also what keeps it from being overwritten
// the moment the package pushes a frame.
//
// Returns the new argv, or null if the arguments do not fit — a command with
// more text than its stack can hold is refused rather than truncated, because
// a package acting on half its arguments is worse than one that did not run.
#define PKG_ARGV_MAX      32
#define PKG_ARGV_RESERVE  512      // never eat more than this of the stack

static char **marshal_argv(void *stack_base, uint32_t *stack_top_inout,
                           int argc, char **argv) {
    if (argc < 0 || argc > PKG_ARGV_MAX) return nullptr;
    uint8_t *floor = (uint8_t *)stack_base + *stack_top_inout - PKG_ARGV_RESERVE;
    uint8_t *p = (uint8_t *)stack_base + *stack_top_inout;

    char *copied[PKG_ARGV_MAX];
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t n = argv[i] ? (uint32_t)strlen(argv[i]) + 1u : 1u;
        if ((uint32_t)(p - floor) < n) return nullptr;
        p -= n;
        if (argv[i]) memcpy(p, argv[i], n);
        else         p[0] = 0;
        copied[i] = (char *)p;
    }

    // The pointer array, below the strings and eight-byte aligned so the stack
    // that starts under it is aligned too.
    p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)7);
    uint32_t need = (uint32_t)(argc + 1) * sizeof(char *);
    if ((uint32_t)(p - floor) < need) return nullptr;
    p -= need;
    p = (uint8_t *)((uintptr_t)p & ~(uintptr_t)7);
    char **out = (char **)p;
    for (int i = 0; i < argc; i++) out[i] = copied[i];
    out[argc] = nullptr;

    *stack_top_inout = (uint32_t)(p - (uint8_t *)stack_base);
    return out;
}

// Reachable from the host test, which is the only place the boundary arithmetic
// above can be exercised — on a board it either works or produces a fault
// somewhere inside a package.
char **marshal_argv_for_test(void *base, uint32_t *top, int argc, char **argv) {
    return marshal_argv(base, top, argc, argv);
}

void app_enter(const LoadedApp *app, TaskAppMem *saved, bool *had_saved) {
    *had_saved = task_app_mem_get(saved);
    if (!app) return;
    TaskAppMem m;
    describe(app, &m);
    task_app_mem_set(&m);
}

void app_leave(const TaskAppMem *saved, bool had_saved) {
    if (had_saved) task_app_mem_set(saved);
    else           task_app_mem_clear();
}

// Run package code, sandboxed if this board can and privileged if it cannot.
//
// The fallback is deliberate and is not silent: `apps` and `mpu` both say which
// a package got. An RP2040 cannot afford the regions, and refusing to run
// packages there would be a worse answer than running them the way they have
// always run.
// `require_sandbox` is true for a task a PACKAGE spawned.
//
// The foreground command keeps its fallback: somebody typed it, the warning is
// on their screen, and refusing to run a package on a device that is merely
// short of memory would be the worse trade.
//
// A spawned task is different. It exists because the package asked for a second
// thread, and the whole reason apps_spawn_in_sandbox exists is that a task
// started any other way runs privileged with no regions registered — which is
// how a package used to escape its sandbox simply by asking for a thread.
// Letting it run unsandboxed when the pool is empty re-opens exactly that, and
// nobody is watching to read the warning.
int app_run_stack(const LoadedApp *app, int (*fn)(int), int arg,
                  uint32_t stack_bytes, bool require_sandbox) {
    TaskAppMem saved;
    bool had_saved = false;
    TaskAppMem m;
    describe(app, &m);

    SandboxAlloc sa;
    bool pooled = false;
    bool boxed = sandbox_supported() && app->veneer_gates &&
                 sandbox_acquire(&sa, &m, &pooled, stack_bytes);
    // A board that cannot sandbox is a fact about the board. A board that can
    // and did not is an event, and a silent one would be the worst kind: the
    // package runs with the OS's own privileges and nothing anywhere says the
    // protection it was supposed to have did not happen.
    if (sandbox_supported() && app->veneer_gates && !boxed) {
        if (require_sandbox) {
            out_errp("apps", "'%s' asked for a task and there was no memory to "
                             "sandbox it, so it was not started.",
                     app->header.name);
            app_leave(&saved, had_saved);
            return -1;
        }
        out_warnp("apps", "Not enough memory to sandbox '%s' — it is running "
                          "with full privileges.", app->header.name);
    }

    had_saved = task_app_mem_get(&saved);
    task_app_mem_set(&m);
    if (boxed) task_arena_set(&sa.arena);

    int ret;
    if (boxed) {
        ret = sandbox_enter((void *)fn, arg, nullptr,
                            (uint8_t *)sa.stack + m.stack_size,
                            app_return_gate(app), app_enter_gate(app),
                            app_exit_gate(app), m.stack_size);
    } else {
        ret = fn(arg);
    }

    if (boxed) { task_arena_set(nullptr); sandbox_return(&sa, pooled); }
    bb_note_phase("apps: sandbox returned");
    app_leave(&saved, had_saved);
    // The alarm cannot print — it fires inside an arbitrary instruction — so it
    // leaves a flag and this says what happened, back in task context. A fault
    // handler cannot print either, for a different reason, and leaves its
    // report the same way.
    if (sandbox_took_call_back()) {
        fault_report_contained();
        out_errp("apps", "'%s' was stopped. The shell is fine.",
                 app->header.name);
    }
    return ret;
}

int app_run(const LoadedApp *app, int (*fn)(int), int arg) {
    return app_run_stack(app, fn, arg, PKG_STACK_BYTES, /*require_sandbox*/false);
}

// --- tasks a package spawns --------------------------------------------------
//
// A package that called fw_task_spawn used to escape its own sandbox, and it
// did not have to try: task_spawn starts every task with app_mem_set false, so
// the new task ran with the OS's own privileges AND with no regions registered
// — which meant every pointer it handed the ABI passed unchecked as well,
// because the checks read the calling task's regions and there were none.
//
// So a sandboxed package got out of the sandbox by asking for a second thread.
//
// The task now starts in a shim that re-enters the package's sandbox first: its
// own stack, its own arena, the same code and data regions, unprivileged. The
// entry costs one table slot and one extra frame, and it is the difference
// between the sandbox being a property of the package and a property of
// whichever call happened to enter it.
struct PkgTask {
    bool     used;
    void    *image;        // which package it belongs to
    int    (*fn)(int);
    int      arg;
    uint32_t stack;        // what the package asked for, and must actually get
};
static PkgTask g_pkg_tasks[8];

// Run fn(arg) inside the sandbox of the package that owns `image`.
static bool run_in_image(void *image, int (*fn)(int), int arg, uint32_t stack, int *ret) {
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i] || g_apps[i].image != image) continue;
        *ret = app_run_stack(&g_apps[i], fn, arg, stack, /*require_sandbox*/true);
        return true;
    }
    return false;
}

static int pkg_task_shim(void *v) {
    PkgTask *e = (PkgTask *)v;
    int r = 0;
    if (!run_in_image(e->image, e->fn, e->arg, e->stack, &r)) {
        // The package was unloaded between the spawn and this task getting a
        // turn. Running its code now would be running code the heap has taken
        // back, so the task simply ends.
        r = -1;
    }
    e->used = false;
    return r;
}

// Spawn a task that will run package code, inside that package's sandbox.
//
// Returns -1 when the caller is not inside a package, so fw_task_spawn can fall
// back to an ordinary task — the shell and the OS's own code spawn tasks too.
extern "C" int apps_spawn_in_sandbox(const char *name, int (*fn)(int), void *arg,
                                     uint32_t stack) {
    TaskAppMem m;
    if (!task_app_mem_get(&m) || !m.text) return -1;

    for (unsigned i = 0; i < sizeof(g_pkg_tasks) / sizeof(g_pkg_tasks[0]); i++) {
        if (g_pkg_tasks[i].used) continue;
        g_pkg_tasks[i].used  = true;
        g_pkg_tasks[i].image = (void *)m.text;
        g_pkg_tasks[i].fn    = fn;
        g_pkg_tasks[i].arg   = (int)(uintptr_t)arg;
        // The SANDBOX stack is what the package asked for, and it asked for a
        // reason — stress gives its filesystem workers four kilobytes because
        // littlefs needs them.
        uint32_t want = stack ? stack : PKG_STACK_BYTES;
        g_pkg_tasks[i].stack = want;

        // The TASK stack depends on whether the sandbox will actually be set
        // up. When it is, the package runs on the sandbox stack and this one
        // only carries the shim — find the image, call app_run_stack — so the
        // minimum is ample. When it is not (ARMv6-M, or an allocation that
        // fails), app_run calls the package function directly on THIS stack,
        // and it had better be the size the package asked for.
        //
        // Getting that backwards is a stack overflow whose cause looks like
        // anything but a stack size, which is how the first version of this
        // went wrong.
        uint32_t task_stack = sandbox_supported() ? TASK_STACK_MIN : want;
        if (task_stack < TASK_STACK_MIN) task_stack = TASK_STACK_MIN;

        int pid = task_spawn(name, "(package)", pkg_task_shim, &g_pkg_tasks[i],
                             task_stack, AFFINITY_ANY);
        if (pid < 0) g_pkg_tasks[i].used = false;
        return pid;
    }
    return -1;      // no slot: the caller reports the failure rather than
                    // quietly spawning something unprotected
}

bool app_run_owner(const void *owner, int (*fn)(int, char **), int argc,
                   char **argv, int *ret) {
    if (!owner) return false;
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i] || g_apps[i].image != owner) continue;

        // Said HERE rather than left to sandbox_enter, because by the time that
        // refuses, the message has to be reconstructed from a return code and
        // this is the only place that knows which command was being asked for.
        //
        // The route in is fw_shell_run: a package command can run a shell
        // command, and until this check the shell would dispatch straight back
        // into a package on the same task — overwriting the outer call's way
        // home. It killed the device on `novad1 service restart`.
        if (sandbox_in_package()) {
            out_errp("apps", "'%s' cannot run while another package command is "
                             "already running on this task.", g_apps[i].header.name);
            out_multi("  A package that needs one runs it in a task of its own.");
            *ret = 1;
            return true;
        }
        // A registered command takes two arguments where app_main takes one, so
        // the shim carries both. Reinterpreting the pointer is safe because the
        // call is made in assembly from registers rather than through this type:
        // r0 and r1 are argc and argv under AAPCS either way.
        LoadedApp *a = &g_apps[i];
        TaskAppMem saved, m;
        bool had_saved;
        describe(a, &m);
        SandboxAlloc sa;
        bool pooled = false;
        bool boxed = sandbox_supported() && a->veneer_gates &&
                     sandbox_acquire(&sa, &m, &pooled, PKG_STACK_BYTES);
        if (sandbox_supported() && a->veneer_gates && !boxed)
            out_warnp("apps", "Not enough memory to sandbox '%s' — it is running "
                              "with full privileges.", a->header.name);
        had_saved = task_app_mem_get(&saved);
        task_app_mem_set(&m);
        if (boxed) task_arena_set(&sa.arena);
        if (boxed) {
            uint32_t top = m.stack_size;
            char **boxed_argv = marshal_argv(sa.stack, &top, argc, argv);
            if (!boxed_argv) {
                out_errp("apps", "'%s' was given more arguments than its stack "
                                 "can hold.", a->header.name);
                *ret = 1;
            } else {
                *ret = sandbox_enter((void *)fn, argc, boxed_argv,
                                     (uint8_t *)sa.stack + top,
                                     app_return_gate(a), app_enter_gate(a),
                                     app_exit_gate(a), top);
            }
        } else {
            *ret = fn(argc, argv);
        }
        // CHECKPOINTS, because this is the path a package COMMAND takes and it
        // had none. `havoc fault` is a registered command, so it comes through
        // here rather than through app_run_stack — and a fault contained inside
        // it left a report that could not say whether the unwind had finished,
        // because the only marker after sandbox_enter was in the other function.
        bb_note_phase("apps: the command returned");
        if (boxed) { task_arena_set(nullptr); sandbox_return(&sa, pooled); }
        bb_note_phase("apps: sandbox released");
        app_leave(&saved, had_saved);
        bb_note_phase("apps: left the package");
        if (sandbox_took_call_back()) {
            // The handler recorded and returned; this is where it gets read
            // out. Nothing was printed inside the fault itself, on purpose —
            // see the note on fault_report_contained.
            fault_report_contained();
            out_errp("apps", "'%s' was stopped. The shell is fine.",
                     a->header.name);
        }
        return true;
    }
    return false;
}

bool app_enter_owner(const void *owner, TaskAppMem *saved, bool *had_saved) {
    *had_saved = false;
    if (!owner) return false;                  // a built-in: nothing to protect
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i] || g_apps[i].image != owner) continue;
        app_enter(&g_apps[i], saved, had_saved);
        return true;
    }
    return false;
}

LoadedApp *apps_store(const LoadedApp *app) {
    if (find(app->header.name)) return nullptr;      // already resident
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i]) {
            g_apps[i] = *app;      // image/veneer pointers copy over and stay valid
            g_used[i] = true;
            return &g_apps[i];
        }
    }
    return nullptr;
}

bool apps_resident(const char *name) {
    return name && find(name) != nullptr;
}

bool apps_unload(const char *name) {
    LoadedApp *a = find(name);
    if (!a) return false;
    // Nobody may be INSIDE it.
    //
    // A task parks on every ABI call a package makes, so the shell can sit
    // halfway through a package's command indefinitely while something else —
    // a background job running `unload`, a second session — removes it. Freeing
    // the image there leaves that task about to resume into code the heap has
    // taken back, and it will hand the same address to the next malloc.
    //
    // Refusing is the whole fix, and it is a better answer than unloading
    // carefully would be: the package's code is genuinely still executing, and
    // no amount of tidying up the protection registers makes it safe to free.
    if (task_app_mem_holder(a->image) >= 0) return false;
    // Order matters: drop the commands FIRST (while the code they point into is
    // still mapped), then free the image. The reverse would leave the registry
    // holding function pointers into freed memory for the moment between.
    cmd_remove_owner(a->image);
    app_unload(a);
    for (int i = 0; i < APPS_MAX; i++) if (&g_apps[i] == a) { g_used[i] = false; break; }
    return true;
}

int apps_busy_pid(const char *name) {
    LoadedApp *a = find(name);
    return a ? task_app_mem_holder(a->image) : -1;
}

int apps_launch(const char *file, int arg, bool quiet) {
    AppSource src;
    void *handle = nullptr;
    if (!storage_open_source(file, &src, &handle)) {
        if (!quiet) out_err("No such app: %s", file);
        return -1;
    }
    uint32_t before = heap_free();
    LoadedApp app;
    LoadResult rc = app_load(src, &app);
    storage_close_source(handle);
    if (rc != LOAD_OK) {
        if (!quiet) out_err("Load failed: %s%s%s", load_result_str(rc),
                            app.detail[0] ? " - " : "", app.detail);
        return -1;
    }

    // A package that registers nothing usually just finished its work. One that
    // TRIED and was refused is a different thing entirely, and used to be
    // invisible: pkg reported it installed, the command did not exist, and
    // nothing anywhere connected the two. The refusal counter is what tells
    // them apart.
    uint32_t refused_before = cmd_refused();

    api_set_current_app(app.image);
    g_current_app = app.header.name;
    // The first jump into loaded code. If a crash report stops here the loader
    // produced something that does not execute, which is a very different
    // problem from a package with a bug in it.
    //
    // The protection goes on immediately before that jump and comes off
    // immediately after, and not one line wider. Outside this window the image
    // is ordinary heap that the loader wrote and app_unload is about to hand
    // back — and free() writes its own bookkeeping into a block it is
    // reclaiming, which a read-only region would fault on.
    bb_note_phase("entering app_main");
    int ret = app_run(&app, app.entry, arg);
    bb_note_phase("app_main returned");
    g_current_app = nullptr;
    api_set_current_app(nullptr);

    // Resident iff it registered a command owned by its image.
    bool resident = false;
    for (uint32_t i = 0; i < cmd_count(); i++)
        if (cmd_at(i)->owner == app.image) { resident = true; break; }

    if (resident) {
        if (apps_store(&app)) {
            if (!quiet) out_ok("Package '%s' loaded.", app.header.name);
        } else {
            // Table full or already loaded: pull the commands back rather than
            // orphan them, and unload.
            cmd_remove_owner(app.image);
            app_unload(&app);
            if (!quiet) out_err("'%s' could not stay resident.", app.header.name);
        }
    } else if (cmd_refused() != refused_before) {
        // It asked for a command and did not get one. TWO different reasons,
        // and they used to share one message that guessed at the table being
        // full — which sent people looking for a limit they were nowhere near
        // when a name was simply already registered.
        bool full = cmd_full();
        app_unload(&app);
        out_errp("apps", "'%s' could not register its command%s.",
                 app.header.name,
                 cmd_refused() - refused_before > 1 ? "s" : "");
        if (full)
            out_multi("  The command table is full (%d).", CMD_MAX);
        else
            out_multi("  A name it wanted is already taken — most likely by a "
                      "copy of itself that is still loaded.");
        return -1;
    } else {
        app_unload(&app);
        if (!quiet) {
            uint32_t after = heap_free();
            if (after == before) out_ok("'%s' finished  (exit %d).", app.header.name, ret);
            else out_warn("'%s' finished (exit %d) but did not release %u bytes.",
                          app.header.name, ret, (unsigned)(before - after));
        }
    }
    return ret;
}

// Find the package a raw address belongs to. A fault report giving only an
// absolute SRAM address is nearly useless — the image lands wherever malloc put
// it, so the number differs every boot. An offset into a named package can be
// looked up directly in the .app.
extern "C" const char *apps_locate(uint32_t addr, uint32_t *offset, bool *in_veneer) {
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i]) continue;
        uint32_t base = (uint32_t)(uintptr_t)g_apps[i].image;
        if (addr >= base && addr < base + g_apps[i].image_size) {
            if (offset)    *offset = addr - base;
            if (in_veneer) *in_veneer = false;
            return g_apps[i].header.name;
        }
        uint32_t vbase = (uint32_t)(uintptr_t)g_apps[i].veneers;
        if (addr >= vbase && addr < vbase + g_apps[i].veneer_size) {
            if (offset)    *offset = addr - vbase;
            if (in_veneer) *in_veneer = true;
            return g_apps[i].header.name;
        }
    }
    return nullptr;
}

static int cmd_apps(int, char **) {
    int n = 0;
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i]) continue;
        out_multi("  %s%-16s%s %u B", C_CYAN, g_apps[i].header.name, C_RESET,
                  (unsigned)g_apps[i].bytes_allocated);
        n++;
    }
    if (!n) out_multi("  (no packages loaded)");
    return 0;
}

static int cmd_unload(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: unload <package>"); return 1; }
    // Two ways to fail, and they need different answers. "Not loaded" is a typo;
    // "in use" is a package another task is executing right now, and saying it
    // was not loaded would send someone looking for a name that is right there
    // in `apps`.
    int busy = apps_busy_pid(argv[1]);
    if (busy >= 0) {
        out_err("'%s' is running right now (task %d).", argv[1], busy);
        out_multi("  Let it finish, or stop it with 'kill %d'.", busy);
        return 1;
    }
    if (!apps_unload(argv[1])) { out_err("Not loaded: %s", argv[1]); return 1; }
    out_ok("Unloaded %s.", argv[1]);
    return 0;
}

void apps_register(void) {
    static const Command cmds[] = {
        {"apps",   "list resident packages", cmd_apps,   nullptr},
        {"unload", "unload a package",        cmd_unload, nullptr, LEVEL_ADMIN},
    };
    for (const auto &c : cmds) cmd_register(&c);
}

// --- what the fault handler asks --------------------------------------------
//
// A stacking fault says the exception frame would not fit and nothing else.
// WHICH stack it would not fit in is the whole question, and reasoning it out
// from an address and a fault code got it wrong twice — so this answers it from
// the same table the protection unit is programmed from.
//
// Printed at the point of the crash rather than logged, because the device
// reboots two seconds later and a log line about a stack that no longer exists
// is worth less than one naming it while it does.
extern "C" void mpu_dump_live(unsigned sp);

// Was this fault the PACKAGE's, and can it be unwound?
//
// Called from the fault handler once it has printed its report. Returning
// non-zero means the frame has been rewritten so the exception return lands in
// the shim's tail — the same unwind `svc #1` performs when a package returns
// normally — and the device carries on instead of resetting.
//
// The test is: a package is running, and the faulting stack pointer is inside
// the sandbox stack that package was given. That is precise for the case this
// is for, a package running off the end of its own stack, and it is the only
// thing available for a STACKING fault — when the frame could not be pushed
// there is no stacked PC to look at, so the usual "was the program counter in
// package code" question cannot be asked.
//
// WHAT IT DOES NOT KNOW: firmware called through the ABI also runs on that
// stack, so a fault inside an ABI call is contained as though it were the
// package's. The package asked for the call, the report names it either way,
// and a lock the firmware held at that moment is not released — so a contained
// fault can in principle leave something stuck. That is still a better outcome
// than resetting a device with a working shell on it, and the watchdog remains
// underneath for the case where it is not.
extern "C" bool sandbox_abandon_call(uint32_t *frame);

extern "C" int fault_try_contain(uint32_t *frame) {
    // DECLINING IS SAID OUT LOUD. A reset that could have been contained and
    // was not is the interesting case, and it used to be indistinguishable
    // from one that was never containable. The report is already on screen by
    // the time this runs, so one more line costs nothing.
    if (!frame) return 0;
    if (!sandbox_in_package()) {
        log_addf(LOG_K_ERR, "not contained: no package was running");
        printf("    not contained: no package was running\n");
        return 0;
    }

    TaskAppMem m;
    if (!task_app_mem_get(&m) || !m.stack || !m.stack_size) {
        log_addf(LOG_K_ERR, "not contained: the package has no sandbox stack");
        printf("    not contained: the package has no sandbox stack\n");
        return 0;
    }

    uint32_t sp   = (uint32_t)(uintptr_t)frame;
    uint32_t base = (uint32_t)(uintptr_t)m.stack;
    if (sp < base || sp >= base + m.stack_size) {
        log_addf(LOG_K_ERR, "not contained: sp %08lx outside package stack "
                            "%08lx..%08lx", (unsigned long)sp,
                 (unsigned long)base, (unsigned long)(base + m.stack_size));
        printf("    not contained: sp 0x%08lx is outside the package stack "
               "0x%08lx..0x%08lx\n", (unsigned long)sp, (unsigned long)base,
               (unsigned long)(base + m.stack_size));
        return 0;
    }

    if (!sandbox_abandon_call(frame)) {
        log_addf(LOG_K_ERR, "not contained: the package was not inside a call");
        printf("    not contained: the package was not inside a call\n");
        return 0;
    }
    // WHY THE LOG RING AND NOT JUST printf: the console is the first thing lost
    // when a fault takes the device down, and the reason a fault could not be
    // contained is precisely what is wanted afterwards. logdump survives the
    // reset; the terminal does not.
    log_addf(LOG_K_WARN, "contained a fault in a package; the shell survived");
    // apps.cpp says which package once it is back in task context; see
    // sandbox_took_call_back below.
    return 1;
}

// `quiet` is a fault that is about to be CONTAINED: the device carries on, so
// everything here runs on a stack that is still in use and printing is exactly
// what must not happen. The two numbers go to the log ring and nothing else.
extern "C" void fault_report_stacks(uint32_t sp, int quiet) {
    TaskAppMem m;

    if (quiet) {
        if (!task_app_mem_get(&m) || !m.stack || !m.stack_size) return;
        uint32_t base = (uint32_t)(uintptr_t)m.stack;
        uint32_t top  = base + m.stack_size;
        const uint8_t *p = (const uint8_t *)(uintptr_t)base;
        uint32_t untouched = 0;
        while (untouched < m.stack_size && p[untouched] == STACK_PAINT) untouched++;
        log_addf(LOG_K_ERR,
                 "stack %08lx..%08lx sp %s, %lu below, deepest %lu of %lu",
                 (unsigned long)base, (unsigned long)top,
                 (sp >= base && sp < top) ? "inside" : "OUTSIDE",
                 (unsigned long)(sp >= base && sp < top ? sp - base : 0),
                 (unsigned long)(m.stack_size - untouched),
                 (unsigned long)m.stack_size);
        return;
    }

    // The hardware first, because what the MPU HELD is the question and
    // everything below is what it was supposed to hold.
    mpu_dump_live(sp);

    if (!task_app_mem_get(&m) || !m.stack || !m.stack_size) {
        printf("    stack: not inside a package (sp=0x%08lx)\n", (unsigned long)sp);
        return;
    }

    uint32_t base = (uint32_t)(uintptr_t)m.stack;
    uint32_t top  = base + m.stack_size;
    printf("    package stack : 0x%08lx .. 0x%08lx  (%lu bytes)\n",
           (unsigned long)base, (unsigned long)top, (unsigned long)m.stack_size);

    if (sp >= base && sp < top) {
        printf("    sp is INSIDE it, %lu bytes from the bottom\n",
               (unsigned long)(sp - base));
    } else {
        // The interesting answer. If the stack pointer was not in the region
        // the protection unit was told about, the fault is a mismatch between
        // the two rather than a package using too much.
        printf("    sp is OUTSIDE it — %s by %lu bytes\n",
               sp < base ? "below" : "above",
               (unsigned long)(sp < base ? base - sp : sp - top));
    }

    // How deep it ever got, from the paint. A figure close to the size means a
    // genuine overflow; one nowhere near it means the fault was something else
    // wearing an overflow's clothes.
    const uint8_t *p = (const uint8_t *)(uintptr_t)base;
    uint32_t untouched = 0;
    while (untouched < m.stack_size && p[untouched] == STACK_PAINT) untouched++;
    printf("    deepest used  : %lu of %lu bytes\n",
           (unsigned long)(m.stack_size - untouched), (unsigned long)m.stack_size);

    // AND INTO THE LOG RING, because this half is the half that gets read.
    //
    // Nothing printed above survives a reset. logdump does, and these two
    // numbers are what decides whether the handler had room to run: how far the
    // fault was from the bottom of the package's stack, and how deep anything
    // ever went.
    log_addf(LOG_K_ERR, "stack %08lx..%08lx sp %s, %lu below, deepest %lu of %lu",
             (unsigned long)base, (unsigned long)top,
             (sp >= base && sp < top) ? "inside" : "OUTSIDE",
             (unsigned long)(sp >= base && sp < top ? sp - base : 0),
             (unsigned long)(m.stack_size - untouched),
             (unsigned long)m.stack_size);
}
