// The exported symbol table — the firmware's side of the app ABI.
//
// A flat array with a linear scan. That is deliberate for a table this size:
// lookup happens once per undefined symbol at load time, perhaps a dozen times
// per app, and a sorted table with a binary search would trade a measurable
// amount of flash for a saving nobody can perceive. Revisit if the table passes
// a few hundred entries.
//
// Every entry here is a permanent compatibility commitment. Adding one is a
// MINOR bump; changing or removing one is a MAJOR bump and refuses every app
// built before it.

#include "api.h"
#include "rpc_app.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "pico/stdlib.h"

// --- implementations -------------------------------------------------------

extern "C" int fw_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

extern "C" uint32_t fw_millis(void) {
    return (uint32_t)(time_us_64() / 1000u);
}

extern "C" void *fw_malloc(size_t n) { return malloc(n); }
extern "C" void  fw_free(void *p)    { free(p); }

// --- the table -------------------------------------------------------------

struct ApiSymbol { const char *name; uint32_t addr; };

#define SYM(fn) { #fn, (uint32_t)(void *)&fn }

static const ApiSymbol kSymbols[] = {
    SYM(fw_printf),
    SYM(fw_millis),
    SYM(fw_malloc),
    SYM(fw_free),
};

static const uint32_t kSymbolCount = sizeof(kSymbols) / sizeof(kSymbols[0]);

uint32_t api_lookup(const char *name) {
    for (uint32_t i = 0; i < kSymbolCount; i++)
        if (strcmp(kSymbols[i].name, name) == 0) return kSymbols[i].addr;
    return 0;
}

uint32_t api_symbol_count(void) { return kSymbolCount; }
