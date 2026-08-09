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

// Sections in one .app, not bytes. Every function and every data item gets its
// own with -ffunction-sections -fdata-sections, and each one that needs fixing
// up brings a .rel section too — so this is roughly twice the number of
// functions, and it grows with the SOURCE, not with the binary.
//
// 48 was set when the biggest package was a calculator, and calc reached 47 of
// it. `havoc` needs 60 and would not load at all: "too many sections", on a
// package that was otherwise fine.
//
// Raising it is cheap because the two tables it sizes are static rather than
// stack — see app_load. The cost is .bss, paid once; the alternative was six
// kilobytes of frame on the stack of whoever happened to install a package.
#define LOADER_MAX_SECTIONS 128

// Loaded blocks start on a boundary of this and are padded to a multiple of it,
// so the memory protection hardware can cover each one exactly. Both supported
// architectures protect memory in 32-byte units at minimum, so one number
// serves both. See os/core/mpu.h for what is done with them.
#define APP_BLOCK_ALIGN 32u

struct LoadedApp {
    // TWO allocations, one per half: everything the app may not write to, and
    // everything it must be able to write to. They want opposite permissions
    // and the protection hardware covers each separately, so they were already
    // two regions in everything that consumes them — the MPU, the pointer
    // checker, the fault reporter — and only the heap thought otherwise.
    //
    // It used to be one block. A Nova D1 at 123 KB then needed 123 KB in one
    // piece, and upgrading it on a device already running it could not find
    // that: the copy being replaced is unloaded first, but the hole it leaves
    // is its own old size and the new image does not fit in it. So the free
    // total climbed and the largest block did not, and `pkg install` failed
    // with "out of memory" on a device reporting 281 KB free.
    //
    // THE TWO BLOCKS ARE NOT ADJACENT and nothing may assume they are.
    // `image + image_size` is not an address. Anything walking the whole image
    // walks two ranges.
    void    *image;            // the read-only half, and the app's identity —
                               // the token a registered command is tagged with
                               // and what a fault address is resolved against
    uint32_t image_size;       // text_size + data_size, for accounting ONLY
    uint32_t text_size;        // the read-only half, from `image`
    void    *data;             // the writable half; null if the app has none
    uint32_t data_size;
    // Position-independent packages only. The loader synthesises a GOT at the
    // BASE of the writable block, so `data` doubles as the GOT origin — which is
    // the value r9 must hold whenever package code runs. `got_size` is zero for
    // a non-PIC package, and that zero is the single flag the entry points read
    // to decide whether to touch r9 at all: the default (non-PIC) path leaves it
    // exactly as it was. See app_pic_base and the r9 notes in loader.cpp.
    uint32_t got_size;         // bytes of GOT at the start of the data block
    uint32_t got_count;        // slots used, for diagnostics
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
    void    *data_raw;
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
uint32_t app_enter_gate(const LoadedApp *app);
uint32_t app_exit_gate(const LoadedApp *app);

// Where the loader gets memory. Pluggable for one concrete reason: the host
// test must place an app image at a genuine 32-bit address to reproduce the
// flash-to-SRAM distance that forces veneers. On the device this is malloc.
typedef void *(*LoaderAlloc)(size_t);
typedef void  (*LoaderFree)(void *);
void loader_set_allocator(LoaderAlloc a, LoaderFree f);

// Load, relocate and resolve. Does not run anything.
// The header alone: name, version and ABI, with no image loaded and nothing
// relocated. The only allocation is the section-name strings, freed before it
// returns.
//
// This exists so that deciding WHAT a package is does not cost the same memory
// as running it. Installing an upgrade has to unload the copy already resident
// before it can afford to load the new one, and it cannot know which package to
// unload without reading the name first — which used to mean loading it.
LoadResult app_peek(const AppSource &src, RpcAppHeader *out);

LoadResult app_load(const AppSource &src, LoadedApp *out);

// The value r9 must hold on every entry into this package's code — the GOT
// origin, which is the base of the writable block. Zero for a non-PIC package,
// which the entry points take as "leave r9 alone". This is the ONE place the
// r9-setting convention is named; sandbox_enter, the direct-call trampoline and
// the task shim all ask here rather than reaching into the struct.
uint32_t app_pic_base(const LoadedApp *app);

// --- running a position-independent package from a flash slot (task #93 stage 3)
//
// A PIC package's read-only half — .text, .rodata and its firmware-call veneers —
// is a POSITION-INDEPENDENT blob: .text reaches its data through r9/GOT and calls
// firmware through a veneer that sits in the blob beside it, so the blob holds no
// absolute address and runs correctly wherever it lands. That is what lets it be
// written to a flash slot once and executed in place, costing no RAM. Only the
// writable half — .data, .bss and the GOT — is resident, allocated at load.
//
// The split is INSTALL then LOAD:
//   app_pic_install  assembles the blob once (host or device) and records a
//                    manifest — how to build the GOT, how to fix up the ABS32
//                    pointers in .data, the .data initialisers, and where
//                    app_main is. It is the only step that reads the ELF, so the
//                    32 KB string table and 70 KB symbol table a full parse pulls
//                    in are touched here, never at load.
//   app_pic_load     needs only the blob and the manifest — never the ELF again.
//                    It allocates the one RAM block (GOT + .data + .bss), copies
//                    the initialisers, applies the ABS32 fixups, builds the GOT
//                    and points r9 at it. That block is the whole resident cost.
//
// SVC (sandboxed) packages only. A DIRECT-mode veneer holds a raw firmware
// ADDRESS, which changes on any firmware rebuild and cannot be baked into a slot;
// a package that would run privileged takes the ordinary copy-to-RAM app_load
// path instead. The device is SVC on RP2350 (main.cpp), which is the whole point.

// Which region a resolved address is measured from. SLOT is the blob — flash on
// the device — and covers .text, .rodata and the veneers. RAM is the block
// app_pic_load allocates and covers .data, .bss and the GOT. ABS is a whole
// address already, from an SHN_ABS symbol, and is added verbatim.
enum PicClass {
    PIC_CLASS_SLOT = 0,
    PIC_CLASS_RAM  = 1,
    PIC_CLASS_ABS  = 2,
};

// One GOT slot:  got[k] = base(cls) + value. `value` already carries the Thumb
// bit where one is due — a defined function keeps it in st_value (AAELF) and a
// veneer has it set at install — so load adds base and nothing else. Adding a
// Thumb bit a second time clears it, which is the bug app_load's ABS32 case warns
// about. is_func is diagnostic only.
struct PicGotEntry {
    uint32_t value;      // final region-relative pointer, or an ABS address
    uint8_t  cls;        // PicClass
    uint8_t  is_func;    // whether the symbol is code, for diagnostics
};

// One ABS32 pointer in .data:  *(uint32_t*)(ram + site) += base(cls) + value.
// The addend is already in the copied .data word; this adds the resolved base.
// ORDER IS LOAD-BEARING: copy the pristine .data initialisers FIRST, then apply
// these ONCE. Applied twice — or to a block whose initialisers were not re-copied
// — an ABS32 doubles an address, which reads exactly like heap corruption.
struct PicAbs32 {
    uint32_t site;       // byte offset of the word within the RAM block
    uint32_t value;      // offset within the class's region, or an ABS address
    uint8_t  cls;        // PicClass
};

// Everything app_pic_load needs and the ELF no longer has to be read for. On the
// device this is serialised into the slot header; on the host it is held in RAM.
// The arrays are owned by the manifest — app_pic_manifest_free releases them.
struct PicManifest {
    uint16_t api_major, api_minor;   // refuse a slot the running ABI cannot honour
    uint32_t ro_size;                // blob bytes: .text + .rodata + veneers
    uint32_t text_size;              // .text length within the blob
    uint32_t rodata_off, rodata_size;
    uint32_t veneer_off, veneer_size;
    uint32_t entry_off;              // app_main, offset into the blob (Thumb added at load)
    uint32_t got_bytes;              // GOT size, at the base of the RAM block (r9)
    uint32_t data_off;               // where .data lands in the RAM block
    uint32_t data_size;              // .data initialiser length
    uint32_t ram_size;               // GOT + .data + .bss, block-aligned
    uint32_t got_count, abs_count;
    PicGotEntry *got;                // [got_count]
    PicAbs32    *abs;                // [abs_count]
    uint8_t     *data_init;          // [data_size]
};

// The sink the blob is streamed to as it is assembled: on the host it appends to
// a buffer, on the device it programs a flash slot a chunk at a time — so the
// 122 KB read-only half is never resident in one piece, which is what lets an
// install run on a board whose largest free block is 89 KB. Writes arrive in
// ascending, non-overlapping offset order. Returns false to abort the install.
typedef bool (*SlotWrite)(void *ctx, uint32_t off, const void *data, uint32_t len);

// Assemble the read-only blob into `sink` and fill `m`. SVC veneer mode only.
LoadResult app_pic_install(const AppSource &src, SlotWrite sink, void *sink_ctx,
                           PicManifest *m);

// Instantiate a package whose blob is already at `slot` (a flash slot on the
// device, the assembled buffer on the host), using the manifest. Allocates the
// RAM block and the gate pool; sets image=slot, data=RAM, entry, r9 base.
LoadResult app_pic_load(const void *slot, const PicManifest *m, LoadedApp *out);

// Release the arrays app_pic_install allocated inside a manifest.
void app_pic_manifest_free(PicManifest *m);

// Free everything app_load allocated.
void app_unload(LoadedApp *app);

const char *load_result_str(LoadResult r);

#endif  // RPC_LOADER_H
