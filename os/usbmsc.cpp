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

#include "blockcache.h"
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
static bool g_formatted;
static bool g_open;
static bool g_media_changed;

// Whether `download` is running.
//
// The volume itself is ALWAYS present — an empty, read-only megabyte named
// RPCORTEX — and this only says whether it is open for business.
//
// Making the medium come and go was the obvious reading of "hide it until
// download mode", and it was slow enough to be unusable. A host that has been
// told there is no medium backs off polling for one, so the drive took the best
// part of a minute to appear; and it caches the volume it last saw, so after
// closing it went on showing files that were no longer there. Neither is
// something the device can hurry along: both are the host's own timers.
//
// An empty volume gives nothing away — there is nothing on it to see — while
// staying mounted, which is what makes entering download mode instant. What
// changes on entry is the CONTENTS, and a media-change notice is how the host
// is told to look again.
// (declared above, next to g_formatted)

// Set when the filesystem could not take what the host wrote.
//
// The drive itself has room — the FAT reports its own free space and the host
// respects it — but a file is only useful once it has been copied off the drive
// and into the filesystem, and THAT can run out first. Telling the host the
// volume is write protected is the one signal in the protocol that means "stop
// sending"; the alternative is accepting files that quietly never arrive
// anywhere.
static bool g_full;

#define MSC_REG_KEY "USB.Drive"

// Where files dropped on the drive end up, so they are usable rather than
// stranded in a transfer area nothing else can read.
#define USB_INBOX "/usb"

// The volume is offered whenever it exists, open or not.
static bool usb_offered(void) { return g_formatted; }

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

static uint8_t    g_blk[BLK];
static BlockCache g_cache;
static bool       g_cache_ready;

static bool flash_read_block(void *, uint32_t block, void *dst) {
    return storage_usb_read(block * BLK, dst, BLK);
}
static bool flash_write_block(void *, uint32_t block, const void *src) {
    return storage_usb_write_block(block * BLK, (const uint8_t *)src);
}

static void cache_start(void) {
    if (g_cache_ready) return;
    bc_init(&g_cache, g_blk, BLK, F12_SECTOR, nullptr, flash_read_block, flash_write_block);
    g_cache_ready = true;
}

static bool blk_flush(void) { cache_start(); return bc_flush(&g_cache); }

static bool region_read(void *, uint32_t lba, void *buf) {
    if ((lba + 1) * F12_SECTOR > storage_usb_bytes()) return false;
    cache_start();
    return bc_read_sector(&g_cache, lba, buf);
}

static bool region_write(void *, uint32_t lba, const void *buf) {
    if ((lba + 1) * F12_SECTOR > storage_usb_bytes()) return false;
    cache_start();
    return bc_write_sector(&g_cache, lba, buf);
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

bool usbmsc_enabled(void) { return g_open; }
bool usbmsc_full(void) { return g_full; }

// Open the drive, empty.
//
// Formatted on the way in rather than on the way out, so a session that ended
// in a reset or a pulled cable cannot leave yesterday's files sitting on a
// drive somebody else plugs in. It costs three flash blocks and it means the
// state at the start of every session is the same one.
// Empty the volume and tell the host to look again.
static bool wipe_locked(void) {
    F12Io io = region_io();
    g_formatted = false;
    if (!f12_format(&g_vol, &io, "RPCORTEX")) return false;
    blk_flush();
    g_formatted = true;
    g_media_changed = true;
    return true;
}

bool usbmsc_open(void) {
    LockGuard _lk(&g_usb_lock);
    g_full = false;
    // Emptied on the way IN. A session that ended in a reset or a pulled cable
    // cannot leave yesterday's files on a drive somebody else plugs in.
    if (!wipe_locked()) return false;
    g_open = true;
    return true;
}

void usbmsc_close(void) {
    LockGuard _lk(&g_usb_lock);
    g_open = false;
    // Emptied on the way out too, so what the host sees between sessions
    // matches what it is allowed to have: nothing. Everything on it has already
    // been copied into /usb by the time this runs.
    wipe_locked();
}

void usbmsc_init(void) { g_open = false; }

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
    // Dropped rather than flushed: the region is about to be overwritten with a
    // firmware image, and writing this back would leave stale bytes in the
    // middle of it.
    cache_start();
    bc_discard(&g_cache);
    g_formatted = false;
    return had_files;
}

// --- taking what the host dropped -------------------------------------------
//
// A file sitting in the transfer area is not much use: nothing else on the
// device can read it, because the area is FAT and everything else is littlefs.
// So anything the host leaves there is copied into /usb, where `cat`, `pkg`,
// the editor and every other command can reach it.
//
// COPIED, not moved. A file that vanished from the drive the moment it finished
// copying would look like a failed transfer, and the host still has it cached
// as present anyway. It stays until it is deleted from either side.
//
// Nothing is imported twice: a name that already exists in /usb at the same
// size is left alone. That makes a scan idempotent, which matters because it
// runs on a timer rather than on an event — there is no "the host has finished"
// signal in MSC beyond a cache flush, and hosts do not always send one.

// Names seen in a scan, and what has to be decided about them.
struct ScanItem { char name[F12_MAXNAME]; uint32_t size; };
#define SCAN_MAX 24
struct Scan { ScanItem item[SCAN_MAX]; uint32_t n; };

static void scan_cb(void *ctx, const F12Entry *e) {
    Scan *s = (Scan *)ctx;
    if (e->is_dir || s->n >= SCAN_MAX) return;
    snprintf(s->item[s->n].name, sizeof(s->item[s->n].name), "%s", e->name);
    s->item[s->n].size = e->size;
    s->n++;
}

// When the volume last changed under the host's hand, so a scan waits for a
// copy to finish rather than importing a file the host is halfway through.
static uint32_t g_last_write_ms;
static bool     g_import_pending;
#define IMPORT_SETTLE_MS 1500

// Take everything on the drive into the filesystem. Returns how many arrived,
// and sets `refused` when something did not fit.
//
// Runs on the SHELL task, from download mode — not in the background on the usb
// task. It was in the background, and it hard-faulted: the usb task's stack
// could not hold this array plus the FAT12 call chain underneath it, and the
// device rebooted in a loop. Beyond the stack, a scan that runs on a timer is a
// scan nobody asked for, at a moment nobody chose.
uint32_t usbmsc_import_all(bool *refused) {
    if (refused) *refused = false;
    static Scan s;                 // 1.6 KB: static, not a stack frame
    s.n = 0;
    {
        LockGuard _lk(&g_usb_lock);
        if (!ensure_volume()) return 0;
        f12_list(&g_vol, scan_cb, &s);
    }
    uint32_t took = 0;

    storage_mkdir(USB_INBOX);          // harmless when it is already there
    bool any_refused = false;

    for (uint32_t i = 0; i < s.n; i++) {
        char dest[F12_MAXNAME + 8];
        snprintf(dest, sizeof(dest), USB_INBOX "/%s", s.item[i].name);

        bool is_dir = false;
        uint32_t have = 0;
        if (storage_stat(dest, &is_dir, &have) && have == s.item[i].size) continue;

        // Room, with a margin. littlefs needs somewhere to put its own metadata
        // and a filesystem filled to the last byte is one that cannot be
        // recovered from without erasing it.
        if (s.item[i].size + 16384 > storage_free_bytes()) { any_refused = true; continue; }

        if (usbdrv_import(s.item[i].name, dest)) took++;
        else any_refused = true;
    }

    // Write protection goes on when something could not be taken, and comes off
    // by itself once there is room again — the caller frees space with `rm` and
    // the next scan notices. A latch that needed clearing by hand would be one
    // more thing to remember at the exact moment of being annoyed.
    g_full = any_refused;
    if (refused) *refused = any_refused;
    return took;
}

// --- servicing --------------------------------------------------------------

extern "C" void usbmsc_service(void) {
    if (!usb_offered()) return;

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
    if (g_cache_ready && g_cache.dirty) {
        if (!lock_try(&g_usb_lock)) return;  // the shell has it; the next turn will do
        blk_flush();
        lock_release(&g_usb_lock);
    }

}

// Whether the host has written since this was last asked, and has since gone
// quiet. Download mode uses it to notice a finished copy without polling the
// filesystem, and clears it by asking.
bool usbmsc_settled(void) {
    if (!g_import_pending) return false;
    if (task_now_ms() - g_last_write_ms < IMPORT_SETTLE_MS) return false;
    g_import_pending = false;
    return true;
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
    if (g_media_changed) {
        // Reported exactly once, then the next poll succeeds. "Not ready to
        // ready change, medium may have changed" is the sense every operating
        // system answers by discarding what it cached and reading the volume
        // again — which is the only way to tell a host that what it already
        // read is no longer true.
        //
        // TinyUSB only supplies its own sense when the callback left one unset,
        // so this survives rather than being replaced by "medium not present".
        g_media_changed = false;
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
        return false;
    }
    (void)lun;
    return usb_offered();
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_size  = F12_SECTOR;
    *block_count = (usb_offered() && g_formatted) ? storage_usb_bytes() / F12_SECTOR : 0;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    // Writable only while download mode is running. Outside it the volume is
    // present and empty, so there is nothing to read and nowhere to put
    // anything — which is the whole of what "hidden" needs to mean.
    //
    // Read-only too once the filesystem cannot take any more: accepting a write
    // that has nowhere to go is worse than refusing it, because the host
    // believes the file arrived.
    return g_open && !g_full;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    (void)lun;
    if (!usb_offered() || !g_formatted) return -1;
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
    if (!g_open || !g_formatted) return -1;

    // Out of room on the device, so nothing written here could be kept.
    //
    // Refused per command as well as through tud_msc_is_writable_cb, because
    // hosts read that flag when they mount the volume and do not necessarily
    // look again — a device that fills up mid-session would otherwise keep
    // accepting files it cannot hold. The sense code is the one that means
    // exactly this, and Windows renders it as "the disk is write-protected"
    // rather than as an unexplained failure.
    if (g_full) {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27, 0x00);
        return -1;
    }

    // Whole sectors only. The endpoint buffer is a full sector so a partial one
    // does not arise, and refusing beats half-writing the cached block.
    if (offset != 0 || bufsize < F12_SECTOR) return (int32_t)bufsize;

    if (!lock_try(&g_usb_lock)) return ++g_busy < MSC_BUSY_LIMIT ? 0 : (g_busy = 0, -1);
    g_busy = 0;
    bool ok = region_write(nullptr, lba, buffer);
    lock_release(&g_usb_lock);
    // Note the moment, so the importer waits for the copy to finish rather than
    // reading a file the host is still writing.
    g_last_write_ms = task_now_ms();
    g_import_pending = true;
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
