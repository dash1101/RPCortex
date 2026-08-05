#include "apps.h"
#include "command.h"
#include "out.h"
#include "storage.h"
#include "kernel.h"
#include "blackbox.h"
#include "mpu.h"
#include "arena.h"
#include "sandbox.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Set around app_main so a registered command is tagged with its app (api.cpp)
// and a fault names the culprit (fault.cpp).
extern "C" void api_set_current_app(void *owner);
extern "C" volatile const char *g_current_app;

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

struct SandboxAlloc {
    void  *stack_raw;
    void  *stack;
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

static bool sandbox_alloc(SandboxAlloc *sa, TaskAppMem *m) {
    memset(sa, 0, sizeof(*sa));
    MpuBlockPlan sp, ap;
    if (!mpu_v8_plan_block(PKG_STACK_BYTES, &sp)) return false;
    if (!mpu_v8_plan_block(PKG_ARENA_BYTES, &ap)) return false;

    sa->stack_raw = malloc(sp.alloc_bytes);
    sa->arena_raw = malloc(ap.alloc_bytes);
    if (!sa->stack_raw || !sa->arena_raw) {
        free(sa->stack_raw); free(sa->arena_raw);
        memset(sa, 0, sizeof(*sa));
        return false;
    }
    sa->stack = (void *)(uintptr_t)mpu_align_up((uint32_t)(uintptr_t)sa->stack_raw, sp.align);
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
    mpu_v8_plan_block(PKG_STACK_BYTES, &sp);
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

static bool sandbox_acquire(SandboxAlloc *sa, TaskAppMem *m, bool *pooled) {
    int slot = task_slot_index();
    *pooled = false;
    // No slot means no owner to hold anything against — boot, or a context the
    // scheduler does not know. Transient, as it always was.
    if (slot < 0) return sandbox_alloc(sa, m);

    for (int i = 0; i < SANDBOX_POOL; i++) {
        if (!g_pool[i].used || g_pool[i].slot != slot || g_pool[i].lent) continue;
        g_pool[i].lent = true;
        *sa = g_pool[i].sa;
        sandbox_describe_from(sa, m);
        // Fresh arena, same memory. See the note above.
        arena_init(&sa->arena, sa->arena_mem, m->arena_size);
        g_pool[i].sa = *sa;
        *pooled = true;
        return true;
    }

    if (!sandbox_alloc(sa, m)) {
        // Out of memory with idle slots still held is the one case the pool
        // must never cause. Give them all back and try once more before
        // reporting failure, since failing here means running a package
        // unprotected.
        if (!apps_pool_reclaim()) return false;
        if (!sandbox_alloc(sa, m)) return false;
    }

    for (int i = 0; i < SANDBOX_POOL; i++) {
        if (g_pool[i].used) continue;
        g_pool[i].used = true;
        g_pool[i].slot = slot;
        g_pool[i].lent = true;
        g_pool[i].sa = *sa;
        *pooled = true;
        break;
    }
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
    for (int i = 0; i < SANDBOX_POOL; i++) {
        if (!g_pool[i].used || g_pool[i].lent) continue;
        sandbox_free(&g_pool[i].sa);
        memset(&g_pool[i], 0, sizeof(g_pool[i]));
    }
    uint32_t after = heap_free();
    return after > before ? after - before : 0;
}

static void sandbox_return(SandboxAlloc *sa, bool pooled) {
    if (!pooled) { sandbox_free(sa); return; }
    for (int i = 0; i < SANDBOX_POOL; i++) {
        if (!g_pool[i].lent || g_pool[i].sa.stack_raw != sa->stack_raw) continue;
        g_pool[i].lent = false;
        // Held only while there is room to spare. Under pressure the block goes
        // straight back, which costs the next call an allocation and is exactly
        // the right trade at that point.
        if (memory_is_tight()) {
            sandbox_free(&g_pool[i].sa);
            memset(&g_pool[i], 0, sizeof(g_pool[i]));
        }
        return;
    }
    // Lent from no slot: it was transient after all.
    sandbox_free(sa);
}

// The scheduler is reusing a task slot, so whatever it held is nobody's now.
//
// Without this the pool would hand a dead task's stack to whatever pid landed
// in the same slot next, and hold the memory for the life of the device.
extern "C" void apps_task_ended(int slot) {
    for (int i = 0; i < SANDBOX_POOL; i++) {
        if (!g_pool[i].used || g_pool[i].slot != slot) continue;
        // Still lent means the task died inside a call. The memory cannot be
        // freed from here — the call may still be unwinding on another core —
        // so the slot is disowned and the block leaks rather than being handed
        // to somebody still using it. Rare, and the safe direction.
        if (!g_pool[i].lent) sandbox_free(&g_pool[i].sa);
        memset(&g_pool[i], 0, sizeof(g_pool[i]));
    }
}

// How much the pool is holding, for meminfo to report.
uint32_t apps_pool_bytes(void) {
    MpuBlockPlan sp, ap;
    if (!mpu_v8_plan_block(PKG_STACK_BYTES, &sp)) return 0;
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
int app_run(const LoadedApp *app, int (*fn)(int), int arg) {
    TaskAppMem saved;
    bool had_saved = false;
    TaskAppMem m;
    describe(app, &m);

    SandboxAlloc sa;
    bool pooled = false;
    bool boxed = sandbox_supported() && app->veneer_gates &&
                 sandbox_acquire(&sa, &m, &pooled);
    // A board that cannot sandbox is a fact about the board. A board that can
    // and did not is an event, and a silent one would be the worst kind: the
    // package runs with the OS's own privileges and nothing anywhere says the
    // protection it was supposed to have did not happen.
    if (sandbox_supported() && app->veneer_gates && !boxed)
        out_warnp("apps", "Not enough memory to sandbox '%s' — it is running "
                          "with full privileges.", app->header.name);

    had_saved = task_app_mem_get(&saved);
    task_app_mem_set(&m);
    if (boxed) task_arena_set(&sa.arena);

    int ret;
    if (boxed) {
        ret = sandbox_enter((void *)fn, arg, nullptr,
                            (uint8_t *)sa.stack + m.stack_size,
                            app_return_gate(app), app_enter_gate(app),
                            app_exit_gate(app));
    } else {
        ret = fn(arg);
    }

    if (boxed) { task_arena_set(nullptr); sandbox_return(&sa, pooled); }
    app_leave(&saved, had_saved);
    return ret;
}

bool app_run_owner(const void *owner, int (*fn)(int, char **), int argc,
                   char **argv, int *ret) {
    if (!owner) return false;
    for (int i = 0; i < APPS_MAX; i++) {
        if (!g_used[i] || g_apps[i].image != owner) continue;
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
                     sandbox_acquire(&sa, &m, &pooled);
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
                                     app_exit_gate(a));
            }
        } else {
            *ret = fn(argc, argv);
        }
        if (boxed) { task_arena_set(nullptr); sandbox_return(&sa, pooled); }
        app_leave(&saved, had_saved);
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
        // It asked for a command and did not get one. Almost always the table
        // being full, which is a limit rather than a mistake in the package.
        app_unload(&app);
        out_errp("apps", "'%s' could not register its command%s.",
                 app.header.name,
                 cmd_refused() - refused_before > 1 ? "s" : "");
        out_multi("  The command table is full (%d), or the name is taken.", CMD_MAX);
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
