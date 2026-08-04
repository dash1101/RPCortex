// The USB drive: a real FAT12 volume in its own flash region.
//
// This replaced a view synthesised over littlefs, and the reasons are worth
// keeping because they were all found the hard way.
//
// A synthesised view has to work out what a raw sector write MEANT. A host does
// not say "create this file" — it writes some clusters, some table entries and
// a directory entry, in an order of its choosing, and the code underneath has
// to infer intent from that. It can only ever recognise the cases it was taught,
// so edits did not stick, deletes were refused, and the two sides drifted apart
// with nothing able to say why. Worse, honouring a write meant calling into
// littlefs from inside the USB stack, which put the filesystem lock inside the
// console's — and since the shell prints while holding the filesystem lock, the
// two orders met and the device livelocked. A three kilobyte file was enough.
//
// A real volume has none of that. The host mounts it, owns it, and creates,
// renames, edits and deletes exactly as it would on a memory card, because
// nothing here is interpreting anything: a sector write is a sector write. What
// the device gives up is seeing its whole filesystem from the host; what it
// gets back is a transfer area that cannot desynchronise, because there is only
// one copy of the data and both sides read the same bytes.
//
// The region is the firmware staging slot, borrowed. It is the only megabyte on
// the chip not already spoken for, and using it means littlefs does not move —
// which matters, because moving where the filesystem starts costs every device
// its files.

#include "fat12.h"
#include "fatview.h"
#include "lock.h"
#include "out.h"
#include "registry.h"
#include "storage.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#include "tusb.h"

#if CFG_TUD_MSC

// Serialises the region against the shell's own use of it.
//
// NOT the filesystem lock, deliberately. Taking that one here is what created
// the inversion that livelocked the device, and this region is not littlefs, so
// there is nothing about it the filesystem lock protects.
//
// The rule that keeps this one safe is short: NOTHING PRINTS WHILE HOLDING IT.
// The console path takes the SDK's stdio mutex, the USB task holds that mutex
// while it services the device stack, and a lock taken on both sides of that
// boundary in opposite orders is a deadlock. Every operation below gathers what
// it needs under the lock and leaves printing to its caller.
static RpcLock g_usb_lock;

// How many times a callback may answer "not ready" before it gives up.
//
// Returning zero from a read or write callback asks TinyUSB to call it again —
// but tud_task loops until its queue is empty and the retry is queued
// immediately, so "again" means inside the same call, on the same core, without
// yielding. If whatever holds the lock is a task on that core, it never gets to
// run and the retry never succeeds: the exact livelock this design was rebuilt
// to remove, reappearing one level down.
//
// So the retry is bounded. A few dozen turns costs microseconds and covers
// every short-lived hold; past that the operation fails, the host retries the
// command later, and the device stays responsive. A failed read is recoverable
// and a hung one is not.
#define MSC_BUSY_LIMIT 64
static uint32_t g_busy;

static F12  g_vol;
static bool g_enabled = true;
static bool g_formatted;

#define MSC_REG_KEY "USB.Drive"

// --- the block layer --------------------------------------------------------
//
// Flash erases in 4 KB blocks and a host writes in 512-byte sectors, so a naive
// write would erase and rewrite a whole block eight times over for one
// contiguous kilobyte. One block is held in memory instead: writes land there,
// and it is pushed out when the host moves on or when the transfer settles.
//
// The flush deliberately does NOT happen inside the MSC callback where it can
// be avoided. A callback runs inside the USB device stack, which runs inside
// the console's mutex; an erase there stalls the console for as long as it
// takes. usbmsc_service() runs on the same task a moment earlier, outside all
// of that, and that is where the common case is paid.
#define BLK STORAGE_USB_BLOCK
#define SECTORS_PER_BLK (BLK / F12_SECTOR)

static uint8_t  g_blk[BLK];
static uint32_t g_blk_no = 0xFFFFFFFF;
static bool     g_blk_dirty;

static bool blk_flush(void) {
    if (g_blk_no == 0xFFFFFFFF || !g_blk_dirty) return true;
    bool ok = storage_usb_write_block(g_blk_no * BLK, g_blk);
    g_blk_dirty = false;
    return ok;
}

static bool blk_load(uint32_t block) {
    if (g_blk_no == block) return true;
    if (!blk_flush()) return false;
    if (!storage_usb_read(block * BLK, g_blk, BLK)) return false;
    g_blk_no = block;
    return true;
}

static bool region_read(void *, uint32_t lba, void *buf) {
    if ((lba + 1) * F12_SECTOR > storage_usb_bytes()) return false;
    uint32_t block = lba / SECTORS_PER_BLK;
    // The cached block may hold writes flash has not seen yet, so it answers
    // for its own sectors. Reading around it would hand the host back the bytes
    // that were there before its own write.
    if (block == g_blk_no) {
        memcpy(buf, g_blk + (lba % SECTORS_PER_BLK) * F12_SECTOR, F12_SECTOR);
        return true;
    }
    return storage_usb_read(lba * F12_SECTOR, buf, F12_SECTOR);
}

static bool region_write(void *, uint32_t lba, const void *buf) {
    if ((lba + 1) * F12_SECTOR > storage_usb_bytes()) return false;
    if (!blk_load(lba / SECTORS_PER_BLK)) return false;
    memcpy(g_blk + (lba % SECTORS_PER_BLK) * F12_SECTOR, buf, F12_SECTOR);
    g_blk_dirty = true;
    return true;
}

static F12Io region_io(void) {
    F12Io io;
    io.ctx = nullptr;
    io.read = region_read;
    io.write = region_write;
    io.sectors = storage_usb_bytes() / F12_SECTOR;
    return io;
}

// Bring the volume up, formatting when what is there is not one.
//
// After an update the region holds part of a firmware image, and on a new
// device it holds erased flash. Neither is a filesystem, and both are the
// normal case rather than an error — so failing to mount means format, not
// complain.
static bool ensure_volume(void) {
    if (g_formatted) return true;
    F12Io io = region_io();
    if (io.sectors < 64) return false;
    if (f12_mount(&g_vol, &io)) { g_formatted = true; return true; }
    if (!f12_format(&g_vol, &io, "RPCORTEX")) return false;
    blk_flush();
    g_formatted = true;
    return true;
}

// --- what the shell calls ---------------------------------------------------
//
// Each of these locks, does its work, and returns. None of them print: see the
// note on g_usb_lock.

bool usbdrv_ready(void) {
    LockGuard _lk(&g_usb_lock);
    return ensure_volume();
}

bool usbdrv_list(F12WalkFn cb, void *ctx) {
    LockGuard _lk(&g_usb_lock);
    if (!ensure_volume()) return false;
    return f12_list(&g_vol, cb, ctx);
}

bool usbdrv_find(const char *name, F12Entry *out) {
    LockGuard _lk(&g_usb_lock);
    if (!ensure_volume()) return false;
    return f12_find(&g_vol, name, out);
}

bool usbdrv_remove(const char *name) {
    LockGuard _lk(&g_usb_lock);
    if (!ensure_volume()) return false;
    bool ok = f12_remove(&g_vol, name);
    blk_flush();
    return ok;
}

bool usbdrv_format(void) {
    LockGuard _lk(&g_usb_lock);
    F12Io io = region_io();
    g_formatted = false;
    if (!f12_format(&g_vol, &io, "RPCORTEX")) return false;
    blk_flush();
    g_formatted = true;
    return true;
}

void usbdrv_space(uint32_t *total, uint32_t *freebytes) {
    LockGuard _lk(&g_usb_lock);
    if (!ensure_volume()) { if (total) *total = 0; if (freebytes) *freebytes = 0; return; }
    if (total) *total = f12_total_bytes(&g_vol);
    if (freebytes) *freebytes = f12_free_bytes(&g_vol);
}

// Copy a file OUT of the drive and into the filesystem.
//
// The lock is taken PER SECTOR rather than across the copy, and that is about
// keeping the USB side alive rather than about correctness. Writing into
// littlefs takes the filesystem lock and erases flash, so holding the drive's
// lock across it would block the MSC callbacks for as long as a large copy
// takes — and a blocked callback spins rather than waiting. Microsecond holds
// mean the host never notices.
bool usbdrv_import(const char *name, const char *dest) {
    F12Entry e;
    {
        LockGuard _lk(&g_usb_lock);
        if (!ensure_volume()) return false;
        if (!f12_find(&g_vol, name, &e) || e.is_dir) return false;
    }

    void *sink = storage_open_sink(dest);
    if (!sink) return false;

    // A sector at a time, so the size of the file is not also the size of the
    // buffer it needs. The transfer area exists to carry things the device
    // cannot hold in memory, which is most of the point of it.
    uint8_t buf[F12_SECTOR];
    uint32_t off = 0;
    bool ok = true;
    while (off < e.size && ok) {
        uint32_t n;
        {
            LockGuard _lk(&g_usb_lock);
            n = f12_read(&g_vol, &e, off, buf, sizeof(buf));
        }
        if (!n) { ok = false; break; }
        ok = storage_sink_write(sink, buf, n);
        off += n;
    }
    if (!storage_close_sink(sink)) ok = false;
    if (!ok) storage_remove(dest);
    return ok;
}

// Copy a file INTO the drive from the filesystem.
struct CopyIn { AppSource src; };

static uint32_t copy_in(void *ctx, uint32_t off, void *buf, uint32_t len) {
    CopyIn *c = (CopyIn *)ctx;
    int n = c->src.read(c->src.ctx, off, buf, len);
    return n < 0 ? 0 : (uint32_t)n;
}

bool usbdrv_export(const char *path, const char *name) {
    LockGuard _lk(&g_usb_lock);
    if (!ensure_volume()) return false;

    bool is_dir = false;
    uint32_t size = 0;
    if (!storage_stat(path, &is_dir, &size) || is_dir) return false;
    if (size > f12_free_bytes(&g_vol)) return false;

    CopyIn c;
    void *handle = nullptr;
    if (!storage_open_source(path, &c.src, &handle)) return false;

    bool ok = f12_write(&g_vol, name, size, storage_mtime(path), copy_in, &c);
    storage_close_source(handle);
    blk_flush();
    return ok;
}

// --- the toggle -------------------------------------------------------------

bool usbmsc_enabled(void) { return g_enabled; }

void usbmsc_set_enabled(bool on) {
    if (on == g_enabled) return;
    g_enabled = on;
    if (!on) { LockGuard _lk(&g_usb_lock); blk_flush(); }
    reg_set(MSC_REG_KEY, on ? "on" : "off");
}

void usbmsc_init(void) {
    const char *v = reg_get(MSC_REG_KEY, "on");
    g_enabled = !(v && (v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F'));
}

// Give the region up so an update can stage a firmware image into it.
//
// Returns whether the drive was holding anything, so the caller can say so.
// After this the volume is gone: the next service turn finds a region that is
// not a filesystem and formats a fresh one, which is the same path a new device
// takes.
bool usbdrv_release_for_update(void) {
    LockGuard _lk(&g_usb_lock);
    bool had_files = false;
    if (g_formatted) {
        // Only the fact that there WAS something, not what. The caller prints,
        // and printing under this lock is what must not happen.
        struct Counter { uint32_t n; } c{0};
        f12_list(&g_vol, [](void *ctx, const F12Entry *) {
            ((Counter *)ctx)->n++;
        }, &c);
        had_files = c.n > 0;
    }
    g_blk_dirty = false;          // whatever was pending is about to be overwritten
    g_blk_no = 0xFFFFFFFF;
    g_formatted = false;
    return had_files;
}

// --- servicing --------------------------------------------------------------

extern "C" void usbmsc_service(void) {
    if (!g_enabled) return;

    // Bring the volume up here rather than in a callback: formatting a region
    // that holds a firmware image writes the whole thing, and this runs on the
    // usb task before the device stack is entered.
    if (!g_formatted) {
        if (!lock_try(&g_usb_lock)) return;
        ensure_volume();
        lock_release(&g_usb_lock);
        return;
    }

    // Push a settled write out, still outside the device stack, so the erase
    // does not happen with the console's mutex held.
    if (!g_blk_dirty) return;
    if (!lock_try(&g_usb_lock)) return;      // the shell has it; the next turn will do
    blk_flush();
    lock_release(&g_usb_lock);
}

// --- the MSC callbacks ------------------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id,  "NovaLabs", 8);
    memcpy(product_id, "RPCortex Drive  ", 16);
    memcpy(product_rev, "2.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    // Not ready until the volume is up, which happens on the usb task. The MSC
    // layer answers "medium not present", TinyUSB advertises the device as
    // removable, and hosts poll removable media rather than giving up on it.
    return g_enabled && g_formatted;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_size  = F12_SECTOR;
    *block_count = (g_enabled && g_formatted) ? storage_usb_bytes() / F12_SECTOR : 0;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return g_enabled;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    (void)lun;
    if (!g_enabled || !g_formatted) return -1;
    if (offset >= F12_SECTOR) return 0;

    // lock_try rather than a blocking acquire. Blocking here would yield with
    // the console's mutex held, which is the inversion this whole design exists
    // to avoid; returning zero asks the host to come back, and whatever shell
    // operation holds the lock finishes in microseconds.
    if (!lock_try(&g_usb_lock)) return ++g_busy < MSC_BUSY_LIMIT ? 0 : (g_busy = 0, -1);
    g_busy = 0;
    uint8_t sec[F12_SECTOR];
    bool ok = region_read(nullptr, lba, sec);
    lock_release(&g_usb_lock);
    if (!ok) return -1;

    uint32_t n = F12_SECTOR - offset;
    if (n > bufsize) n = bufsize;
    memcpy(buffer, sec + offset, n);
    return (int32_t)n;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (!g_enabled || !g_formatted) return -1;
    // Whole sectors only. The endpoint buffer is a full sector so a partial one
    // does not arise, and refusing beats half-writing the cached block.
    if (offset != 0 || bufsize < F12_SECTOR) return (int32_t)bufsize;

    if (!lock_try(&g_usb_lock)) return ++g_busy < MSC_BUSY_LIMIT ? 0 : (g_busy = 0, -1);
    g_busy = 0;
    bool ok = region_write(nullptr, lba, buffer);
    lock_release(&g_usb_lock);
    return ok ? (int32_t)F12_SECTOR : -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize) {
    (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;

    case 0x35:   // SYNCHRONIZE CACHE (10), which TinyUSB does not name
    case 0x91:   // SYNCHRONIZE CACHE (16)
        // The host asking for its writes to be durable. This is the one place
        // an erase inside the device stack cannot be avoided, and also the one
        // place the host is already expecting to wait.
        if (lock_try(&g_usb_lock)) { blk_flush(); lock_release(&g_usb_lock); }
        return 0;

    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject && !start && lock_try(&g_usb_lock)) {
        blk_flush();
        lock_release(&g_usb_lock);
    }
    return true;
}

extern "C" void tud_umount_cb(void) {
    if (lock_try(&g_usb_lock)) { blk_flush(); lock_release(&g_usb_lock); }
}

#endif // CFG_TUD_MSC
