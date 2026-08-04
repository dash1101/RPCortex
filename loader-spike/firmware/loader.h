#ifndef RPC_LOADER_H
#define RPC_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "rpc_app.h"

// How the loader reads an app. Abstracted so the same code serves a file on
// littlefs and a buffer in flash, and so the host test can feed it a file on a
// PC without any of the device in the way.
struct AppSource {
    void *ctx;
    // Read `len` bytes at `off`. Returns bytes read, or negative on error.
    int (*read)(void *ctx, uint32_t off, void *dst, uint32_t len);
    uint32_t size;
};

enum LoadResult {
    LOAD_OK = 0,
    LOAD_ERR_READ,
    LOAD_ERR_NOT_ELF,
    LOAD_ERR_NOT_REL,
    LOAD_ERR_NOT_ARM,
    LOAD_ERR_NO_HEADER,       // missing .rpc_app_header
    LOAD_ERR_BAD_MAGIC,
    LOAD_ERR_API_MISMATCH,
    LOAD_ERR_OOM,
    LOAD_ERR_NO_ENTRY,        // no app_main
    LOAD_ERR_UNDEF_SYMBOL,    // an import the firmware does not export
    LOAD_ERR_RELOC_UNSUPPORTED,
    LOAD_ERR_RELOC_RANGE,
    LOAD_ERR_TOO_MANY_SECTIONS,
};

#define LOADER_MAX_SECTIONS 48

// Loaded blocks start on a boundary of this and are padded to a multiple of it,
// so the memory protection hardware can cover each one exactly. Both supported
// architectures protect memory in 32-byte units at minimum, so one number
// serves both. See os/core/mpu.h for what is done with them.
#define APP_BLOCK_ALIGN 32u

struct LoadedApp {
    // One allocation holding every SHF_ALLOC section, laid out in two halves:
    // everything the app may not write to first, everything it must be able to
    // write to after. `image` is the start and also the app's identity — the
    // token a registered command is tagged with, and what a fault address is
    // resolved against.
    void    *image;
    uint32_t image_size;       // both halves together
    uint32_t text_size;        // the read-only half, from `image`
    void    *data;             // the writable half; null if the app has none
    uint32_t data_size;
    void    *veneers;          // trampoline pool (see loader.cpp)
    uint32_t veneer_size;
    uint32_t veneers_used;
    // Bytes at the START of the pool taken by the two fixed gates a sandboxed
    // package needs. Zero when packages run privileged, because then there is
    // nothing to gate.
    uint32_t veneer_gates;
    // What the allocator actually returned. The pointers above are a few bytes
    // further in, because a protected block has to start on a boundary and the
    // allocator only promises eight — and handing back a pointer the allocator
    // never issued corrupts the heap somewhere else entirely.
    void    *image_raw;
    void    *veneers_raw;
    int (*entry)(int);
    RpcAppHeader header;
    // Diagnostics for whatever failed, so an error can name the symbol or the
    // relocation type rather than just saying no.
    char     detail[48];
    uint32_t bytes_allocated;  // image + veneers + slack, for the RAM accounting
};

// How a package reaches the firmware.
//
// DIRECT loads the function's address and branches to it, which is what a
// package running with the OS's own privileges does and what every build did
// before sandboxing existed.
//
// SVC loads the function's INDEX and asks the supervisor to make the jump.
// That is the only form a sandboxed package can use, because unprivileged code
// cannot fetch instructions from flash — which is what the sandbox is.
//
// Set once at startup rather than baked in at compile time, so both forms can
// be built and checked on a host that has neither.
enum LoaderVeneerMode {
    LOADER_VENEER_DIRECT = 0,
    LOADER_VENEER_SVC,
};
void loader_set_veneer_mode(LoaderVeneerMode m);
LoaderVeneerMode loader_veneer_mode(void);

// The two fixed gates, once an app is loaded in SVC mode. Both are zero in
// DIRECT mode. See loader.cpp for what they are for; the short version is that
// the instruction which gives privilege back has to be one the package is
// allowed to execute, and flash is not.
uint32_t app_return_gate(const LoadedApp *app);
uint32_t app_exit_gate(const LoadedApp *app);

// Where the loader gets memory. Pluggable for one concrete reason: the host
// test must place an app image at a genuine 32-bit address to reproduce the
// flash-to-SRAM distance that forces veneers. On the device this is malloc.
typedef void *(*LoaderAlloc)(size_t);
typedef void  (*LoaderFree)(void *);
void loader_set_allocator(LoaderAlloc a, LoaderFree f);

// Load, relocate and resolve. Does not run anything.
LoadResult app_load(const AppSource &src, LoadedApp *out);

// Free everything app_load allocated.
void app_unload(LoadedApp *app);

const char *load_result_str(LoadResult r);

#endif  // RPC_LOADER_H
