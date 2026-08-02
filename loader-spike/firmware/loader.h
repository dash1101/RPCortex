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

struct LoadedApp {
    void    *image;            // one allocation holding every SHF_ALLOC section
    uint32_t image_size;
    void    *veneers;          // trampoline pool (see loader.cpp)
    uint32_t veneer_size;
    uint32_t veneers_used;
    int (*entry)(int);
    RpcAppHeader header;
    // Diagnostics for whatever failed, so an error can name the symbol or the
    // relocation type rather than just saying no.
    char     detail[48];
    uint32_t bytes_allocated;  // image + veneers, for the RAM accounting
};

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
