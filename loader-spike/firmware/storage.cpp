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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "lfs.h"
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

// Last 512 KB of flash. The firmware lives at the start; apps live at the end.
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

bool storage_write_file(const char *name, const uint8_t *data, uint32_t len) {
    if (!g_mounted) return false;
    lfs_file_t f;
    if (lfs_file_open(&g_lfs, &f, name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0)
        return false;
    lfs_ssize_t n = lfs_file_write(&g_lfs, &f, data, len);
    lfs_file_close(&g_lfs, &f);
    return n == (lfs_ssize_t)len;
}

uint32_t storage_read_file(const char *name, uint8_t *buf, uint32_t cap) {
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

uint32_t storage_free_bytes(void) {
    if (!g_mounted) return 0;
    lfs_ssize_t used = lfs_fs_size(&g_lfs);
    if (used < 0) return 0;
    return FS_SIZE - (uint32_t)used * FS_BLOCK_SIZE;
}
