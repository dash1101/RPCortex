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

extern "C" int fw_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap); va_end(ap);
    return n;
}
extern "C" uint32_t fw_millis(void) { return (uint32_t)(time_us_64() / 1000u); }
extern "C" void    *fw_malloc(size_t n) { return malloc(n); }
extern "C" void     fw_free(void *p)    { free(p); }

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
    return storage_write_file(path, (const uint8_t *)data, len) ? 1 : 0;
}
extern "C" uint32_t fw_file_read(const char *path, void *buf, uint32_t cap) {
    return storage_read_file(path, (uint8_t *)buf, cap);
}
extern "C" int fw_file_remove(const char *path) { return storage_remove(path) ? 1 : 0; }
extern "C" int fw_file_exists(const char *path) { return storage_stat(path, nullptr, nullptr) ? 1 : 0; }

extern "C" uint32_t fw_heap_free(void)  { return heap_free(); }
extern "C" uint32_t fw_heap_total(void) { return heap_total(); }

// A checkpoint that survives a hang. Printed output does not: whatever is in the
// USB buffer when the device stops is never delivered, which is why a crash
// report can name the command but not the line. This is recorded in memory the
// reset does not clear, so the last checkpoint reached IS the failing step.
extern "C" void fw_progress(const char *what) { bb_note_phase(what); }

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
