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

#define FS_SIZE        (512 * 1024)
#define FS_OFFSET      (PICO_FLASH_SIZE_BYTES - FS_SIZE)
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

static int fs_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size) {
    (void)c;
    // Writing flash must not race the XIP cache or a second core. Interrupts
    // off for the duration; the SDK handles the cache.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(FS_OFFSET + block * FS_BLOCK_SIZE + off,
                        (const uint8_t *)buffer, size);
    restore_interrupts(ints);
    return 0;
}

static int fs_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FS_OFFSET + block * FS_BLOCK_SIZE, FS_BLOCK_SIZE);
    restore_interrupts(ints);
    return 0;
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

uint32_t storage_total_bytes(void) { return FS_SIZE; }

uint32_t storage_free_bytes(void) {
    LockGuard _fs(&g_fs_lock);
    if (!g_mounted) return 0;
    lfs_ssize_t used = lfs_fs_size(&g_lfs);
    if (used < 0) return 0;
    return FS_SIZE - (uint32_t)used * FS_BLOCK_SIZE;
}
