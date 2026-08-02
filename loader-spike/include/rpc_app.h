// The app-facing ABI. This is the ONLY header an application includes.
//
// An app is a relocatable ELF object. It never links against the firmware; it
// declares the firmware functions it wants as `extern` and the loader patches
// the call sites at load time from a symbol table the firmware exports.
//
// Everything here is `extern "C"`. C++ name mangling is a compiler-version
// detail, and an ABI that changes when the toolchain is upgraded is not an ABI.
#ifndef RPC_APP_H
#define RPC_APP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- API version -----------------------------------------------------------
// MAJOR changes when an exported symbol is removed or changes signature: apps
// built against an older major are refused. MINOR changes when symbols are
// added: an app built against an older minor still runs, because everything it
// asked for is still there.
#define RPC_API_MAJOR 1
#define RPC_API_MINOR 0

// Every app carries this in a dedicated section so the loader can read it
// WITHOUT relocating or running anything. Refusing a mismatched app has to
// happen before its code is trusted, not after.
typedef struct {
    uint32_t magic;          // RPC_APP_MAGIC
    uint16_t api_major;
    uint16_t api_minor;
    char     name[24];
    uint32_t reserved;
} RpcAppHeader;

#define RPC_APP_MAGIC 0x52504341u   // 'RPCA'

// Places the header in its own section and keeps it despite --gc-sections.
#define RPC_APP(appname)                                                      \
    __attribute__((section(".rpc_app_header"), used))                         \
    const RpcAppHeader rpc_app_header = {                                     \
        RPC_APP_MAGIC, RPC_API_MAJOR, RPC_API_MINOR, appname, 0                \
    }

// --- exported firmware services --------------------------------------------
// Resolved by name at load time. Adding to this list is a MINOR bump; changing
// or removing anything here is a MAJOR bump.
int      fw_printf(const char *fmt, ...);
uint32_t fw_millis(void);
void    *fw_malloc(size_t n);
void     fw_free(void *p);

// The entry point every app must define.
int app_main(int arg);

#ifdef __cplusplus
}
#endif
#endif  // RPC_APP_H
