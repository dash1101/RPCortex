// The exported symbol table — the OS side of the app ABI.
//
// A flat array with a linear scan: lookup happens a handful of times per app at
// load time, so a sorted table and a binary search would trade flash for a
// saving nobody can measure. Every entry is a permanent commitment; adding one
// is a MINOR bump, changing one is MAJOR.

#include "api.h"
#include "rpc_app.h"
#include "pkgslot.h"   // pkgslot_crc32, for the ABI-table identity a slot records
#include "kernel.h"
#include "command.h"
#include "task.h"
#include "ptrcheck.h"
#include "storage.h"
#include "logring.h"
#include "blackbox.h"
#include "interrupt.h"
#include "out.h"        // out_capture_* for fw_shell_run
#include "registry.h"   // fw_reg_*
#include "persist.h"    // fw_reg_save
#if defined(RPC_HAS_SD) && RPC_HAS_SD
#include "sdcard.h"     // the "/sd" root fw_storage_roots offers
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"
#include "pico/rand.h"
#include "pico/unique_id.h"
#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI
#include "pico/cyw43_arch.h"    // WL_GPIO2 is the only route to VBUS sense here
#endif
#include "pico/aon_timer.h"
#include "sha256.h"
#include "framebuf.h"
#include "arena.h"
#include <time.h>
#include "hardware/clocks.h"

// The app currently being run, so a command it registers can be tagged with an
// owner and swept when the app unloads. Set by the shell around app_main. Same
// pattern as the fault handler's g_current_app.
static void *g_current_owner = nullptr;
extern "C" void api_set_current_app(void *owner) { g_current_owner = owner; }

// --- implementations -------------------------------------------------------

// Every one of these calls task_alive first.
//
// A package doing real work calls into the ABI constantly — printing, timing,
// allocating, touching files — so this is the liveness signal, and it costs the
// package nothing to provide. Without it, code that legitimately runs for a
// while without yielding is indistinguishable from a hang, and gets rebooted for
// doing its job.
// --- checking what a package points at --------------------------------------
//
// A sandboxed package cannot touch the OS's memory. It can ASK the OS to, and
// until these existed the OS did as it was told: every pointer argument below
// was a way to read or write anything on the machine from inside the sandbox,
// because the firmware dereferences them while privileged and the protection
// unit does not apply to privileged code.
//
// So each of them is checked against the five regions the package was actually
// given. Failing the check refuses the call rather than terminating the
// package: the read or write does not happen, which is the whole requirement,
// and a package that gets a failure back can report it. Killing the task from
// inside a supervisor call would be a larger and more delicate thing to get
// right for no extra safety.
//
// The refusals are counted and `mpu` prints the total. One is a bug in a
// package. A stream of them is a package doing it deliberately.
#define PKG_MEM() task_app_mem_current()

static inline bool ok_r(const void *p, uint32_t n) {
    if (ptr_ok(PKG_MEM(), p, n, PTR_READ)) return true;
    ptr_note_refusal();
    return false;
}
static inline bool ok_w(void *p, uint32_t n) {
    if (ptr_ok(PKG_MEM(), p, n, PTR_WRITE)) return true;
    ptr_note_refusal();
    return false;
}
static inline bool ok_s(const char *s) {
    if (ptr_str_ok(PKG_MEM(), s, nullptr)) return true;
    ptr_note_refusal();
    return false;
}

extern "C" int fw_printf(const char *fmt, ...) {
    // Reaching here at all proves the call from package code into the firmware
    // works — the veneer, the relocation and the symbol lookup. If a crash
    // report stops at "calling package 'x'" and never shows this, the fault is
    // in getting here, not in anything the package does.
    bb_note_phase("entered fw_printf");
    task_alive();
    // The format string only. What a %s in it points at cannot be checked from
    // here — the arguments are already on the stack in a form this cannot walk
    // without parsing the format itself, which is a second implementation of
    // printf and a second thing to get wrong. Noted in UNPRIV-DESIGN.md as the
    // one pointer the ABI still follows unchecked.
    if (!ok_s(fmt)) return -1;
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap); va_end(ap);
    return n;
}
extern "C" uint32_t fw_millis(void) { bb_note_phase("entered fw_millis"); task_alive(); return (uint32_t)(time_us_64() / 1000u); }
// A SANDBOXED package cannot be handed OS heap: it has no way to reach it, and
// the first write would fault. It gets its arena instead — one block, because
// there is one protection region to describe it with. A privileged package
// carries on using the ordinary heap, which is what every package did before
// sandboxing existed.
extern "C" void *fw_malloc(size_t n) {
    bb_note_phase("entered fw_malloc");
    task_alive();
    Arena *a = task_arena();
    if (a) {
        void *p = arena_alloc(a, (uint32_t)n);
        // Say so. A sandboxed package allocates from a block of its own, and
        // running that block out looks exactly like the OS being out of memory
        // — except `meminfo` will cheerfully report hundreds of kilobytes free.
        // Only when the request was plausible. A package deliberately asking
        // for 112 MB to check that failure is handled cleanly — which `stress`
        // does — is not a device running out of memory, and reporting it as one
        // trains people to ignore the message that matters. Nor is asking for
        // nothing: `malloc(0)` returning null is an answer, not a shortage, and
        // "ran out of its own heap asking for 0 bytes" said the opposite.
        if (!p && n && n <= arena_size(a))
            klog(LOG_WARN, "a package ran out of its own heap asking for %u bytes",
                 (unsigned)n);
        return p;
    }
    return malloc(n);
}
extern "C" void fw_free(void *p) {
    task_alive();
    Arena *a = task_arena();
    if (a) { arena_free(a, p); return; }
    free(p);
}

extern "C" void fw_log(int level, const char *msg) {
    if (!ok_s(msg)) return;
    LogLevel l = (level == 2) ? LOG_ERROR : (level == 1) ? LOG_WARN : LOG_INFO;
    klog(l, "%s", msg ? msg : "");
}

extern "C" int rpc_register_command(const char *name, const char *help,
                                    RpcCommandFn fn) {
    Command c{name, help ? help : "", (CommandFn)fn, g_current_owner};
    return cmd_register(&c) ? 1 : 0;
}

// --- API 1.3: tasks, files, memory ------------------------------------------
//
// Without these a package can print and allocate and nothing else, which is not
// enough to be a program. They are the smallest set that lets one run work in
// the background, keep state on disk, and see what memory it is using — added
// as a MINOR bump, so every existing package still loads.

extern "C" int apps_spawn_in_sandbox(const char *name, int (*fn)(int), void *arg,
                                     uint32_t stack);

extern "C" int fw_task_spawn(const char *name, TaskFn fn, void *arg, uint32_t stack) {
    if (!ok_s(name)) return -1;
    // A null entry point returned a live pid, and the task it made branched to
    // address zero as soon as the scheduler reached it — a fault in a task the
    // package spawned, blamed on the package, from a call that said it worked.
    // task_spawn refuses this; apps_spawn_in_sandbox is reached first and did
    // not. Found by havoc.
    if (!fn) return -1;

    // Inside a sandboxed package, the new task goes into the SAME sandbox.
    //
    // Without this a package escaped simply by asking for a second thread:
    // task_spawn starts every task with no regions registered, so the new one
    // ran privileged and every pointer it passed went unchecked. The sandbox
    // has to be a property of the package, not of whichever call entered it.
    int pid = apps_spawn_in_sandbox(name, (int (*)(int))fn, arg, stack);
    if (pid >= 0) return pid;
    // Not inside a package, or no slot left. The first is the shell and the OS
    // spawning their own tasks, which is ordinary; the second is a refusal, and
    // task_spawn below will not paper over it because the caller is then not a
    // package either.
    if (task_app_mem_current()) return -1;

    // A package's task is AFFINITY_ANY, so it uses the second core when there is
    // one and the first when there is not. A package should never have to know.
    return task_spawn(name, "(package)", fn, arg,
                      stack ? stack : TASK_STACK_DEF, AFFINITY_ANY);
}
extern "C" void fw_task_yield(void)            { task_yield(); }
extern "C" void fw_task_sleep_ms(uint32_t ms)  { task_sleep_ms(ms); }
extern "C" int  fw_task_self(void)             { return task_self(); }
// CTRL+C, not just the stop flag.
//
// This returned task_should_stop() alone, which is only set when something
// calls task_kill. Ctrl+C is folded in by intr_check, and a package cannot
// call that — so no package loop has ever been able to see Ctrl+C. httpd ran
// until the connection died, havoc's spin could not be interrupted, and the
// header's promise that "any loop that runs for a while must check it" was
// true of the wrong thing.
//
// intr_check yields as well, which is the other half of what a long loop in a
// package should be doing at the point it asks.
extern "C" int  fw_task_should_stop(void)      { return intr_check() ? 1 : 0; }
extern "C" int  fw_task_kill(int pid)          { return task_kill(pid) ? 1 : 0; }
extern "C" uint32_t fw_cores(void)             { return task_core_count(); }

extern "C" int fw_file_write(const char *path, const void *data, uint32_t len) {
    if (!ok_s(path) || !ok_r(data, len)) return 0;
    task_alive();
    return storage_write_file(path, (const uint8_t *)data, len) ? 1 : 0;
}
extern "C" uint32_t fw_file_read(const char *path, void *buf, uint32_t cap) {
    task_alive();
    if (!ok_s(path) || !ok_w(buf, cap)) return 0;
    return storage_read_file(path, (uint8_t *)buf, cap);
}
extern "C" int fw_file_remove(const char *path) {
    task_alive();
    if (!ok_s(path)) return 0;
    return storage_remove(path) ? 1 : 0;
}
extern "C" int fw_file_exists(const char *path) {
    if (!ok_s(path)) return 0;
    return storage_stat(path, nullptr, nullptr) ? 1 : 0;
}

extern "C" int fw_mkdir(const char *path) {
    task_alive();
    if (!ok_s(path)) return 0;
    return storage_mkdir(path) ? 1 : 0;
}

// --- streamed file operations (API 1.23) ------------------------------------
//
// Doors onto three things storage already did for the shell. Each streams in
// the firmware, so none of them holds a whole file in RAM — which is the whole
// point: fw_file_write plus fw_file_read could only copy a file by landing it
// in one buffer, and a file larger than a single allocation was refused rather
// than copied. See rpc_app.h for the return convention and the copy caveat.
//
// The copy and rename take TWO paths, and BOTH are checked before either is
// used. A package that could get one of the two followed unchecked would be
// handing the firmware a pointer into memory it does not own, which is the one
// thing this boundary exists to stop.
extern "C" int fw_file_append(const char *path, const void *data, uint32_t len) {
    if (!ok_s(path) || !ok_r(data, len)) return 0;
    task_alive();
    return storage_append_file(path, (const uint8_t *)data, len) ? 1 : 0;
}
extern "C" int fw_file_copy(const char *from, const char *to) {
    if (!ok_s(from) || !ok_s(to)) return 0;
    task_alive();
    return storage_copy(from, to) ? 1 : 0;
}
extern "C" int fw_file_rename(const char *from, const char *to) {
    if (!ok_s(from) || !ok_s(to)) return 0;
    task_alive();
    return storage_rename(from, to) ? 1 : 0;
}

// --- the registry (API 1.19) ------------------------------------------------
//
// A package's configuration, in the same store the OS keeps its own in, so
// `reg` shows it and a factory reset clears it. Nova D1 alone carries about
// sixty keys — pins, radio settings, the home layout, lock state — and until
// now the only route was fw_shell_run("reg get ..."), which means a package
// parses text to read its own settings and inherits whatever privileges the
// session happened to have.
//
// fw_reg_get RETURNS THE VALUE BY COPY rather than handing back the pointer
// reg_get gives. That pointer is into the firmware's own table, which a
// sandboxed package cannot read at all — it would be a valid address that
// faults on dereference, which is the worst kind. Copying costs a buffer the
// caller already has.
//
// Writes are not saved to flash automatically. A settings screen changing six
// values in a row should cost one flash write, not six, so persistence is the
// package's explicit call — the same shape reg_set/persist_save_registry has
// inside the firmware.
extern "C" int fw_reg_get(const char *key, char *out, uint32_t cap) {
    task_alive();
    if (!ok_s(key) || !ok_w(out, cap) || cap == 0) return 0;
    const char *v = reg_get(key, nullptr);
    if (!v) { out[0] = 0; return 0; }
    uint32_t n = (uint32_t)strlen(v);
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n);
    out[n] = 0;
    return 1;
}

extern "C" int fw_reg_set(const char *key, const char *value) {
    task_alive();
    if (!ok_s(key) || !ok_s(value)) return 0;
    return reg_set(key, value) ? 1 : 0;
}

extern "C" int32_t fw_reg_get_int(const char *key, int32_t def) {
    task_alive();
    if (!ok_s(key)) return def;
    return reg_get_int(key, def);
}

extern "C" int fw_reg_has(const char *key) {
    task_alive();
    if (!ok_s(key)) return 0;
    return reg_has(key) ? 1 : 0;
}

extern "C" void fw_reg_save(void) {
    task_alive();
    persist_save_registry();
}

// Which board this is — "pico2_w", "pico_w", "pico2", "pico" (API 1.19).
//
// A package is one binary for every board, so it cannot know this at build time,
// and the things that differ are exactly the things a package cares about: which
// GPIO the radio has taken, how many pins exist, whether there is a radio at
// all. Nova D1 picks its pin map from this.
//
// The board's own name, not a guess from the pin count — an RP2350A and an
// RP2040 both report 30 GPIO and are not interchangeable.
extern "C" int fw_board(char *out, unsigned cap) {
    task_alive();
    if (!ok_w(out, cap) || cap == 0) return 0;
    unsigned n = (unsigned)strlen(PICO_BOARD);
    if (n >= cap) n = cap - 1;
    memcpy(out, PICO_BOARD, n);
    out[n] = 0;
    return 1;
}

// How busy the machine is, 0-100, sampled since anything last asked.
//
// For a status bar: the Nova D1 wants a number next to the clock, the way a
// desktop shows one. Per CORE, so two cores each fully busy is 100 rather than
// 200 — the figure people read is "how much of this machine is in use".
extern "C" uint32_t fw_cpu_percent(void) { task_alive(); return task_cpu_percent(); }

// The signal strength of the network this device is ON, in dBm, or 0 when there
// is no reading. A scan says what can be HEARD; this is what is carrying
// traffic, which is the one worth putting on a screen.
bool net_signal(int *rssi_out);

extern "C" int fw_net_rssi(void) {
    task_alive();
    int r = 0;
    return net_signal(&r) ? r : 0;
}

// --- running a shell command ------------------------------------------------
//
// The Nova D1 has a shell app: a screen with a prompt on it, where a command is
// typed on hardware buttons and its output drawn on an OLED. That needs the
// command RUN and its output CAPTURED, not a second terminal — there is one
// serial console and only one thing can own it.
//
// So this is the whole of what such an app needs, and it reuses what `.rps`
// already does for `capture`: the shell's own line runner, with the output
// diverted into a buffer. Pipes, chaining and redirection all work, because it
// is the same runner that handles a typed line.
//
// PRIVILEGE IS THE SESSION'S, not the package's. The command runs exactly as if
// the logged-in user had typed it — an admin session can do admin things and a
// guest session cannot. A package cannot use this to become root, because the
// commands that matter ask users_is_admin about the session and know nothing
// about who called them.
//
// Returns the command's exit status, or -1 if the arguments were refused.
int  shell_run_line_now(char *line);

extern "C" int fw_shell_run(const char *line, char *out, uint32_t cap) {
    if (!ok_s(line)) return -1;
    if (out && cap && !ok_w(out, cap)) return -1;
    task_alive();

    // The runner splits its buffer in place, so it cannot be handed the
    // package's string directly — that memory belongs to the package and the
    // pointer check above proved only that it is READABLE.
    char work[RPC_SHELL_LINE_MAX];
    unsigned n = 0;
    while (n + 1 < sizeof(work) && line[n]) { work[n] = line[n]; n++; }
    if (line[n]) return -1;               // too long to run, rather than truncated
    work[n] = 0;

    if (!out || !cap) return shell_run_line_now(work);

    // Everything a person would have seen, tagged lines included, with the
    // colour stripped out. A pipe wants the data channel only; a package that
    // asked for a command's output means the whole answer.
    //
    // Begin can refuse: there is one capture, and a pipeline or another task's
    // fw_shell_run may hold it. The command still runs — the caller wanted it
    // to — and out stays empty, which is how the caller can tell. Callers that
    // parse what comes back must check for an empty buffer and not read it as
    // an empty answer.
    out[0] = 0;
    if (!out_capture_begin_all(out, cap)) return shell_run_line_now(work);
    int rc = shell_run_line_now(work);
    out_capture_end();
    return rc;
}

// --- running a command on a task of its own (API 1.22) ----------------------
//
// WHY THIS IS NOT fw_task_spawn WITH A SHELL COMMAND IN IT.
//
// fw_task_spawn puts the new task in the CALLING PACKAGE'S SANDBOX, deliberately
// and for a good reason: the function it is given is package code, and a task
// started any other way would run it privileged with no regions registered —
// which is how a package used to escape simply by asking for a thread.
//
// The function here is not package code. It is detach_entry, in the firmware,
// and it runs the firmware's own shell dispatcher. So the sandbox has nothing to
// contain, and task_spawn is the correct call rather than a way round
// apps_spawn_in_sandbox.
//
// That difference is the whole point. A package command dispatched from inside a
// package is refused, on every task the package owns, because entering package
// code records one way back out per task and a second entry overwrites it. This
// task carries no package state at all — it begins in firmware, at depth zero —
// so when the shell dispatches into a package from here it is a first entry, and
// the invariant is honoured rather than bypassed. It is the same reasoning that
// already lets two packages run at once on two different tasks.
//
// The output is captured into a buffer THE FIRMWARE OWNS, and copied into the
// package's only when the package collects the run. So the detached task never
// dereferences a package pointer — which is the lifetime answer: a package can
// be unloaded while its command is still running, and there is nothing left
// pointing into it to write through.
#include "detach.h"
#include "joblist.h"    // the first word of the line, for the task name

static int detach_entry(void *arg) {
    int handle = (int)(intptr_t)arg;
    DetachRun *r = detach_of(handle);
    if (!r) return -1;                 // reclaimed before this task got a turn

    // The runner splits its buffer in place, so the slot's copy is consumed
    // here. Nothing reads it afterwards — `ps` shows the line from the task's
    // own path, which task_spawn copied at spawn time.
    char *cap = detach_capture_buffer(handle);
    int rc;
    if (cap && out_capture_begin_all(cap, DETACH_OUT_MAX)) {
        rc = shell_run_line_now(r->line);
        out_capture_end();
    } else {
        // No buffer wanted, or the one capture was taken after all. The command
        // still runs — that is what was asked for — and the output goes to the
        // console, which is where it would have gone anyway.
        rc = shell_run_line_now(r->line);
    }
    detach_finish(handle, rc);
    return rc;
}

// Which package is asking.
//
// task_app_mem_get rather than task_app_mem_current: the latter answers "is
// there a sandbox to range-check pointers against" and returns null for a
// package running with the OS's own privileges, which is every package on an
// RP2040. This question is "whose code is this", and it has an answer on both.
static const void *calling_package(void) {
    TaskAppMem m;
    if (!task_app_mem_get(&m)) return nullptr;
    return m.text;
}

extern "C" int fw_shell_run_detached(const char *line, char *out, uint32_t cap) {
    if (!ok_s(line)) return -1;
    if (out && cap && !ok_w(out, cap)) return -1;
    task_alive();

    // Only a package can start one. A run belongs to whoever asked for it, and
    // that ownership is the whole of what stops one package collecting another's
    // answer — so a caller with no identity is refused rather than given a
    // handle nobody can be checked against.
    const void *owner = calling_package();
    if (!owner) return -1;

    int handle = detach_claim(owner, line, out, cap);
    if (handle < 0) return -1;
    DetachRun *r = detach_of(handle);

    // TASK_STACK_SHELL, and the size is not caution. On a part with no sandbox
    // the package this dispatches into runs on THIS stack rather than on a
    // sandbox stack of its own, so it has to be the size the shell task itself
    // needed — which was found by overflowing 3 KB in exactly that shape. On
    // RP2350 the task carries only the firmware frames and the size is slack.
    //
    // Named after the command and carrying the whole line as its path, so `ps`
    // reads the way it does for `bg`. Twelve tasks called "detached" would tell
    // whoever is looking at the list nothing at all.
    //
    // The handle travels as the argument rather than a pointer to anything: the
    // slot is the firmware's and outlives the caller either way, and an integer
    // cannot dangle.
    char tname[TASK_NAME_MAX];
    if (!joblist_first_word(r->line, tname, sizeof(tname)))
        snprintf(tname, sizeof(tname), "detached");
    int pid = task_spawn(tname, r->line, detach_entry,
                         (void *)(intptr_t)handle, TASK_STACK_SHELL, AFFINITY_ANY);
    if (pid < 0) {
        detach_release(handle);
        return -1;
    }
    r->pid = pid;
    return handle;
}

extern "C" int fw_shell_done(int handle, int *status) {
    task_alive();
    if (status && !ok_w(status, sizeof(int))) return -1;

    const void *owner = calling_package();
    if (!owner) return -1;
    DetachRun *r = detach_find(handle, owner);
    if (!r) return -1;
    if (!r->done) return 0;

    // Finished, so the answer is handed over HERE — on the package's own task,
    // where the package is alive by construction and its buffer can be checked
    // against the regions it actually holds.
    if (r->pkg_out && r->pkg_cap && ok_w(r->pkg_out, r->pkg_cap)) {
        const char *src = detach_capture_buffer(handle);
        uint32_t n = 0;
        if (src) while (src[n] && n + 1 < r->pkg_cap) n++;
        if (src) memcpy(r->pkg_out, src, n);
        r->pkg_out[n] = 0;
    }
    if (status) *status = r->status;
    detach_release(handle);
    return 1;
}

// --- hardware ---------------------------------------------------------------
//
// Thin wrappers over the SDK, with the pin checked first. On RP2 an out of
// range pin number is not an error the hardware reports — it aliases onto some
// other register — so refusing here is the difference between "that pin does
// not exist" and a device that behaves strangely for reasons nobody can find.

// Pins the OS owns, which a package must not have. On the wireless boards the
// radio is wired to GPIO 23/24/25/29 through the CYW43, and driving those from
// a package takes the network down in a way that looks like a WiFi fault.
static bool pin_reserved(unsigned pin) {
#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI
    return pin == 23 || pin == 24 || pin == 25 || pin == 29;
#else
    (void)pin;
    return false;
#endif
}

extern "C" uint32_t fw_core_id(void) { return task_this_core(); }

extern "C" unsigned fw_gpio_count(void) { return NUM_BANK0_GPIOS; }

extern "C" int fw_gpio_usable(unsigned pin) {
    return (pin < NUM_BANK0_GPIOS && !pin_reserved(pin)) ? 1 : 0;
}

extern "C" int fw_gpio_init(unsigned pin, int dir) {
    if (!fw_gpio_usable(pin)) return -1;
    task_alive();
    gpio_init(pin);
    gpio_set_dir(pin, dir == FW_PIN_OUT);
    return 0;
}

extern "C" int fw_gpio_pull(unsigned pin, int mode) {
    if (!fw_gpio_usable(pin)) return -1;
    gpio_set_pulls(pin, mode == FW_PULL_UP, mode == FW_PULL_DOWN);
    return 0;
}

extern "C" int fw_gpio_put(unsigned pin, int value) {
    if (!fw_gpio_usable(pin)) return -1;
    gpio_put(pin, value != 0);
    return 0;
}

extern "C" int fw_gpio_get(unsigned pin) {
    if (!fw_gpio_usable(pin)) return -1;
    return gpio_get(pin) ? 1 : 0;
}

// --- edges that cannot be missed (API 1.19) ---------------------------------
//
// Polling a button is fine until the thing doing the polling sleeps. The Nova
// D1's screen loop naps up to 300 ms when the display is off, and a tap is
// 40 ms — so a polled button drops presses, and the harder it tries to save
// power the more it drops. Its MicroPython version caught them on a hardware
// interrupt for exactly this reason.
//
// The ABI cannot hand an interrupt to a package: calling INTO unprivileged code
// is the hard direction, it would have to happen with the sandbox already
// programmed, and a package that faults inside an interrupt is a fault with
// nowhere to be contained to. So the interrupt stays in the firmware and only
// its RESULT crosses — a count of edges since the package last asked. Nothing
// is missed, nothing runs in package code at interrupt time, and there is no
// handle to leak.
//
// NO LOCK, and that is deliberate rather than an oversight. lock_hw_enter is
// not recursive and taking it twice on one core hangs the board silently, which
// makes an interrupt handler the last place it belongs. Instead each of the two
// fields has exactly ONE writer: the handler only ever increments `count`, the
// reader only ever writes `seen`, and the difference between them is the
// answer. Unsigned subtraction handles the wrap. There is no interleaving that
// loses an edge because there is no read-modify-write shared between them.
//
// One consequence worth stating: two tasks polling the SAME pin share `seen`,
// so they split the edges between them rather than each seeing all of them.
// That is a package deciding to have two readers for one button, not a fault
// here.
#define GPIO_WATCH_MAX 8

struct GpioWatch {
    volatile uint32_t count;    // written by the handler, only ever ++
    uint32_t          seen;     // written by fw_gpio_events, only ever =
    uint8_t           pin;
    bool              used;
};
static GpioWatch g_watch[GPIO_WATCH_MAX];

static void gpio_watch_irq(uint gpio, uint32_t events) {
    (void)events;
    for (int i = 0; i < GPIO_WATCH_MAX; i++)
        if (g_watch[i].used && g_watch[i].pin == gpio) { g_watch[i].count++; return; }
}

extern "C" int fw_gpio_watch(unsigned pin, int edges) {
    if (!fw_gpio_usable(pin)) return -1;
    task_alive();

    uint32_t mask = 0;
    if (edges & FW_EDGE_FALL) mask |= GPIO_IRQ_EDGE_FALL;
    if (edges & FW_EDGE_RISE) mask |= GPIO_IRQ_EDGE_RISE;

    int slot = -1;
    for (int i = 0; i < GPIO_WATCH_MAX; i++)
        if (g_watch[i].used && g_watch[i].pin == pin) { slot = i; break; }

    // edges == 0 means stop watching. Releasing the slot is what makes a
    // package that opens and closes a screen repeatedly not run out of them.
    if (!mask) {
        if (slot < 0) return 0;
        gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, false);
        g_watch[slot].used = false;
        return 0;
    }

    if (slot < 0)
        for (int i = 0; i < GPIO_WATCH_MAX; i++)
            if (!g_watch[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    // Re-arming an existing watch keeps its history; a fresh one starts at zero
    // on both sides, so the first fw_gpio_events cannot report edges that
    // happened before anyone was interested.
    if (!g_watch[slot].used) {
        g_watch[slot].count = 0;
        g_watch[slot].seen  = 0;
        g_watch[slot].pin   = (uint8_t)pin;
        g_watch[slot].used  = true;
    }

    // The callback and the bank interrupt belong to whichever core calls this,
    // which is where the handler will then run. Setting them every time is
    // harmless and means a package that arms pins from two tasks on two cores
    // gets both served rather than only the first.
    gpio_set_irq_callback(gpio_watch_irq);
    irq_set_enabled(IO_IRQ_BANK0, true);
    gpio_set_irq_enabled(pin, mask, true);
    return 0;
}

extern "C" int fw_gpio_events(unsigned pin, int *level) {
    if (!fw_gpio_usable(pin)) return -1;
    if (level && !ok_w(level, sizeof(*level))) return -1;
    task_alive();
    if (level) *level = gpio_get(pin) ? 1 : 0;
    for (int i = 0; i < GPIO_WATCH_MAX; i++) {
        if (!g_watch[i].used || g_watch[i].pin != pin) continue;
        uint32_t now = g_watch[i].count;
        uint32_t n   = now - g_watch[i].seen;
        g_watch[i].seen = now;
        return (int)(n > 0x7fffffffu ? 0x7fffffffu : n);
    }
    return -1;      // not being watched, which is a different thing from zero
}

// The ADC. Channel 4 is the temperature sensor on RP2040; RP2350 moved it to
// channel 4 as well on the 30-pin parts but the SDK's own constant is the only
// thing worth trusting here, so it is reported rather than assumed.
static bool g_adc_ready;

extern "C" unsigned fw_adc_temp_channel(void) { return 4; }

extern "C" int fw_adc_init(unsigned channel) {
    if (channel > 4) return -1;
    if (!g_adc_ready) { adc_init(); g_adc_ready = true; }
    if (channel == fw_adc_temp_channel()) adc_set_temp_sensor_enabled(true);
    else                                  adc_gpio_init(26 + channel);
    return 0;
}

extern "C" int fw_adc_read(unsigned channel) {
    if (channel > 4 || !g_adc_ready) return -1;
    task_alive();
    adc_select_input(channel);
    return (int)adc_read();
}

// I2C. Returning the SDK's byte count straight through is what makes a bus scan
// work: a zero-length write to an address either acknowledges or does not, and
// the caller can tell those apart without a separate probe call.
static i2c_inst_t *i2c_of(unsigned bus) {
    if (bus == 0) return i2c0;
    if (bus == 1) return i2c1;
    return nullptr;
}
static bool g_i2c_ready[2];

extern "C" int fw_i2c_init(unsigned bus, unsigned sda, unsigned scl, unsigned baud) {
    i2c_inst_t *i = i2c_of(bus);
    if (!i) return -1;
    if (!fw_gpio_usable(sda) || !fw_gpio_usable(scl)) return -1;
    if (baud == 0) baud = 100000;
    task_alive();
    i2c_init(i, baud);
    gpio_set_function(sda, GPIO_FUNC_I2C);
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_pull_up(sda);
    gpio_pull_up(scl);
    g_i2c_ready[bus] = true;
    return 0;
}

extern "C" int fw_i2c_write(unsigned bus, unsigned addr, const void *data,
                            unsigned len, int nostop) {
    if (!ok_r(data, len)) return -1;
    i2c_inst_t *i = i2c_of(bus);
    if (!i || !g_i2c_ready[bus] || addr > 0x7f) return -1;
    task_alive();
    return i2c_write_blocking(i, (uint8_t)addr, (const uint8_t *)data, len, nostop != 0);
}

extern "C" int fw_i2c_read(unsigned bus, unsigned addr, void *buf,
                           unsigned len, int nostop) {
    i2c_inst_t *i = i2c_of(bus);
    if (!i || !g_i2c_ready[bus] || addr > 0x7f) return -1;
    task_alive();
    if (!ok_w(buf, len)) return -1;
    return i2c_read_blocking(i, (uint8_t)addr, (uint8_t *)buf, len, nostop != 0);
}

extern "C" int fw_i2c_deinit(unsigned bus) {
    i2c_inst_t *i = i2c_of(bus);
    if (!i || !g_i2c_ready[bus]) return -1;
    i2c_deinit(i);
    g_i2c_ready[bus] = false;
    return 0;
}

extern "C" uint32_t fw_clock_hz(void) { return clock_get_hz(clk_sys); }

extern "C" uint32_t fw_micros(void) { return time_us_32(); }

// Busy, not yielding, and that is the point. A protocol timed in microseconds
// cannot survive a task switch in the middle of it — a DHT's whole conversation
// is over in about 5 ms, and yielding once loses the reading. Capped so this
// cannot be used to take the core away indefinitely; anything longer belongs in
// fw_task_sleep_ms, which yields properly.
extern "C" void fw_busy_wait_us(uint32_t us) {
    if (us > 20000) us = 20000;
    busy_wait_us(us);
    task_alive();
}

// --- power ------------------------------------------------------------------

int power_sleep(unsigned ms, int wake_pin, int wake_high, bool dormant);
unsigned power_min_sleep_ms(void);

extern "C" int fw_power_sleep(unsigned ms, int pin, int high) {
    return power_sleep(ms, pin, high, /*dormant*/false);
}
extern "C" int fw_power_dormant(unsigned ms, int pin, int high) {
    return power_sleep(ms, pin, high, /*dormant*/true);
}
extern "C" unsigned fw_power_min_sleep_ms(void) { return power_min_sleep_ms(); }

// What is powering the board. See the note in rpc_app.h for why a package
// cannot answer this itself.
// A PURE QUERY, not net_radio_up() — that one is an activator (radio_up()) and
// using it here brought the chip up, or faulted trying, as a side effect of
// drawing the battery icon every frame. That was the incognito freeze.
bool net_radio_is_up(void);
int  net_power_source(void);
extern "C" int fw_power_source(void) {
    task_alive();
#if defined(RPC_HAS_WIFI) && RPC_HAS_WIFI
    // WL_GPIO2 on the CYW43 is the VBUS sense on a Pico W and a Pico 2 W, so
    // reading it means a SPI transaction with the radio. That is asked for HERE
    // sixty times a second, by a status bar drawing a battery icon, on a task
    // that is not pinned to anything.
    //
    // It used to call cyw43_arch_gpio_get straight out, which takes the
    // driver's own lock — keyed on the CORE, and a __wfe wait with no yield and
    // no timeout when the other core holds it. Drawing the icon while the boot
    // join was inside the driver on core 0 stopped core 0 dead, and since the
    // joiner holds a lock, preemption was deferred and the watchdog starved:
    // task #98, a third of all cold boots, no fault and nothing in the log.
    //
    // net_power_source answers from the cache that every other network reading
    // comes from, and refreshes it only when that costs nothing.
    if (!net_radio_is_up()) return FW_POWER_UNKNOWN;
    int p = net_power_source();
    if (p < 0) return FW_POWER_UNKNOWN;
    return p ? FW_POWER_USB : FW_POWER_BATTERY;
#elif defined(PICO_VBUS_PIN)
    // A non-wireless board has it on an ordinary pin, and nothing else is using
    // it there. Left as an input; the boot ROM has already configured it.
    gpio_set_function(PICO_VBUS_PIN, GPIO_FUNC_SIO);
    gpio_set_dir(PICO_VBUS_PIN, GPIO_IN);
    return gpio_get(PICO_VBUS_PIN) ? FW_POWER_USB : FW_POWER_BATTERY;
#else
    return FW_POWER_UNKNOWN;
#endif
}

// --- drawing ----------------------------------------------------------------
//
// FwFrameBuf and FrameBuf are the same three fields in the same order, so the
// cast is free and core/framebuf.cpp stays free of anything ABI-shaped. Checked
// at compile time rather than assumed.
static_assert(sizeof(FwFrameBuf) == sizeof(FrameBuf), "framebuffer layouts must match");


// A framebuffer is a struct in the package's memory that POINTS AT more of it.
//
// Checking the struct is not enough: the firmware follows f->buf and writes
// however many bytes the width and height say. So both are checked, and the
// size is recomputed here from w and h rather than trusted — a package that
// says 8x8 and points at four bytes would otherwise have the firmware write
// sixty past the end of its own buffer.
//
// This is the only nested pointer in the ABI. Everything else that crosses is
// flat, which is worth keeping: each level of indirection is another thing to
// remember to check, and forgetting one is silent.
static bool ok_fb(const FwFrameBuf *f, bool write) {
    if (!ok_r(f, sizeof(FwFrameBuf))) return false;
    if (f->w <= 0 || f->h <= 0) return false;
    int need = fb_bytes(f->w, f->h);
    if (need <= 0) return false;
    return write ? ok_w(f->buf, (uint32_t)need) : ok_r(f->buf, (uint32_t)need);
}

extern "C" int fw_fb_bytes(int w, int h) { return fb_bytes(w, h); }
extern "C" void fw_fb_fill(FwFrameBuf *f, int c) {
    if (!ok_fb(f, true)) return; task_alive(); fb_fill((FrameBuf *)f, c); }
extern "C" void fw_fb_pixel(FwFrameBuf *f, int x, int y, int c) {
    if (!ok_fb(f, true)) return; fb_pixel((FrameBuf *)f, x, y, c); }
extern "C" int  fw_fb_get(const FwFrameBuf *f, int x, int y) {
    if (!ok_fb(f, false)) return 0; return fb_get((const FrameBuf *)f, x, y); }
extern "C" void fw_fb_hline(FwFrameBuf *f, int x, int y, int n, int c) {
    if (!ok_fb(f, true)) return; fb_hline((FrameBuf *)f, x, y, n, c); }
extern "C" void fw_fb_vline(FwFrameBuf *f, int x, int y, int n, int c) {
    if (!ok_fb(f, true)) return; fb_vline((FrameBuf *)f, x, y, n, c); }
extern "C" void fw_fb_line(FwFrameBuf *f, int x0, int y0, int x1, int y1, int c) {
    if (!ok_fb(f, true)) return;
    fb_line((FrameBuf *)f, x0, y0, x1, y1, c);
}
extern "C" void fw_fb_rect(FwFrameBuf *f, int x, int y, int w, int h, int c, int fi) {
    if (!ok_fb(f, true)) return;
    task_alive(); fb_rect((FrameBuf *)f, x, y, w, h, c, fi);
}
extern "C" void fw_fb_text(FwFrameBuf *f, const char *s, int x, int y, int c) {
    if (!ok_fb(f, true) || !ok_s(s)) return;
    task_alive(); fb_text((FrameBuf *)f, s, x, y, c);
}
extern "C" int  fw_fb_text_width(const char *s) {
    if (!ok_s(s)) return 0; return fb_text_width(s); }
extern "C" void fw_fb_blit(FwFrameBuf *d, const FwFrameBuf *s, int x, int y, int t) {
    if (!ok_fb(d, true) || !ok_fb(s, false)) return;
    task_alive(); fb_blit((FrameBuf *)d, (const FrameBuf *)s, x, y, t);
}
extern "C" void fw_fb_scroll(FwFrameBuf *f, int dx, int dy) {
    if (!ok_fb(f, true)) return;
    task_alive(); fb_scroll((FrameBuf *)f, dx, dy);
}

// --- PWM --------------------------------------------------------------------

extern "C" int fw_pwm_init(unsigned pin, unsigned freq_hz) {
    if (!fw_gpio_usable(pin) || freq_hz == 0) return -1;
    task_alive();

    // Pick the smallest divider that lets the wrap count stay under 16 bits,
    // because a bigger wrap is finer duty resolution. At 125 MHz a 1 kHz tone
    // wants a divider of about 2 and a wrap near 62500.
    uint32_t clk = clock_get_hz(clk_sys);
    uint32_t div16 = (uint32_t)((((uint64_t)clk * 16) / freq_hz) / 65536) + 1;
    if (div16 < 16)  div16 = 16;          // 1.0 in 12.4 fixed point
    if (div16 > 255 * 16) div16 = 255 * 16;

    uint32_t wrap = (uint32_t)(((uint64_t)clk * 16) / div16 / freq_hz) - 1;
    if (wrap > 65535) wrap = 65535;
    if (wrap < 1)     wrap = 1;

    uint slice = pwm_gpio_to_slice_num(pin);
    gpio_set_function(pin, GPIO_FUNC_PWM);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac(&c, (uint8_t)(div16 / 16), (uint8_t)(div16 % 16));
    pwm_config_set_wrap(&c, (uint16_t)wrap);
    pwm_init(slice, &c, true);
    pwm_set_gpio_level(pin, 0);           // start silent, not at whatever was there

    return (int)(((uint64_t)clk * 16) / div16 / (wrap + 1));
}

extern "C" int fw_pwm_duty(unsigned pin, unsigned permille) {
    if (!fw_gpio_usable(pin)) return -1;
    if (permille > 1000) permille = 1000;
    uint slice = pwm_gpio_to_slice_num(pin);
    uint32_t wrap = pwm_hw->slice[slice].top;
    pwm_set_gpio_level(pin, (uint16_t)(((uint64_t)wrap + 1) * permille / 1000));
    return 0;
}

extern "C" int fw_pwm_stop(unsigned pin) {
    if (!fw_gpio_usable(pin)) return -1;
    pwm_set_gpio_level(pin, 0);
    pwm_set_enabled(pwm_gpio_to_slice_num(pin), false);
    // Back to a plain input, so a stopped buzzer is not a pin still driving.
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, false);
    return 0;
}

// --- UART -------------------------------------------------------------------

static uart_inst_t *uart_of(unsigned bus) {
    if (bus == 0) return uart0;
    if (bus == 1) return uart1;
    return nullptr;
}
static bool g_uart_ready[2];

extern "C" int fw_uart_init(unsigned bus, unsigned tx, unsigned rx, unsigned baud) {
    uart_inst_t *u = uart_of(bus);
    if (!u) return -1;
    if (!fw_gpio_usable(tx) || !fw_gpio_usable(rx)) return -1;
    if (baud == 0) baud = 115200;
    task_alive();
    int actual = (int)uart_init(u, baud);
    gpio_set_function(tx, GPIO_FUNC_UART);
    gpio_set_function(rx, GPIO_FUNC_UART);
    g_uart_ready[bus] = true;
    return actual;
}

extern "C" int fw_uart_write(unsigned bus, const void *data, unsigned len) {
    if (!ok_r(data, len)) return -1;
    uart_inst_t *u = uart_of(bus);
    if (!u || !g_uart_ready[bus] || !data) return -1;
    task_alive();
    uart_write_blocking(u, (const uint8_t *)data, len);
    return (int)len;
}

extern "C" int fw_uart_available(unsigned bus) {
    uart_inst_t *u = uart_of(bus);
    if (!u || !g_uart_ready[bus]) return -1;
    return uart_is_readable(u) ? 1 : 0;
}

// Returns what arrived rather than insisting on the full length. A framed
// protocol asks for its header, then for the body the header describes, and
// neither request should hang the device when the other end goes quiet.
extern "C" int fw_uart_read(unsigned bus, void *buf, unsigned len, unsigned timeout_ms) {
    if (!ok_w(buf, len)) return -1;
    uart_inst_t *u = uart_of(bus);
    if (!u || !g_uart_ready[bus] || !buf) return -1;
    uint8_t *p = (uint8_t *)buf;
    unsigned got = 0;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (got < len) {
        if (uart_is_readable(u)) { p[got++] = uart_getc(u); continue; }
        if (to_ms_since_boot(get_absolute_time()) - start >= timeout_ms) break;
        // Yield rather than spin: waiting on a serial line is exactly the sort
        // of wait the rest of the device should be allowed to work through.
        task_yield();
    }
    return (int)got;
}

extern "C" int fw_uart_deinit(unsigned bus) {
    uart_inst_t *u = uart_of(bus);
    if (!u || !g_uart_ready[bus]) return -1;
    uart_deinit(u);
    g_uart_ready[bus] = false;
    return 0;
}

// --- system -----------------------------------------------------------------

extern "C" unsigned long fw_random(void) { return (unsigned long)get_rand_32(); }

extern "C" void fw_random_bytes(void *buf, unsigned len) {
    if (!ok_w(buf, len)) return;
    if (!buf) return;
    unsigned char *p = (unsigned char *)buf;
    // A word at a time, because the generator produces 32 bits per call and
    // asking four times for four bytes is four times the work.
    while (len >= 4) {
        uint32_t v = get_rand_32();
        p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
        p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
        p += 4; len -= 4;
    }
    if (len) {
        uint32_t v = get_rand_32();
        while (len--) { *p++ = (unsigned char)v; v >>= 8; }
    }
}

extern "C" int fw_unique_id(char *out, unsigned cap) {
    if (!ok_w(out, cap)) return -1;
    if (!out || cap < 17) return -1;
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    static const char *H = "0123456789abcdef";
    unsigned k = 0;
    for (unsigned i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES && k + 2 < cap; i++) {
        out[k++] = H[id.id[i] >> 4];
        out[k++] = H[id.id[i] & 15];
    }
    out[k] = 0;
    return (int)k;
}

extern "C" void fw_sha256(const void *data, unsigned len, unsigned char *out) {
    if (!ok_r(data, len) || !ok_w(out, 32)) return;
    if (!data || !out) return;
    task_alive();
    sha256(data, len, out);
}

void sys_reboot(void);
extern "C" void fw_reboot(void) { sys_reboot(); }

extern "C" int fw_time_get(struct FwTime *out) {
    if (!ok_w(out, sizeof(struct FwTime))) return -1;
    if (!out) return 0;
    struct tm t;
    if (!aon_timer_get_time_calendar(&t)) {
        // Never set. Zeroed rather than left as whatever was on the stack, so a
        // package that ignores the return value gets an obviously wrong date
        // instead of a plausible one.
        out->year = out->month = out->day = 0;
        out->hour = out->minute = out->second = out->weekday = 0;
        return 0;
    }
    out->year    = t.tm_year + 1900;
    out->month   = t.tm_mon + 1;
    out->day     = t.tm_mday;
    out->hour    = t.tm_hour;
    out->minute  = t.tm_min;
    out->second  = t.tm_sec;
    out->weekday = t.tm_wday;
    return 1;
}

// --- network ----------------------------------------------------------------
//
// Every one of these is a door into something the OS already does, opened for
// packages. Nothing here talks to lwIP directly: the implementations live with
// the code that owns the radio, so a package cannot reach the driver by a route
// that has not been made safe.

int net_pkg_scan(FwNetAp *out, unsigned max);
int net_pkg_ssid(char *out, unsigned cap);
int net_pkg_ip(char *out, unsigned cap);
int net_pkg_resolve(const char *host, char *out, unsigned cap);
int net_pkg_http_get(const char *url, void *buf, unsigned cap);
int net_pkg_http_download(const char *url, const char *path);
int net_pkg_http_measure(const char *url, uint32_t *bytes, uint32_t *ms);
int net_pkg_ping(const char *host, uint32_t timeout_ms);
int net_pkg_tcp_listen(unsigned port);
int net_pkg_tcp_accept(int listener, uint32_t timeout_ms);
int net_pkg_tcp_recv(int conn, void *buf, unsigned cap, uint32_t timeout_ms);
int net_pkg_tcp_send(int conn, const void *buf, unsigned len);
int net_pkg_tcp_close(int handle);
bool net_is_connected(void);

extern "C" int fw_net_connected(void) { return net_is_connected() ? 1 : 0; }
extern "C" int fw_net_ssid(char *out, unsigned cap) {
    if (!ok_w(out, cap)) return -1; return net_pkg_ssid(out, cap); }
extern "C" int fw_net_ip(char *out, unsigned cap)   {
    if (!ok_w(out, cap)) return -1; return net_pkg_ip(out, cap); }

extern "C" int fw_net_scan(FwNetAp *out, unsigned max) {
    if (!ok_w(out, max * sizeof(FwNetAp))) return -1;
    task_alive();
    return net_pkg_scan(out, max);
}
extern "C" int fw_net_resolve(const char *host, char *out, unsigned cap) {
    if (!ok_s(host) || !ok_w(out, cap)) return -1;
    task_alive();
    return net_pkg_resolve(host, out, cap);
}
extern "C" int fw_http_get(const char *url, void *buf, unsigned cap) {
    // Noted, because the gap used to be invisible.
    //
    // A package that faults during a fetch left "entered fw_printf" as the last
    // recorded phase — from whatever it printed BEFORE the call — and the crash
    // then had to be placed by elimination. These three are the longest, deepest
    // things a package can ask for, so they are the ones worth naming.
    bb_note_phase("entered fw_http_get");
    if (!ok_s(url) || !ok_w(buf, cap)) return -1;
    task_alive();
    return net_pkg_http_get(url, buf, cap);
}
extern "C" int fw_http_download(const char *url, const char *path) {
    bb_note_phase("entered fw_http_download");
    if (!ok_s(url) || !ok_s(path)) return -1;
    task_alive();
    return net_pkg_http_download(url, path);
}
extern "C" int fw_http_measure(const char *url, uint32_t *bytes, uint32_t *ms) {
    bb_note_phase("entered fw_http_measure");
    // Both outputs are optional, so each is checked only if it was given —
    // refusing a null here would make "just tell me the byte count" impossible.
    if (!ok_s(url)) return -1;
    if (bytes && !ok_w(bytes, sizeof(*bytes))) return -1;
    if (ms    && !ok_w(ms,    sizeof(*ms)))    return -1;
    task_alive();
    return net_pkg_http_measure(url, bytes, ms);
}
// --- directories -------------------------------------------------------------
//
// storage_walk holds the filesystem lock across the callback — that was #82 —
// so these callbacks copy and count and do nothing else. No printing, no
// yielding, no allocation.
struct DirCount { unsigned n; };

static void dir_count_cb(void *ctx, const char *, bool, uint32_t) {
    ((DirCount *)ctx)->n++;
}

struct DirPick { unsigned want, at; bool found; FwDirEntry *out; };

static void dir_pick_cb(void *ctx, const char *name, bool is_dir, uint32_t size) {
    DirPick *p = (DirPick *)ctx;
    if (p->found || p->at++ != p->want) return;
    snprintf(p->out->name, FW_NAME_MAX, "%s", name ? name : "");
    p->out->is_dir = is_dir ? 1 : 0;
    p->out->size   = size;
    p->found = true;
}

extern "C" uint32_t fw_file_read_at(const char *path, uint32_t offset,
                                    void *buf, uint32_t cap) {
    if (!ok_s(path) || !ok_w(buf, cap)) return 0;
    task_alive();
    return storage_read_at(path, offset, (uint8_t *)buf, cap);
}

extern "C" uint32_t fw_file_size(const char *path) {
    if (!ok_s(path)) return 0;
    task_alive();
    bool is_dir = false;
    uint32_t size = 0;
    if (!storage_stat(path, &is_dir, &size) || is_dir) return 0;
    return size;
}

extern "C" int fw_dir_count(const char *path) {
    if (!ok_s(path)) return -1;
    task_alive();
    DirCount c{0};
    if (!storage_walk(path, dir_count_cb, &c)) return -1;
    return (int)c.n;
}

extern "C" int fw_dir_entry(const char *path, unsigned index, FwDirEntry *out) {
    if (!ok_s(path) || !ok_w(out, sizeof(*out))) return -1;
    task_alive();
    // Filled before the walk, so a caller that ignores the return value gets an
    // empty entry rather than whatever its buffer held.
    memset(out, 0, sizeof(*out));
    DirPick p{index, 0, false, out};
    if (!storage_walk(path, dir_pick_cb, &p)) return -1;
    return p.found ? 1 : 0;
}

// The storage roots a browser lists. Flash is always root 0 and always there;
// a card is root 1 and comes and goes. See FwStorageRoot in rpc_app.h for the
// contract this implements.
//
// The `present=0` row is the part worth reading twice. A card pulled out while
// somebody is browsing it must not make the row VANISH under the cursor — the
// selection would jump to whatever slid up into its place, and on a device
// where the next button press might delete something that matters. So a card
// that has gone is still offered for a few seconds, marked not present, and the
// browser gets to say "card removed" in the row it was already looking at.
// sd_info's `recently_removed` is that window, and a deliberate `sd unmount`
// deliberately does not open it.
//
// The buffer check is for what will actually be WRITTEN, not for `max`: a
// package asking for eight roots with room for two is lying, and validating its
// claim rather than our use would either write past its buffer or refuse a
// perfectly good call.
extern "C" int fw_storage_roots(FwStorageRoot *out, int max) {
    if (max < 1) return -1;
    int want = max < 2 ? max : 2;
    if (!ok_w(out, sizeof(*out) * (unsigned)want)) return -1;
    task_alive();
    memset(out, 0, sizeof(*out) * (unsigned)want);

    snprintf(out[0].label, sizeof(out[0].label), "On-Board");
    snprintf(out[0].path, sizeof(out[0].path), "/");
    out[0].kind = FW_ROOT_FLASH;
    out[0].present = 1;
    out[0].total_kb = (uint32_t)(storage_total_bytes() / 1024);
    out[0].free_kb  = (uint32_t)(storage_free_bytes() / 1024);
    int n = 1;

#if defined(RPC_HAS_SD) && RPC_HAS_SD
    if (want >= 2) {
        SdInfo sd;
        sd_info(&sd);                       // this is also where a card is noticed
        if (sd.mounted || sd.recently_removed) {
            // The label the card carries, when it has one — a browser showing
            // "HOLIDAY" rather than "SD" is showing the user their own card.
            snprintf(out[1].label, sizeof(out[1].label), "%s",
                     (sd.mounted && sd.label[0]) ? sd.label : "SD");
            snprintf(out[1].path, sizeof(out[1].path), SD_ROOT);
            out[1].kind = FW_ROOT_SD;
            out[1].present = sd.mounted ? 1 : 0;
            out[1].total_kb = (uint32_t)(sd.volume_bytes / 1024);
            out[1].free_kb  = (uint32_t)(sd.free_bytes / 1024);
            n = 2;
        }
    }
#endif
    return n;
}

extern "C" int fw_tcp_listen(unsigned port) {
    bb_note_phase("entered fw_tcp_listen");
    task_alive();
    return net_pkg_tcp_listen(port);
}
extern "C" int fw_tcp_accept(int listener, uint32_t timeout_ms) {
    task_alive();
    return net_pkg_tcp_accept(listener, timeout_ms);
}
extern "C" int fw_tcp_recv(int conn, void *buf, unsigned cap, uint32_t timeout_ms) {
    if (!ok_w(buf, cap)) return -1;
    task_alive();
    return net_pkg_tcp_recv(conn, buf, cap, timeout_ms);
}
extern "C" int fw_tcp_send(int conn, const void *buf, unsigned len) {
    if (!ok_r(buf, len)) return -1;
    task_alive();
    return net_pkg_tcp_send(conn, buf, len);
}
extern "C" int fw_tcp_close(int handle) {
    task_alive();
    return net_pkg_tcp_close(handle);
}
extern "C" int fw_net_ping(const char *host, uint32_t timeout_ms) {
    bb_note_phase("entered fw_net_ping");
    if (!ok_s(host)) return -1;
    task_alive();
    return net_pkg_ping(host, timeout_ms);
}

// --- SPI --------------------------------------------------------------------

static spi_inst_t *spi_of(unsigned bus) {
    if (bus == 0) return spi0;
    if (bus == 1) return spi1;
    return nullptr;
}
static bool g_spi_ready[2];

extern "C" int fw_spi_init(unsigned bus, unsigned sck, unsigned mosi,
                           unsigned miso, unsigned baud) {
    spi_inst_t *s = spi_of(bus);
    if (!s) return -1;
    if (!fw_gpio_usable(sck) || !fw_gpio_usable(mosi)) return -1;
    // miso is optional: a write-only device (a display, a shift register) has
    // nothing to send back, and demanding a pin for it would waste one.
    if (miso != 0xffffffffu && !fw_gpio_usable(miso)) return -1;
    if (baud == 0) baud = 1000000;

    task_alive();
    int actual = (int)spi_init(s, baud);
    gpio_set_function(sck,  GPIO_FUNC_SPI);
    gpio_set_function(mosi, GPIO_FUNC_SPI);
    if (miso != 0xffffffffu) gpio_set_function(miso, GPIO_FUNC_SPI);
    g_spi_ready[bus] = true;
    return actual;
}

extern "C" int fw_spi_set_baud(unsigned bus, unsigned baud) {
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus] || baud == 0) return -1;
    return (int)spi_set_baudrate(s, baud);
}

extern "C" int fw_spi_write(unsigned bus, const void *data, unsigned len) {
    if (!ok_r(data, len)) return -1;
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus] || !data) return -1;
    task_alive();
    return spi_write_blocking(s, (const uint8_t *)data, len);
}

extern "C" int fw_spi_read(unsigned bus, void *buf, unsigned len, unsigned char tx_fill) {
    if (!ok_w(buf, len)) return -1;
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus] || !buf) return -1;
    task_alive();
    return spi_read_blocking(s, tx_fill, (uint8_t *)buf, len);
}

extern "C" int fw_spi_transfer(unsigned bus, const void *tx, void *rx, unsigned len) {
    if (!ok_r(tx, len) || !ok_w(rx, len)) return -1;
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus] || !tx || !rx) return -1;
    task_alive();
    return spi_write_read_blocking(s, (const uint8_t *)tx, (uint8_t *)rx, len);
}

extern "C" int fw_spi_deinit(unsigned bus) {
    spi_inst_t *s = spi_of(bus);
    if (!s || !g_spi_ready[bus]) return -1;
    spi_deinit(s);
    g_spi_ready[bus] = false;
    return 0;
}

// --- PIO --------------------------------------------------------------------
//
// State machines are a fixed, shared resource — 8 on RP2040, 12 on RP2350 —
// so they are handed out through a claim table rather than by letting packages
// pick. Two packages both deciding to use PIO0 SM0 is not something either of
// them could detect.

#if PICO_RP2040
#define PIO_BLOCKS 2
#else
#define PIO_BLOCKS 3
#endif
#define PIO_SMS_PER_BLOCK 4
#define PIO_SLOTS (PIO_BLOCKS * PIO_SMS_PER_BLOCK)

struct PioSlot {
    bool     used;
    PIO      pio;
    uint     sm;
    int      offset;        // where the program was loaded, -1 if none
    uint8_t  prog_len;      // needed to give the instruction memory back
    pio_sm_config cfg;
};
static PioSlot g_pio[PIO_SLOTS];

static PIO pio_block(int i) {
    switch (i) {
        case 0: return pio0;
        case 1: return pio1;
#if !PICO_RP2040
        case 2: return pio2;
#endif
        default: return pio0;
    }
}

static PioSlot *pio_slot(int h) {
    if (h < 0 || h >= PIO_SLOTS || !g_pio[h].used) return nullptr;
    return &g_pio[h];
}

extern "C" unsigned fw_pio_count(void) { return PIO_SLOTS; }

extern "C" unsigned fw_pio_free(void) {
    unsigned n = 0;
    for (int i = 0; i < PIO_SLOTS; i++) if (!g_pio[i].used) n++;
    return n;
}

extern "C" int fw_pio_claim(void) {
    for (int i = 0; i < PIO_SLOTS; i++) {
        if (g_pio[i].used) continue;
        PIO p = pio_block(i / PIO_SMS_PER_BLOCK);
        uint sm = (uint)(i % PIO_SMS_PER_BLOCK);
        // Ask the SDK rather than assume: something else in the firmware may
        // already hold this one, and claiming it twice is silent breakage.
        if (pio_sm_is_claimed(p, sm)) continue;
        pio_sm_claim(p, sm);
        g_pio[i].used   = true;
        g_pio[i].pio    = p;
        g_pio[i].sm     = sm;
        g_pio[i].offset = -1;
        g_pio[i].cfg    = pio_get_default_sm_config();
        return i;
    }
    return -1;
}

extern "C" void fw_pio_release(int h) {
    PioSlot *s = pio_slot(h);
    if (!s) return;
    pio_sm_set_enabled(s->pio, s->sm, false);

    // Give the instruction memory back, not just the state machine.
    //
    // There are 32 instruction slots per PIO block and they are the scarcer
    // resource — four state machines share them. Releasing the machine without
    // removing the program leaked those slots, so a package claiming and
    // releasing in a loop would eventually fail to load anything with the state
    // machines all reporting free. The length is kept for exactly this.
    if (s->offset >= 0 && s->prog_len) {
        pio_program pp;
        pp.instructions = nullptr;      // remove only needs origin and length
        pp.length       = s->prog_len;
        pp.origin       = (int8_t)s->offset;
#if !PICO_RP2040
        pp.pio_version  = 0;
        pp.used_gpio_ranges = 0;
#endif
        pio_remove_program(s->pio, &pp, (uint)s->offset);
    }

    pio_sm_unclaim(s->pio, s->sm);
    s->used = false;
    s->offset = -1;
    s->prog_len = 0;
}

extern "C" int fw_pio_load(int h, const unsigned short *prog, unsigned len,
                           unsigned wrap_target, unsigned wrap) {
    PioSlot *s = pio_slot(h);
    if (!s || !prog || len == 0 || len > 32) return -1;
    // The instructions are READ by the firmware, so where they live has to be
    // checked like any other pointer the ABI follows. Without this a package
    // could name flash or a peripheral and have the OS read it on their behalf
    // — havoc handed it 0x10000000 and got a program back.
    if (!ok_r(prog, len * sizeof(prog[0]))) return -1;
    if (wrap >= len || wrap_target >= len) return -1;

    pio_program pp;
    pp.instructions = prog;
    pp.length       = (uint8_t)len;
    pp.origin       = -1;                 // let the SDK place it
#if !PICO_RP2040
    pp.pio_version  = 0;
    pp.used_gpio_ranges = 0;
#endif
    if (!pio_can_add_program(s->pio, &pp)) return -1;
    s->offset   = pio_add_program(s->pio, &pp);
    s->prog_len = (uint8_t)len;
    sm_config_set_wrap(&s->cfg, (uint)(s->offset + wrap_target), (uint)(s->offset + wrap));
    return s->offset;
}

extern "C" int fw_pio_config_pins(int h, unsigned out_base, unsigned out_count,
                                  unsigned set_base, unsigned set_count,
                                  unsigned sideset_base, unsigned sideset_count) {
    PioSlot *s = pio_slot(h);
    if (!s) return -1;
    // Every pin validated the same way the GPIO calls are, and for the same
    // reason: a PIO program driving a pin the radio owns takes the network down.
    if (out_count   && !fw_gpio_usable(out_base))     return -1;
    if (set_count   && !fw_gpio_usable(set_base))     return -1;
    if (sideset_count && !fw_gpio_usable(sideset_base)) return -1;

    if (out_count) {
        for (unsigned i = 0; i < out_count; i++) {
            if (!fw_gpio_usable(out_base + i)) return -1;
            pio_gpio_init(s->pio, out_base + i);
        }
        sm_config_set_out_pins(&s->cfg, out_base, out_count);
        pio_sm_set_consecutive_pindirs(s->pio, s->sm, out_base, out_count, true);
    }
    if (set_count) {
        for (unsigned i = 0; i < set_count; i++) {
            if (!fw_gpio_usable(set_base + i)) return -1;
            pio_gpio_init(s->pio, set_base + i);
        }
        sm_config_set_set_pins(&s->cfg, set_base, set_count);
        pio_sm_set_consecutive_pindirs(s->pio, s->sm, set_base, set_count, true);
    }
    if (sideset_count) {
        for (unsigned i = 0; i < sideset_count; i++) {
            if (!fw_gpio_usable(sideset_base + i)) return -1;
            pio_gpio_init(s->pio, sideset_base + i);
        }
        sm_config_set_sideset_pins(&s->cfg, sideset_base);
    }
    return 0;
}

extern "C" int fw_pio_config_shift(int h, int out_shift_dir, int autopull,
                                   unsigned pull_threshold) {
    PioSlot *s = pio_slot(h);
    if (!s) return -1;
    if (pull_threshold > 32) return -1;
    sm_config_set_out_shift(&s->cfg, out_shift_dir == FW_PIO_SHIFT_RIGHT,
                            autopull != 0, pull_threshold ? pull_threshold : 32);
    return 0;
}

extern "C" int fw_pio_config_clock(int h, unsigned clk_div_x256) {
    PioSlot *s = pio_slot(h);
    if (!s || clk_div_x256 == 0) return -1;
    // 24.8 fixed point, split into the integer and fractional halves the
    // hardware wants. No float crosses the ABI, which matters because the
    // calling convention for one is not the same on both chips.
    uint16_t whole = (uint16_t)(clk_div_x256 >> 8);
    uint8_t  frac  = (uint8_t)(clk_div_x256 & 0xff);
    if (whole == 0 && frac == 0) return -1;
    sm_config_set_clkdiv_int_frac(&s->cfg, whole, frac);
    return 0;
}

extern "C" int fw_pio_start(int h) {
    PioSlot *s = pio_slot(h);
    if (!s || s->offset < 0) return -1;
    pio_sm_init(s->pio, s->sm, (uint)s->offset, &s->cfg);
    pio_sm_set_enabled(s->pio, s->sm, true);
    return 0;
}

extern "C" void fw_pio_stop(int h) {
    PioSlot *s = pio_slot(h);
    if (s) pio_sm_set_enabled(s->pio, s->sm, false);
}

extern "C" int fw_pio_put(int h, unsigned long value, unsigned timeout_us) {
    PioSlot *s = pio_slot(h);
    if (!s) return -1;
    uint32_t start = time_us_32();
    while (pio_sm_is_tx_fifo_full(s->pio, s->sm)) {
        if (time_us_32() - start > timeout_us) return -1;
        // Yield only when the wait is long enough to be worth it. A FIFO with
        // four slots drains in microseconds at any sensible clock, and yielding
        // on every word would cost more than the transfer.
        if (time_us_32() - start > 200) task_yield();
    }
    pio_sm_put(s->pio, s->sm, (uint32_t)value);
    return 0;
}

extern "C" int fw_pio_get(int h, unsigned long *out, unsigned timeout_us) {
    if (!ok_w(out, sizeof(unsigned long))) return -1;
    PioSlot *s = pio_slot(h);
    if (!s || !out) return -1;
    uint32_t start = time_us_32();
    while (pio_sm_is_rx_fifo_empty(s->pio, s->sm)) {
        if (time_us_32() - start > timeout_us) return 0;
        if (time_us_32() - start > 200) task_yield();
    }
    *out = pio_sm_get(s->pio, s->sm);
    return 1;
}

extern "C" uint32_t fw_heap_free(void)  { return heap_free(); }
extern "C" uint32_t fw_heap_total(void) { return heap_total(); }

// A checkpoint that survives a hang. Printed output does not: whatever is in the
// USB buffer when the device stops is never delivered, which is why a crash
// report can name the command but not the line. This is recorded in memory the
// reset does not clear, so the last checkpoint reached IS the failing step.
extern "C" void fw_progress(const char *what) {
    if (!ok_s(what)) return; task_alive(); bb_note_phase(what); }

// The biggest single allocation available right now, found by probing. Free
// bytes do not predict whether the next allocation succeeds; this does.
extern "C" uint32_t fw_heap_largest(void) {
    uint32_t lo = 0, hi = heap_free(), best = 0;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (mid == 0) break;
        void *p = malloc(mid);
        if (p) { free(p); best = mid; lo = mid + 1024; }
        else   { if (mid < 1024) break; hi = mid - 1024; }
    }
    return best;
}

// --- the compiler's runtime -------------------------------------------------
//
// Not "API" in the versioned sense — these are the helpers GCC EMITS CALLS TO
// without being asked. An integer % becomes __aeabi_idivmod, a struct copy
// becomes memcpy, and neither appears anywhere in the package's source. Leaving
// them out meant any package doing arithmetic more complicated than addition
// failed to load with "unresolved symbol", which is exactly what happened to the
// self test.
//
// They are listed separately from the API because they are not a compatibility
// commitment this OS makes — they are the C runtime the compiler assumes exists,
// and adding one is not a MINOR bump.
extern "C" {
// Integer division. Cortex-M0+ has no divide instruction at all, so on RP2040
// even a plain / turns into one of these.
void __aeabi_idiv(void);
void __aeabi_idivmod(void);
void __aeabi_uidiv(void);
void __aeabi_uidivmod(void);
void __aeabi_ldivmod(void);
void __aeabi_uldivmod(void);
// 64-bit arithmetic.
void __aeabi_lmul(void);
void __aeabi_llsl(void);
void __aeabi_llsr(void);
void __aeabi_lasr(void);
// Double-precision soft float. Neither RP2040 nor RP2350 has a double FPU, so
// every `double` operation in a package is one of these — including comparisons
// and the conversions at the edges. The first converted package that did any
// arithmetic at all (calc) needed eleven of them, which is a fair sign that
// leaving them out would have met every later one too.
void __aeabi_dadd(void);
void __aeabi_dsub(void);
void __aeabi_dmul(void);
void __aeabi_ddiv(void);
void __aeabi_dcmpeq(void);
void __aeabi_dcmplt(void);
void __aeabi_dcmple(void);
void __aeabi_dcmpge(void);
void __aeabi_dcmpgt(void);
void __aeabi_dcmpun(void);
void __aeabi_d2iz(void);
void __aeabi_d2uiz(void);
void __aeabi_d2lz(void);
void __aeabi_d2ulz(void);
void __aeabi_i2d(void);
void __aeabi_ui2d(void);
void __aeabi_l2d(void);
void __aeabi_ul2d(void);
// Single precision, for the same reason — a package using float rather than
// double would otherwise hit the identical wall one conversion later.
void __aeabi_fadd(void);
void __aeabi_fsub(void);
void __aeabi_fmul(void);
void __aeabi_fdiv(void);
void __aeabi_fcmpeq(void);
void __aeabi_fcmplt(void);
void __aeabi_fcmple(void);
void __aeabi_fcmpge(void);
void __aeabi_fcmpgt(void);
void __aeabi_f2iz(void);
void __aeabi_i2f(void);
void __aeabi_f2d(void);
void __aeabi_d2f(void);
}

// --- the table -------------------------------------------------------------

struct ApiSymbol { const char *name; uint32_t addr; };
#define SYM(fn) { #fn, (uint32_t)(uintptr_t)(void *)&fn }

static const ApiSymbol kSymbols[] = {
    SYM(fw_printf),
    SYM(fw_millis),
    SYM(fw_malloc),
    SYM(fw_free),
    SYM(fw_log),
    SYM(rpc_register_command),
    // API 1.3
    SYM(fw_task_spawn),
    SYM(fw_task_yield),
    SYM(fw_task_sleep_ms),
    SYM(fw_task_self),
    SYM(fw_task_should_stop),
    SYM(fw_task_kill),
    SYM(fw_cores),
    SYM(fw_file_write),
    SYM(fw_file_read),
    SYM(fw_file_remove),
    SYM(fw_file_exists),
    SYM(fw_mkdir),
    SYM(fw_reg_get),
    SYM(fw_reg_set),
    SYM(fw_reg_get_int),
    SYM(fw_reg_has),
    SYM(fw_reg_save),
    SYM(fw_board),
    SYM(fw_shell_run),
    SYM(fw_cpu_percent),
    SYM(fw_net_rssi),
    SYM(fw_core_id),
    SYM(fw_power_sleep),
    SYM(fw_power_dormant),
    SYM(fw_power_min_sleep_ms),
    SYM(fw_power_source),
    SYM(fw_fb_bytes),
    SYM(fw_fb_fill),
    SYM(fw_fb_pixel),
    SYM(fw_fb_get),
    SYM(fw_fb_hline),
    SYM(fw_fb_vline),
    SYM(fw_fb_line),
    SYM(fw_fb_rect),
    SYM(fw_fb_text),
    SYM(fw_fb_text_width),
    SYM(fw_fb_blit),
    SYM(fw_fb_scroll),
    SYM(fw_pwm_init),
    SYM(fw_pwm_duty),
    SYM(fw_pwm_stop),
    SYM(fw_uart_init),
    SYM(fw_uart_write),
    SYM(fw_uart_read),
    SYM(fw_uart_available),
    SYM(fw_uart_deinit),
    SYM(fw_random),
    SYM(fw_random_bytes),
    SYM(fw_unique_id),
    SYM(fw_sha256),
    SYM(fw_reboot),
    SYM(fw_time_get),
    SYM(fw_net_connected),
    SYM(fw_net_ssid),
    SYM(fw_net_ip),
    SYM(fw_net_scan),
    SYM(fw_net_resolve),
    SYM(fw_http_get),
    SYM(fw_http_download),
    SYM(fw_http_measure),
    SYM(fw_net_ping),
    SYM(fw_file_read_at),
    SYM(fw_file_size),
    SYM(fw_dir_count),
    SYM(fw_dir_entry),
    SYM(fw_storage_roots),
    SYM(fw_tcp_listen),
    SYM(fw_tcp_accept),
    SYM(fw_tcp_recv),
    SYM(fw_tcp_send),
    SYM(fw_tcp_close),
    SYM(fw_spi_init),
    SYM(fw_spi_set_baud),
    SYM(fw_spi_write),
    SYM(fw_spi_read),
    SYM(fw_spi_transfer),
    SYM(fw_spi_deinit),
    SYM(fw_pio_claim),
    SYM(fw_pio_release),
    SYM(fw_pio_load),
    SYM(fw_pio_config_pins),
    SYM(fw_pio_config_shift),
    SYM(fw_pio_config_clock),
    SYM(fw_pio_start),
    SYM(fw_pio_stop),
    SYM(fw_pio_put),
    SYM(fw_pio_get),
    SYM(fw_pio_count),
    SYM(fw_pio_free),
    SYM(fw_gpio_count),
    SYM(fw_gpio_usable),
    SYM(fw_gpio_init),
    SYM(fw_gpio_pull),
    SYM(fw_gpio_put),
    SYM(fw_gpio_get),
    SYM(fw_gpio_watch),
    SYM(fw_gpio_events),
    SYM(fw_adc_init),
    SYM(fw_adc_read),
    SYM(fw_adc_temp_channel),
    SYM(fw_i2c_init),
    SYM(fw_i2c_write),
    SYM(fw_i2c_read),
    SYM(fw_i2c_deinit),
    SYM(fw_clock_hz),
    SYM(fw_micros),
    SYM(fw_busy_wait_us),
    SYM(fw_heap_free),
    SYM(fw_heap_total),
    SYM(fw_heap_largest),
    SYM(fw_progress),
    // API 1.4 — the TUI.
    SYM(fw_tui_begin),
    SYM(fw_tui_end),
    SYM(fw_tui_size),
    SYM(fw_tui_clear),
    SYM(fw_tui_text),
    SYM(fw_tui_box),
    SYM(fw_tui_fill),
    SYM(fw_tui_present),
    SYM(fw_tui_poll),
    SYM(fw_tui_refresh),
    // API 1.22 — a shell command on a task of its own.
    SYM(fw_shell_run_detached),
    SYM(fw_shell_done),

    // The compiler's runtime. See above: emitted, not written.
    SYM(__aeabi_idiv),
    SYM(__aeabi_idivmod),
    SYM(__aeabi_uidiv),
    SYM(__aeabi_uidivmod),
    SYM(__aeabi_ldivmod),
    SYM(__aeabi_uldivmod),
    SYM(__aeabi_lmul),
    SYM(__aeabi_llsl),
    SYM(__aeabi_llsr),
    SYM(__aeabi_lasr),
    SYM(__aeabi_dadd),
    SYM(__aeabi_dsub),
    SYM(__aeabi_dmul),
    SYM(__aeabi_ddiv),
    SYM(__aeabi_dcmpeq),
    SYM(__aeabi_dcmplt),
    SYM(__aeabi_dcmple),
    SYM(__aeabi_dcmpge),
    SYM(__aeabi_dcmpgt),
    SYM(__aeabi_dcmpun),
    SYM(__aeabi_d2iz),
    SYM(__aeabi_d2uiz),
    SYM(__aeabi_d2lz),
    SYM(__aeabi_d2ulz),
    SYM(__aeabi_i2d),
    SYM(__aeabi_ui2d),
    SYM(__aeabi_l2d),
    SYM(__aeabi_ul2d),
    SYM(__aeabi_fadd),
    SYM(__aeabi_fsub),
    SYM(__aeabi_fmul),
    SYM(__aeabi_fdiv),
    SYM(__aeabi_fcmpeq),
    SYM(__aeabi_fcmplt),
    SYM(__aeabi_fcmple),
    SYM(__aeabi_fcmpge),
    SYM(__aeabi_fcmpgt),
    SYM(__aeabi_f2iz),
    SYM(__aeabi_i2f),
    SYM(__aeabi_f2d),
    SYM(__aeabi_d2f),
    SYM(memcpy),
    SYM(memset),
    SYM(memmove),
    SYM(memcmp),
    SYM(strlen),
    SYM(strcmp),
    SYM(strncmp),
    SYM(strcpy),
    SYM(strncpy),
    SYM(strchr),
    SYM(strstr),
    SYM(snprintf),

    // API 1.23 — streamed file operations. THE END OF THE TABLE, not beside the
    // 1.3 file calls, because a package names a firmware function by its INDEX
    // here: that index is baked into the veneers in a package's flash slot at
    // install, so anything but a true append repoints already-installed slot
    // packages — the libc entries above included — at the wrong function. Placed
    // after the whole run, the existing indices do not move and a package built
    // before 1.23 keeps working without a reinstall. Every addition goes here.
    SYM(fw_file_append),
    SYM(fw_file_copy),
    SYM(fw_file_rename),
};
static const uint32_t kSymbolCount = sizeof(kSymbols) / sizeof(kSymbols[0]);

uint32_t api_lookup(const char *name) {
    for (uint32_t i = 0; i < kSymbolCount; i++)
        if (strcmp(kSymbols[i].name, name) == 0) return kSymbols[i].addr;
    return 0;
}
uint32_t api_symbol_count(void) { return kSymbolCount; }

// The identity a flash slot records so a later firmware whose table has drifted
// can refuse it. Over the first `count` names, in order, each including its NUL —
// so a reorder or a removal changes the hash while an append past `count` leaves
// it untouched. Reuses pkgslot's CRC-32 so there is one hash in the tree, and the
// commit and open sides call THIS one function, so they cannot disagree.
uint32_t api_abi_prefix_crc(uint32_t count) {
    if (count > kSymbolCount) count = kSymbolCount;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < count; i++)
        crc = pkgslot_crc32(crc, kSymbols[i].name,
                            (uint32_t)strlen(kSymbols[i].name) + 1u);
    return crc;
}

// The same lookup, by INDEX rather than address.
//
// A sandboxed package cannot branch into the firmware at all — flash is not
// reachable from unprivileged code, which is the point — so it names the
// function it wants by its position in this table and asks the supervisor call
// to make the jump. The index has to come from here, because this table is the
// only definition of what that position means.
//
// Returns -1 for a name the firmware does not export, which is the same answer
// api_lookup gives by returning 0: the app is refused at load time rather than
// being allowed to ask for something that does not exist.
int api_index_of(const char *name) {
    for (uint32_t i = 0; i < kSymbolCount; i++)
        if (strcmp(kSymbols[i].name, name) == 0) return (int)i;
    return -1;
}

// And back again. This is the one the supervisor call uses on every entry, so
// the bounds check is the whole security property: an index out of range is a
// package asking for something that is not in the table, and the answer is
// zero rather than whatever happens to follow it in memory.
uint32_t api_addr_at(uint32_t index) {
    if (index >= kSymbolCount) return 0;
    return kSymbols[index].addr;
}

// --- the TUI (API 1.4) ------------------------------------------------------
//
// A package gets the same drawing surface the built-in apps use. The grid lives
// here rather than in the package so the diffing renderer has something stable
// to compare against, and so a package that crashes mid-draw cannot leave the
// terminal in a state nothing can recover.

#include "tui.h"
#include "tuiterm.h"

// The ABI's FW_KEY_* values are the terminal layer's TuiKey values — fw_tui_poll
// (below) copies e.key across with no translation, so a package receives exactly
// the number tuikey.h produced. That makes these two enums one shared numbering,
// and the header's constants a frozen copy of it. If the TuiKey enum ever grows
// a new key BEFORE one of these, its value shifts and the header goes silently
// wrong — which is precisely how FW_KEY_ESC came to read 279 while a real Escape
// arrived as 282, so nothing quitting on Escape ever quit. These asserts turn
// that back into a build error the day it happens rather than a key that does
// nothing on a device nobody can debug.
static_assert(FW_KEY_UP     == TUI_KEY_UP,     "FW_KEY_UP drifted from TuiKey");
static_assert(FW_KEY_DOWN   == TUI_KEY_DOWN,   "FW_KEY_DOWN drifted from TuiKey");
static_assert(FW_KEY_LEFT   == TUI_KEY_LEFT,   "FW_KEY_LEFT drifted from TuiKey");
static_assert(FW_KEY_RIGHT  == TUI_KEY_RIGHT,  "FW_KEY_RIGHT drifted from TuiKey");
static_assert(FW_KEY_HOME   == TUI_KEY_HOME,   "FW_KEY_HOME drifted from TuiKey");
static_assert(FW_KEY_END    == TUI_KEY_END,    "FW_KEY_END drifted from TuiKey");
static_assert(FW_KEY_PGUP   == TUI_KEY_PGUP,   "FW_KEY_PGUP drifted from TuiKey");
static_assert(FW_KEY_PGDN   == TUI_KEY_PGDN,   "FW_KEY_PGDN drifted from TuiKey");
static_assert(FW_KEY_INSERT == TUI_KEY_INSERT, "FW_KEY_INSERT drifted from TuiKey");
static_assert(FW_KEY_DELETE == TUI_KEY_DELETE, "FW_KEY_DELETE drifted from TuiKey");
static_assert(FW_KEY_ESC    == TUI_KEY_ESCAPE, "FW_KEY_ESC drifted from TuiKey");

// Allocated while a full-screen app runs, not for the whole uptime. ~12 KB is
// worth having back on a device with 374 KB of usable heap, and a session that
// never opens a TUI should not pay for one.
static TuiScreen *g_app_screen;

extern "C" void fw_tui_begin(void) {
    task_alive();
    if (!g_app_screen) g_app_screen = (TuiScreen *)malloc(sizeof(TuiScreen));
    if (!g_app_screen) { fw_log(2, "not enough memory for a full-screen app"); return; }

    // begin FIRST: it is what asks the terminal how big it is, so reading the
    // size before it runs would only ever return the default.
    tuiterm_begin();
    uint16_t tw = 80, th = 24;
    tuiterm_size(&tw, &th);
    tui_resize(g_app_screen, tw, th);
}

extern "C" void fw_tui_end(void) {
    task_alive();
    tuiterm_end();
    free(g_app_screen);
    g_app_screen = nullptr;
}

extern "C" void fw_tui_size(int *w, int *h) {
    if (!ok_w(w, sizeof(int)) || !ok_w(h, sizeof(int))) return;
    if (w) *w = g_app_screen ? g_app_screen->w : 0;
    if (h) *h = g_app_screen ? g_app_screen->h : 0;
}

extern "C" void fw_tui_clear(void) { task_alive(); if (g_app_screen) tui_clear(g_app_screen); }

extern "C" void fw_tui_text(int x, int y, const char *s, unsigned char attr, unsigned char fg) {
    if (!ok_s(s)) return;
    task_alive();
    if (g_app_screen) tui_text(g_app_screen, x, y, s, attr, fg);
}

extern "C" void fw_tui_box(int x, int y, int w, int h, const char *title,
                           unsigned char attr, unsigned char fg) {
    task_alive();
    // A box may legitimately have no title, so null is allowed — but anything
    // else has to be a string this package can actually see.
    if (title && !ok_s(title)) return;
    if (g_app_screen) tui_box(g_app_screen, x, y, w, h, title, attr, fg);
}

extern "C" void fw_tui_fill(int x, int y, int w, int h, char ch,
                            unsigned char attr, unsigned char fg) {
    task_alive();
    if (g_app_screen) tui_fill(g_app_screen, x, y, w, h, ch, attr, fg);
}

extern "C" void fw_tui_present(void) { task_alive(); if (g_app_screen) tuiterm_present(g_app_screen); }

extern "C" int fw_tui_refresh(void) {
    task_alive();
    if (!g_app_screen) return 0;
    bool changed = tuiterm_refresh();
    uint16_t tw = 80, th = 24;
    tuiterm_size(&tw, &th);
    tui_resize(g_app_screen, tw, th);
    return changed ? 1 : 0;
}

extern "C" int fw_tui_poll(FwTuiEvent *out) {
    if (!ok_w(out, sizeof(FwTuiEvent))) return 0;
    task_alive();
    if (!out) return 0;
    TuiEvent e;
    if (!tuiterm_poll(&e)) return 0;
    out->kind  = e.kind;
    out->key   = e.key;
    out->mouse = e.mouse;
    out->x = e.x; out->y = e.y;
    out->ctrl = e.ctrl; out->shift = e.shift; out->alt = e.alt;
    return 1;
}
