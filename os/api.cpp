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
};
static const uint32_t kSymbolCount = sizeof(kSymbols) / sizeof(kSymbols[0]);

uint32_t api_lookup(const char *name) {
    for (uint32_t i = 0; i < kSymbolCount; i++)
        if (strcmp(kSymbols[i].name, name) == 0) return kSymbols[i].addr;
    return 0;
}
uint32_t api_symbol_count(void) { return kSymbolCount; }
