// The exported symbol table — the OS side of the app ABI.
//
// A flat array with a linear scan: lookup happens a handful of times per app at
// load time, so a sorted table and a binary search would trade flash for a
// saving nobody can measure. Every entry is a permanent commitment; adding one
// is a MINOR bump, changing one is MAJOR.

#include "api.h"
#include "rpc_app.h"
#include "kernel.h"
#include "command.h"
#include "task.h"
#include "storage.h"
#include "logring.h"
#include "blackbox.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "pico/stdlib.h"

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
extern "C" int fw_printf(const char *fmt, ...) {
    // Reaching here at all proves the call from package code into the firmware
    // works — the veneer, the relocation and the symbol lookup. If a crash
    // report stops at "calling package 'x'" and never shows this, the fault is
    // in getting here, not in anything the package does.
    bb_note_phase("entered fw_printf");
    task_alive();
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap); va_end(ap);
    return n;
}
extern "C" uint32_t fw_millis(void) { bb_note_phase("entered fw_millis"); task_alive(); return (uint32_t)(time_us_64() / 1000u); }
extern "C" void    *fw_malloc(size_t n) { bb_note_phase("entered fw_malloc"); task_alive(); return malloc(n); }
extern "C" void     fw_free(void *p)    { task_alive(); free(p); }

extern "C" void fw_log(int level, const char *msg) {
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

extern "C" int fw_task_spawn(const char *name, TaskFn fn, void *arg, uint32_t stack) {
    // A package's task is AFFINITY_ANY, so it uses the second core when there is
    // one and the first when there is not. A package should never have to know.
    return task_spawn(name, "(package)", fn, arg,
                      stack ? stack : TASK_STACK_DEF, AFFINITY_ANY);
}
extern "C" void fw_task_yield(void)            { task_yield(); }
extern "C" void fw_task_sleep_ms(uint32_t ms)  { task_sleep_ms(ms); }
extern "C" int  fw_task_self(void)             { return task_self(); }
extern "C" int  fw_task_should_stop(void)      { return task_should_stop() ? 1 : 0; }
extern "C" int  fw_task_kill(int pid)          { return task_kill(pid) ? 1 : 0; }
extern "C" uint32_t fw_cores(void)             { return task_core_count(); }

extern "C" int fw_file_write(const char *path, const void *data, uint32_t len) {
    task_alive();
    return storage_write_file(path, (const uint8_t *)data, len) ? 1 : 0;
}
extern "C" uint32_t fw_file_read(const char *path, void *buf, uint32_t cap) {
    task_alive();
    return storage_read_file(path, (uint8_t *)buf, cap);
}
extern "C" int fw_file_remove(const char *path) { task_alive(); return storage_remove(path) ? 1 : 0; }
extern "C" int fw_file_exists(const char *path) { return storage_stat(path, nullptr, nullptr) ? 1 : 0; }

extern "C" uint32_t fw_heap_free(void)  { return heap_free(); }
extern "C" uint32_t fw_heap_total(void) { return heap_total(); }

// A checkpoint that survives a hang. Printed output does not: whatever is in the
// USB buffer when the device stops is never delivered, which is why a crash
// report can name the command but not the line. This is recorded in memory the
// reset does not clear, so the last checkpoint reached IS the failing step.
extern "C" void fw_progress(const char *what) { task_alive(); bb_note_phase(what); }

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
};
static const uint32_t kSymbolCount = sizeof(kSymbols) / sizeof(kSymbols[0]);

uint32_t api_lookup(const char *name) {
    for (uint32_t i = 0; i < kSymbolCount; i++)
        if (strcmp(kSymbols[i].name, name) == 0) return kSymbols[i].addr;
    return 0;
}
uint32_t api_symbol_count(void) { return kSymbolCount; }

// --- the TUI (API 1.4) ------------------------------------------------------
//
// A package gets the same drawing surface the built-in apps use. The grid lives
// here rather than in the package so the diffing renderer has something stable
// to compare against, and so a package that crashes mid-draw cannot leave the
// terminal in a state nothing can recover.

#include "tui.h"
#include "tuiterm.h"

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
    if (w) *w = g_app_screen ? g_app_screen->w : 0;
    if (h) *h = g_app_screen ? g_app_screen->h : 0;
}

extern "C" void fw_tui_clear(void) { task_alive(); if (g_app_screen) tui_clear(g_app_screen); }

extern "C" void fw_tui_text(int x, int y, const char *s, unsigned char attr, unsigned char fg) {
    task_alive();
    if (g_app_screen) tui_text(g_app_screen, x, y, s, attr, fg);
}

extern "C" void fw_tui_box(int x, int y, int w, int h, const char *title,
                           unsigned char attr, unsigned char fg) {
    task_alive();
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
