// littlefs v2 over the tail of the on-board flash.
//
// The version is pinned to v2.11 — byte-identical to the one MicroPython's rp2
// port builds — so the on-disk format matches what a v1.0 device already has.
// That is what keeps an in-place migration open rather than requiring every
// device to be wiped. Format compatibility is necessary but not sufficient: the
// GEOMETRY below (block size, count, offset) has to match the layout an existing
// device was formatted with, and that must be read off a real device before any
// migration is promised.

#include "storage.h"
#include "lock.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "lfs.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

// Last 512 KB of flash. The firmware lives at the start; apps live at the end.
// One lock for the whole filesystem.
//
// littlefs keeps bookkeeping in RAM while a file is open, and two tasks writing
// at once interleave those updates and corrupt it. This is the thing that made
// the scheduler cooperative until now: one task at a time inside here, everyone
// else waits their turn.
//
// It is held for a whole OPERATION, not per call — storage_copy streams a file
// in chunks and must not have another task open something halfway through. The
// lock is recursive, so an operation built out of other operations still works.
RpcLock g_fs_lock;

// How much flash the FIRMWARE gets. Everything after it is the filesystem.
//
// A fixed reserve rather than "wherever the binary happens to end". The end of
// the binary moves every time the firmware grows, and if the filesystem started
// there, an update that added a few kilobytes would shift the whole filesystem
// and lose every file on it. A fixed boundary means an update is safe as long as
// the firmware still fits under it, and kboot checks exactly that.
//
// 1.5 MB against a current binary of ~750 KB.
//
// Sized for OTA rather than for today. An update downloads the new image to the
// filesystem and then writes it over this region, so the reserve has to hold an
// image with room to grow — and an image that outgrew its reserve mid-update
// would be discovered by overwriting the start of the filesystem.
//
// It leaves 2.5 MB of filesystem on a 4 MB board. Raising it MOVES where the
// filesystem starts, so the first boot after this change finds none and builds
// a fresh one: accounts, settings and installed packages go once. Paid
// deliberately now rather than after anyone is relying on the contents.
#ifndef RPC_FW_RESERVE
#define RPC_FW_RESERVE (1536 * 1024)
#endif

#define FS_OFFSET      RPC_FW_RESERVE
#define FS_SIZE        (PICO_FLASH_SIZE_BYTES - FS_OFFSET)
#define FS_BLOCK_SIZE  FLASH_SECTOR_SIZE          // 4096 — the erase unit
#define FS_PROG_SIZE   FLASH_PAGE_SIZE            // 256  — the program unit

static lfs_t  g_lfs;
static bool   g_mounted;

static int fs_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size) {
    (void)c;
    // XIP: flash is memory-mapped for reading, so this is a memcpy.
    const uint8_t *src = (const uint8_t *)(XIP_BASE + FS_OFFSET +
                                           block * FS_BLOCK_SIZE + off);
    memcpy(buffer, src, size);
    return 0;
}

// Flash writes and the second core.
//
// save_and_disable_interrupts is NOT enough once two cores are running. The SDK
// is explicit that an erase or program is unsafe while the other core executes
// from flash: XIP is unavailable for the duration, so the other core stalls
// mid-instruction-fetch and the operation is left half finished. That is a hard
// lockup AND a corrupted filesystem — recoverable only by erasing the chip,
// which is exactly the failure the stress test kept producing once core 1 came
// online.
//
// flash_safe_execute parks the other core in RAM first, then runs the operation
// with interrupts off. On a single-core build it costs nothing.
struct ProgArgs { uint32_t addr; const uint8_t *data; uint32_t len; };
struct EraseArgs { uint32_t addr; uint32_t len; };

static void do_prog(void *v) {
    ProgArgs *a = (ProgArgs *)v;
    flash_range_program(a->addr, a->data, a->len);
}
static void do_erase(void *v) {
    EraseArgs *a = (EraseArgs *)v;
    flash_range_erase(a->addr, a->len);
}

// The fallback matters: flash_safe_execute returns an error rather than acting
// if the other core has not registered as a lockout victim. Doing the operation
// anyway would be the original bug; refusing it corrupts nothing and littlefs
// reports a write error, which is recoverable.
static int guarded(void (*fn)(void *), void *args) {
    int rc = flash_safe_execute(fn, args, 2000);
    if (rc == PICO_OK) return 0;
    return -1;
}

static int fs_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size) {
    (void)c;
    ProgArgs a{FS_OFFSET + block * FS_BLOCK_SIZE + off, (const uint8_t *)buffer, size};
    return guarded(do_prog, &a) == 0 ? 0 : LFS_ERR_IO;
}

static int fs_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    EraseArgs a{FS_OFFSET + block * FS_BLOCK_SIZE, FS_BLOCK_SIZE};
    return guarded(do_erase, &a) == 0 ? 0 : LFS_ERR_IO;
}

static int fs_sync(const struct lfs_config *c) { (void)c; return 0; }

static uint8_t g_read_buf[FS_PROG_SIZE];
static uint8_t g_prog_buf[FS_PROG_SIZE];
static uint8_t g_lookahead[16] __attribute__((aligned(8)));

static struct lfs_config g_cfg_live;

bool storage_init(bool format_if_needed) {
    // Field by field rather than a positional initialiser: lfs_config gains
    // members between releases, and a positional list silently shifts every
    // value along when it does.
    memset(&g_cfg_live, 0, sizeof(g_cfg_live));
    g_cfg_live.read  = fs_read;
    g_cfg_live.prog  = fs_prog;
    g_cfg_live.erase = fs_erase;
    g_cfg_live.sync  = fs_sync;
    g_cfg_live.read_size      = FS_PROG_SIZE;
    g_cfg_live.prog_size      = FS_PROG_SIZE;
    g_cfg_live.block_size     = FS_BLOCK_SIZE;
    g_cfg_live.block_count    = FS_SIZE / FS_BLOCK_SIZE;
    g_cfg_live.block_cycles   = 500;
    g_cfg_live.cache_size     = FS_PROG_SIZE;
    g_cfg_live.lookahead_size = sizeof(g_lookahead);
    g_cfg_live.read_buffer = g_read_buf;
    g_cfg_live.prog_buffer = g_prog_buf;
    g_cfg_live.lookahead_buffer = g_lookahead;

    int err = lfs_mount(&g_lfs, &g_cfg_live);
    if (err && format_if_needed) {
        printf("  no filesystem found, formatting %u KB...\n", FS_SIZE / 1024);
        if (lfs_format(&g_lfs, &g_cfg_live) < 0) return false;
        err = lfs_mount(&g_lfs, &g_cfg_live);
    }
    g_mounted = (err == 0);
    return g_mounted;
}

// --- modification times -----------------------------------------------------
//
// littlefs v2 stores no timestamps, so they are kept as a custom attribute: type
// ATTR_MTIME, four bytes of Unix epoch. Every path that creates or changes a
// file must stamp it, because a listing where SOME entries have a date is worse
// than one where none do — the missing ones read as corruption rather than as an
// unsupported feature.
//
// rpc_now_epoch is a seam like rpc_rand32: the device backs it with the
// always-on clock, the host test with its own source, so storage.cpp stays free
// of hardware headers.
#define ATTR_MTIME 't'

extern "C" uint32_t rpc_now_epoch(void);

static void touch(const char *path) {
    uint32_t now = rpc_now_epoch();
    if (!now) return;                  // clock never set: leave it unstamped
    lfs_setattr(&g_lfs, path, ATTR_MTIME, &now, sizeof(now));
}

uint32_t storage_mtime(const char *path) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return 0;
    uint32_t t = 0;
    if (lfs_getattr(&g_lfs, path, ATTR_MTIME, &t, sizeof(t)) != (lfs_ssize_t)sizeof(t))
        return 0;
    return t;
}

bool storage_write_file(const char *name, const uint8_t *data, uint32_t len) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return false;
    lfs_file_t f;
    if (lfs_file_open(&g_lfs, &f, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0)
        return false;
    lfs_ssize_t n = lfs_file_write(&g_lfs, &f, data, len);
    lfs_file_close(&g_lfs, &f);
    if (n != (lfs_ssize_t)len) return false;
    touch(name);
    return true;
}

bool storage_append_file(const char *name, const uint8_t *data, uint32_t len) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return false;
    lfs_file_t f;
    if (lfs_file_open(&g_lfs, &f, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND) < 0)
        return false;
    lfs_ssize_t n = lfs_file_write(&g_lfs, &f, data, len);
    lfs_file_close(&g_lfs, &f);
    if (n != (lfs_ssize_t)len) return false;
    touch(name);
    return true;
}

uint32_t storage_read_file(const char *name, uint8_t *buf, uint32_t cap) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return 0;
    lfs_file_t f;
    if (lfs_file_open(&g_lfs, &f, name, LFS_O_RDONLY) < 0) return 0;
    lfs_ssize_t n = lfs_file_read(&g_lfs, &f, buf, cap);
    lfs_file_close(&g_lfs, &f);
    return n > 0 ? (uint32_t)n : 0;
}

// --- AppSource over a littlefs file ----------------------------------------
// Random access straight out of the file rather than slurping it into RAM. An
// app ELF is a few kilobytes here, but the loader is meant to survive one that
// is not, and reading it whole would double the peak RAM cost of loading.
struct FileHandle { lfs_file_t f; };

static int lfs_source_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FileHandle *h = (FileHandle *)ctx;
    if (lfs_file_seek(&g_lfs, &h->f, off, LFS_SEEK_SET) < 0) return -1;
    lfs_ssize_t n = lfs_file_read(&g_lfs, &h->f, dst, len);
    return (int)n;
}

bool storage_open_source(const char *name, AppSource *src, void **handle) {
    if (!g_mounted) return false;
    FileHandle *h = (FileHandle *)malloc(sizeof(FileHandle));
    if (!h) return false;
    if (lfs_file_open(&g_lfs, &h->f, name, LFS_O_RDONLY) < 0) { free(h); return false; }
    struct lfs_info info;
    uint32_t size = 0;
    if (lfs_stat(&g_lfs, name, &info) >= 0) size = info.size;
    src->ctx = h;
    src->read = lfs_source_read;
    src->size = size;
    *handle = h;
    return true;
}

// The name is kept so the mtime can be stamped at close, the same as the
// whole-file writers do.
struct SinkHandle { lfs_file_t f; char name[64]; bool ok; };

void *storage_open_sink(const char *name) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return nullptr;
    SinkHandle *h = (SinkHandle *)malloc(sizeof(SinkHandle));
    if (!h) return nullptr;
    snprintf(h->name, sizeof(h->name), "%s", name);
    h->ok = true;
    if (lfs_file_open(&g_lfs, &h->f, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) {
        free(h);
        return nullptr;
    }
    return h;
}

bool storage_sink_write(void *handle, const uint8_t *data, uint32_t len) {
    if (!handle) return false;
    SinkHandle *h = (SinkHandle *)handle;
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) { h->ok = false; return false; }
    lfs_ssize_t n = lfs_file_write(&g_lfs, &h->f, data, len);
    if (n != (lfs_ssize_t)len) { h->ok = false; return false; }
    return true;
}

bool storage_close_sink(void *handle) {
    if (!handle) return false;
    SinkHandle *h = (SinkHandle *)handle;
    bool ok;
    {
        LockGuard _fs(&g_fs_lock);
        // The close is where littlefs actually commits, so its result matters
        // as much as any write's — a download that filled the disk fails here.
        ok = g_mounted && lfs_file_close(&g_lfs, &h->f) >= 0 && h->ok;
    }
    if (ok) touch(h->name);
    free(h);
    return ok;
}

void storage_close_source(void *handle) {
    if (!handle) return;
    FileHandle *h = (FileHandle *)handle;
    lfs_file_close(&g_lfs, &h->f);
    free(h);
}

void storage_list(void) {
    if (!g_mounted) { printf("  (no filesystem)\n"); return; }
    lfs_dir_t dir;
    if (lfs_dir_open(&g_lfs, &dir, "/") < 0) { printf("  (cannot open /)\n"); return; }
    struct lfs_info info;
    int n = 0;
    while (lfs_dir_read(&g_lfs, &dir, &info) > 0) {
        if (info.type == LFS_TYPE_REG) {
            printf("  %-20s %6lu B\n", info.name, (unsigned long)info.size);
            n++;
        }
    }
    lfs_dir_close(&g_lfs, &dir);
    if (!n) printf("  (empty -- use `put <name> <len>` to upload an app)\n");
}

bool storage_mkdir(const char *path) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted || lfs_mkdir(&g_lfs, path) < 0) return false;
    touch(path);
    return true;
}

bool storage_remove(const char *path) {
    LockGuard _fs(&g_fs_lock);
    return g_mounted && lfs_remove(&g_lfs, path) >= 0;
}

bool storage_rename(const char *from, const char *to) {
    LockGuard _fs(&g_fs_lock);
    // No touch(): littlefs carries the attributes across, and mv changes a name
    // rather than content — refreshing the timestamp would misreport it.
    return g_mounted && lfs_rename(&g_lfs, from, to) >= 0;
}

bool storage_stat(const char *path, bool *is_dir, uint32_t *size) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return false;
    struct lfs_info info;
    // The root always exists but does not stat on littlefs; report it directly.
    if (path[0] == '/' && path[1] == 0) { if (is_dir) *is_dir = true; if (size) *size = 0; return true; }
    if (lfs_stat(&g_lfs, path, &info) < 0) return false;
    if (is_dir) *is_dir = (info.type == LFS_TYPE_DIR);
    if (size)   *size = info.size;
    return true;
}

bool storage_copy(const char *from, const char *to) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return false;
    lfs_file_t in, out;
    if (lfs_file_open(&g_lfs, &in, from, LFS_O_RDONLY) < 0) return false;
    if (lfs_file_open(&g_lfs, &out, to, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) {
        lfs_file_close(&g_lfs, &in);
        return false;
    }
    uint8_t chunk[256];
    bool ok = true;
    while (true) {
        lfs_ssize_t n = lfs_file_read(&g_lfs, &in, chunk, sizeof(chunk));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (lfs_file_write(&g_lfs, &out, chunk, n) != n) { ok = false; break; }
    }
    lfs_file_close(&g_lfs, &in);
    lfs_file_close(&g_lfs, &out);
    // The copy is a new file, so it gets NOW rather than the source's time —
    // matching cp without -p, which is what people expect from a bare copy.
    if (ok) touch(to);
    return ok;
}

bool storage_walk(const char *path, StorageWalkFn cb, void *ctx) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return false;
    lfs_dir_t dir;
    if (lfs_dir_open(&g_lfs, &dir, path) < 0) return false;
    struct lfs_info info;
    while (lfs_dir_read(&g_lfs, &dir, &info) > 0) {
        if (info.name[0] == '.' && (info.name[1] == 0 ||
            (info.name[1] == '.' && info.name[2] == 0))) continue;   // skip . and ..
        cb(ctx, info.name, info.type == LFS_TYPE_DIR, info.size);
    }
    lfs_dir_close(&g_lfs, &dir);
    return true;
}

// Erase and remake the filesystem, then mount it.
//
// The recovery path: used when mounting fails, and when the boot counter says
// the device has failed to reach a shell three times running. Losing the files
// is bad; needing another computer and a nuke image to make the board boot at
// all is worse, and that is what this exists to prevent.
bool storage_format_and_mount(void) {
    LockGuard _fs(&g_fs_lock);
    if (g_mounted) { lfs_unmount(&g_lfs); g_mounted = false; }
    if (lfs_format(&g_lfs, &g_cfg_live) < 0) return false;
    if (lfs_mount(&g_lfs, &g_cfg_live) < 0)  return false;
    g_mounted = true;
    return true;
}

uint32_t storage_total_bytes(void) { return FS_SIZE; }

// Where the firmware actually ends, so the boot check can compare it against the
// reserve. Provided by the linker.
extern "C" char __flash_binary_end;

uint32_t storage_firmware_bytes(void) {
    return (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
}
uint32_t storage_reserve_bytes(void) { return RPC_FW_RESERVE; }

uint32_t storage_free_bytes(void) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return 0;
    lfs_ssize_t used = lfs_fs_size(&g_lfs);
    if (used < 0) return 0;
    return FS_SIZE - (uint32_t)used * FS_BLOCK_SIZE;
}
