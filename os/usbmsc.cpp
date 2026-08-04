// The drive the host sees, over the filesystem the device actually has.
//
// fatview.cpp knows FAT and nothing else; this knows littlefs and nothing about
// byte layouts. What joins them is a table built by walking the real filesystem
// once: every file and directory gets a contiguous run of clusters, and after
// that a sector read is a lookup rather than a search.
//
// Contiguous runs are the simplification that makes this possible at all.
// Nothing here is an allocator — the layout is recomputed from scratch every
// time the volume is presented, so every file can be laid out end to end and no
// fragmentation ever has to be modelled.
//
// These callbacks run on the `usb` task, in thread context. That is not
// incidental: they take the filesystem lock, which yields, and reaching them
// from an interrupt is what the whole arrangement in usbdev.cpp exists to
// prevent.

#include "fatview.h"
#include "lock.h"
#include "out.h"
#include "registry.h"
#include "storage.h"
#include "task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tusb.h"

#if CFG_TUD_MSC

extern RpcLock g_fs_lock;
extern "C" uint32_t rpc_now_epoch(void);

// How much of the filesystem the drive can show.
//
// A cap rather than a growing structure, because this table is the only thing
// standing between a filesystem with a surprising number of files and a device
// that runs out of memory while a host is mid-enumeration. Files past the cap
// are not shown; they are still there and the shell still sees them.
#define MSC_MAX_NODES 128

// The real littlefs name, kept because the 8.3 form is lossy — it is upper
// cased and truncated, so it cannot be turned back into a path to open.
#define MSC_NAME_MAX  32

#define MSC_MAX_DEPTH 8

struct MscNode {
    FatNode fat;
    char    name[MSC_NAME_MAX];
};

static FatGeom   g_geom;
static MscNode  *g_nodes;
static uint32_t  g_count;
static bool      g_ready;

// The filesystem revision the current view describes.
static uint32_t  g_built_generation;

// The first cluster not backed by an existing file: where free space begins.
//
// This one number is what lets a write be classified without interpreting the
// filesystem. At or past it is a new file; below it is a change to something
// that already exists, which is refused.
static uint32_t  g_first_free = FAT_FIRST_DATA_CLUSTER;

// Where the tail of a dropped file waits until it is known to be complete.
//
// A real file rather than memory: a drop is then bounded by free flash rather
// than by free heap, and the heap on this device could not hold a firmware
// image or a sound file. littlefs is doing what it is good at.
#define MSC_DROP_TEMP "/tmp/.usbdrop"

// Whether the drive is offered at all.
//
// Plugging a device into a machine should not hand that machine every file on
// it. This device is meant to be carried around and plugged into things, and a
// filesystem holding saved networks, account hashes and captured data is not
// something to expose by reflex.
//
// Off means the medium is reported absent: the interface still exists, so the
// console is unaffected and no re-enumeration is needed, but no sector is ever
// served. That is enough to keep the data in — a host cannot read what the
// device will not answer — and it is the same thing an empty card reader looks
// like, which is a state every operating system already handles.
static bool      g_enabled = true;

#define MSC_REG_KEY "USB.Drive"

// One open file, kept between reads.
//
// A host copying a file asks for its sectors in order, and reopening the file
// for each 512 bytes would walk littlefs metadata thousands of times for one
// copy. Caching the handle turns that into a seek.
static AppSource g_src;
static void     *g_src_handle;
static int32_t   g_src_node = -1;

static void drop_commit(void);
static void msc_invalidate(void);

// A file arriving from the host. Declared here because the service loop has to
// know whether one is in flight before it drops the view.
struct Incoming {
    bool     active;
    bool     failed;
    char     name[MSC_NAME_MAX];
    uint32_t first_cluster;    // the first new cluster this file was given
    uint32_t next_cluster;     // the one expected next, to notice gaps
    uint32_t size;             // from the directory entry; 0 until it arrives
    uint32_t written;
    void    *sink;
};

static Incoming g_in;

static void src_close(void) {
    if (g_src_handle) storage_close_source(g_src_handle);
    g_src_handle = nullptr;
    g_src_node = -1;
}

// --- building the table -----------------------------------------------------

struct WalkCtx {
    uint16_t parent;
    bool     full;
};

static void add_entry(void *vctx, const char *name, bool is_dir, uint32_t size) {
    WalkCtx *ctx = (WalkCtx *)vctx;
    if (g_count >= MSC_MAX_NODES) { ctx->full = true; return; }

    // Anything the host created for its own bookkeeping is not shown back to
    // it. Some of it will already be on the filesystem from before this was
    // refused on the way in.
    if (fat_is_host_metadata(name)) return;
    if (strlen(name) >= MSC_NAME_MAX) return;   // no room to keep the real name

    char shortname[11];
    uint8_t case_flags = 0;
    if (!fat_shortname(name, shortname, &case_flags)) return;  // no 8.3 form: not showable

    MscNode *n = &g_nodes[g_count++];
    memset(n, 0, sizeof(*n));
    memcpy(n->fat.name, shortname, 11);
    n->fat.case_flags = case_flags;
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->fat.parent = ctx->parent;
    n->fat.is_dir = is_dir ? 1 : 0;
    n->fat.size   = is_dir ? 0 : size;           // FAT stores 0 for a directory
}

// Rebuild the full path of a node by walking back up to the root.
static bool node_path(uint32_t idx, char *out, uint32_t cap) {
    const char *parts[MSC_MAX_DEPTH];
    int n = 0;
    uint32_t i = idx;
    while (i != 0 && n < MSC_MAX_DEPTH) {      // node 0 is the root itself
        parts[n++] = g_nodes[i].name;
        uint16_t p = g_nodes[i].fat.parent;
        if (p == FAT_NO_PARENT || p >= g_count) break;
        i = p;
    }
    if (n == 0 || n >= MSC_MAX_DEPTH) return false;

    uint32_t o = 0;
    for (int k = n - 1; k >= 0; k--) {
        int w = snprintf(out + o, cap - o, "/%s", parts[k]);
        if (w < 0 || (uint32_t)w >= cap - o) return false;
        o += (uint32_t)w;
    }
    return true;
}

// The node table without the names, which is all fatview needs.
//
// FatNode is the first member of MscNode, so the table is a strided array of
// them and could be cast. It is copied instead: fatview indexes a plain array,
// the stride would be silently wrong the day either structure gains a field,
// and the failure would be a directory listing of garbage rather than a
// compile error. One 4 KB buffer against the alternative.
static FatNode g_packed[MSC_MAX_NODES];

static const FatNode *packed_nodes(void) {
    for (uint32_t i = 0; i < g_count; i++) g_packed[i] = g_nodes[i].fat;
    return g_packed;
}

// Walk the filesystem and lay the whole tree out in clusters.
static bool msc_build(void) {
    if (!g_nodes) {
        g_nodes = (MscNode *)malloc(sizeof(MscNode) * MSC_MAX_NODES);
        if (!g_nodes) return false;
    }
    src_close();
    // A drop that was never finished leaves its temporary behind. Clearing it
    // here keeps it out of the listing and out of the space accounting.
    storage_remove(MSC_DROP_TEMP);

    // Node 0 is the root. It occupies no cluster — FAT16 gives the root
    // directory a fixed region of its own, which is why its entries are built
    // from the root region rather than from a cluster run.
    memset(&g_nodes[0], 0, sizeof(g_nodes[0]));
    g_nodes[0].fat.parent = FAT_NO_PARENT;
    g_nodes[0].fat.is_dir = 1;
    g_count = 1;

    // Breadth-first, with the table itself as the queue: entries appended while
    // scanning one directory are picked up by a later turn of this loop. No
    // recursion, so the depth of the tree costs nothing in stack — which
    // matters because each level of a recursive walk would hold a 264-byte
    // lfs_info, and this runs on a task stack.
    char path[192];
    for (uint32_t i = 0; i < g_count; i++) {
        if (!g_nodes[i].fat.is_dir) continue;
        if (i == 0) {
            snprintf(path, sizeof(path), "/");
        } else if (!node_path(i, path, sizeof(path))) {
            continue;                            // too deep to name: skip it
        }
        WalkCtx ctx{(uint16_t)i, false};
        bool walked = storage_walk(path, add_entry, &ctx);

        // A root that cannot be read is a failure, not an empty drive.
        //
        // These two look identical in the table — no entries either way — and
        // treating them the same is what let a view built before the filesystem
        // was mounted be cached as a perfectly good empty volume. A
        // subdirectory that will not open is different: it is one unreadable
        // folder on an otherwise fine drive, so the walk carries on.
        if (i == 0 && !walked) return false;
    }

    // Timestamps before the layout, because reading one needs the node's path
    // and nothing about the path depends on clusters.
    for (uint32_t i = 1; i < g_count; i++) {
        if (node_path(i, path, sizeof(path))) g_nodes[i].fat.mtime = storage_mtime(path);
    }

    // Clusters last: a directory's size depends on how many children it has,
    // and that is only known once the whole tree is in the table. The layout
    // itself is fatview's, so the empty-file and out-of-room cases are decided
    // in the one place that has tests for them.
    packed_nodes();
    uint32_t used = fat_layout(g_packed, g_count, g_geom.real_clusters);
    for (uint32_t i = 0; i < g_count; i++) g_nodes[i].fat = g_packed[i];

    // Free space begins after everything that exists. This one number is what
    // lets a write be classified without interpreting the filesystem: at or
    // past it is a new file, below it is a change to an existing one.
    g_first_free = FAT_FIRST_DATA_CLUSTER + used;

    // What the filesystem looked like when this was built, so a later change
    // can be noticed rather than served stale.
    g_built_generation = storage_generation();
    return true;
}

// --- the MSC callbacks ------------------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id,  "NovaLabs", 8);
    memcpy(product_id, "RPCortex Storage", 16);
    memcpy(product_rev, "2.0 ", 4);
}

// Build the view if it is not built.
//
// Every callback that needs the volume to exist goes through here rather than
// relying on the host to ask whether the unit is ready first. Nothing in SCSI
// guarantees that order — TinyUSB answers READ_CAPACITY without consulting the
// ready callback at all — and a capacity of zero blocks is a drive the host
// gives up on before it ever asks again.
static bool ensure_ready(void) {
    if (g_ready) return true;
    if (!g_enabled) return false;

    // The filesystem has to be mounted before there is anything to describe,
    // and during boot it is not.
    //
    // A host attached at power-on enumerates while main is still in its wait
    // loop, which is three seconds before kboot mounts anything, and it starts
    // asking for sectors immediately. storage_total_bytes() is a compile-time
    // constant, so the geometry came out fine; the walk found nothing because
    // nothing was mounted; and the result — a valid, correctly sized, entirely
    // empty volume — was then cached as ready for the rest of the run. The
    // drive appeared, mounted, and showed no files, with nothing anywhere
    // reporting an error.
    //
    // Reporting not-ready instead is both honest and what the host wants: the
    // MSC layer answers with "medium not present", which is a condition every
    // operating system retries rather than gives up on.
    bool is_dir = false;
    if (!storage_stat("/", &is_dir, nullptr)) return false;

    // The lock covers the geometry as well as the walk. How much the filesystem
    // can hold is read from it, so it is as much a filesystem question as the
    // listing is, and splitting them leaves a window where the two disagree.
    LockGuard _fs(&g_fs_lock);
    if (g_ready) return true;                       // built while waiting for the lock
    if (!fat_geom_init(&g_geom, storage_total_bytes())) return false;
    if (!msc_build()) return false;
    g_ready = true;
    return true;
}

// The view is rebuilt at mount, and only at mount.
//
// It cannot be rebuilt while the host is using it. A rebuild reassigns every
// cluster from scratch, so a directory listing the host cached moments earlier
// would now name clusters belonging to a different file, and it would read the
// wrong bytes with nothing to indicate anything went wrong. Re-reading the boot
// sector looks like a fresh mount and is not: hosts re-probe volumes mid-copy.
//
// So the triggers are the two that genuinely mean the host has let go — the bus
// going away, and an explicit eject. Files the shell creates in between appear
// on the next mount, which is the honest behaviour and matches what the caching
// makes possible anyway.
static void msc_invalidate(void) { g_ready = false; }

extern "C" void tud_umount_cb(void) { msc_invalidate(); }

// Build the view from the usb task, before it enters the device stack.
//
// This is about lock ordering, not about being early.
//
// The shell takes the filesystem lock and then prints while still holding it —
// ls does exactly that, printing each entry from inside storage_walk — so its
// order is filesystem lock, then the stdio mutex. A callback reached through
// tud_task() is inside the stdio mutex already and takes the filesystem lock
// second, which is the opposite order and therefore a deadlock whenever the two
// overlap. It resolves only when the SDK's stdio mutex times out after a
// second, and it resolves by throwing away the console output that was waiting.
//
// The tree walk is by far the longest time that lock is held, so doing it here
// — on the usb task, outside tud_task, holding no stdio mutex — takes the
// dominant window out of the inversion entirely. What remains inside the device
// stack is a single sector read, which on this filesystem is a memcpy out of
// memory-mapped flash.
//
// The rest of the exposure is real but small, and it is written down in
// USBMSC-DESIGN.md rather than papered over here.
extern "C" void usbmsc_service(void) {
    // Only once a host has actually configured the device. Before that there is
    // no drive to describe and no reason to spend the walk or the memory.
    if (!tud_mounted()) return;

    // Notice when the filesystem has moved on underneath the view.
    //
    // A file created from the shell simply was not on the drive: the view is
    // built once and the host caches what it is told, so neither side had any
    // reason to look again. The filesystem now counts its own changes, so this
    // is an equality test rather than a guess, and it cannot miss one.
    //
    // Dropping the view is the whole mechanism. The next poll reports the
    // medium absent, the one after that reports it present again, and a host
    // treats that exactly as it treats a card being swapped: it throws away
    // everything it had cached and reads the volume afresh. There is no way to
    // tell a host "this sector changed", but there is a well-worn way to tell
    // it "this is a different disk".
    if (g_ready && storage_generation() != g_built_generation) {
        // Not in the middle of serving a file. Pulling the medium out from
        // under a copy in progress would fail it, and the change can wait for
        // the few milliseconds until the copy finishes.
        if (g_src_node < 0 && !g_in.active) msc_invalidate();
    }

    ensure_ready();
}

// --- the toggle -------------------------------------------------------------

bool usbmsc_enabled(void) { return g_enabled; }

void usbmsc_set_enabled(bool on) {
    if (on == g_enabled) return;
    g_enabled = on;
    // Drop the view either way. Turning it off must not leave a built tree
    // behind, and turning it on must not present one built before whatever the
    // filesystem looked like at the time.
    msc_invalidate();
    reg_set(MSC_REG_KEY, on ? "on" : "off");
}

// Read the saved setting at boot. A device that was told to keep its files to
// itself should still be doing that after a reboot.
void usbmsc_init(void) {
    const char *v = reg_get(MSC_REG_KEY, "on");
    g_enabled = !(v && (v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F'));
}

// What the device thinks it is showing.
//
// This exists because the failure that shipped in the first build — a view
// built before the filesystem was mounted, cached as a valid empty volume —
// looked identical from the host to a device with no files on it, and there was
// no way to tell them apart without a rebuild. One command that prints the
// table settles it.
void usbmsc_report(bool verbose) {
    out_multi("  Drive        : %s", g_enabled ? "on" : "off");
    out_multi("  View         : %s", g_ready ? "built" : "not built");
    if (!g_ready) {
        if (!g_enabled)
            out_multi("  %s(turned off -- 'usbdrive on' to offer it)%s", C_GRAY, C_RESET);
        else
            out_multi("  %s(no host has mounted it yet)%s", C_GRAY, C_RESET);
        return;
    }

    out_multi("  Volume       : %u KB in %u sectors",
              (unsigned)(g_geom.total_sectors / 2), (unsigned)g_geom.total_sectors);
    out_multi("  Clusters     : %u real, %u claimed%s",
              (unsigned)g_geom.real_clusters, (unsigned)g_geom.data_clusters,
              g_geom.data_clusters > g_geom.real_clusters
                  ? "  (the difference is marked bad, to clear the FAT16 floor)" : "");
    out_multi("  Showing      : %u entr%s of %u",
              (unsigned)(g_count - 1), g_count == 2 ? "y" : "ies", MSC_MAX_NODES - 1);

    if (g_count <= 1) {
        out_warn("Nothing to show. The filesystem was empty, or unreadable, when "
                 "the view was built.");
        return;
    }
    if (!verbose) return;

    char path[192];
    for (uint32_t i = 1; i < g_count; i++) {
        const FatNode *f = &g_nodes[i].fat;
        if (!node_path(i, path, sizeof(path))) snprintf(path, sizeof(path), "(unnameable)");
        char eight_three[13];
        int o = 0;
        for (int k = 0; k < 8 && f->name[k] != ' '; k++) eight_three[o++] = f->name[k];
        if (f->name[8] != ' ') {
            eight_three[o++] = '.';
            for (int k = 8; k < 11 && f->name[k] != ' '; k++) eight_three[o++] = f->name[k];
        }
        eight_three[o] = 0;
        out_multi("   %-12s %-5s %6u B  cluster %-5u %s", eight_three,
                  f->is_dir ? "DIR" : "FILE", (unsigned)f->size,
                  (unsigned)f->first_cluster, path);
    }
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject && !start) msc_invalidate();     // the host ejected the volume
    return true;
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return ensure_ready();
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_size  = FAT_SECTOR_SIZE;
    *block_count = ensure_ready() ? g_geom.total_sectors : 0;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    // Writable as a volume. Every EXISTING file still carries the read-only
    // attribute, so a host declines to change one in its own interface rather
    // than issuing writes this code has to refuse after the fact.
    return g_enabled && g_ready;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    (void)lun;
    if (!ensure_ready()) return -1;

    uint8_t sec[FAT_SECTOR_SIZE];
    uint32_t idx, sic;
    FatRegion r = fat_region(&g_geom, lba, &idx, &sic);

    switch (r) {
    case FAT_RGN_BOOT:
        fat_build_boot(&g_geom, sec, "RPCORTEX", 0x52504332);   // "RPC2"
        break;

    case FAT_RGN_FAT:
        fat_fat_sector(&g_geom, idx, packed_nodes(), g_count, sec);
        break;

    case FAT_RGN_ROOT:
        fat_dir_sector(packed_nodes(), g_count, 0,
                       idx * (FAT_SECTOR_SIZE / FAT_DIRENT_SIZE), "RPCORTEX", sec);
        break;

    case FAT_RGN_DATA: {
        int32_t owner = fat_node_for_cluster(packed_nodes(), g_count, idx);
        // A cluster belonging to nothing reads as zeroes. The host should not
        // be asking — the table says it is free — but answering with an error
        // turns a stray read into a failed volume.
        if (owner < 0) { memset(sec, 0, sizeof(sec)); break; }

        const FatNode *f = &g_nodes[owner].fat;
        uint32_t within = (idx - f->first_cluster) * g_geom.sectors_per_cluster + sic;

        if (f->is_dir) {
            fat_dir_sector(g_packed, g_count, (uint32_t)owner,
                           within * (FAT_SECTOR_SIZE / FAT_DIRENT_SIZE), "RPCORTEX", sec);
            break;
        }

        LockGuard _fs(&g_fs_lock);
        if (g_src_node != owner) {
            src_close();
            char path[192];
            if (!node_path((uint32_t)owner, path, sizeof(path))) return -1;
            if (!storage_open_source(path, &g_src, &g_src_handle)) return -1;
            g_src_node = owner;
        }
        uint32_t pos = within * FAT_SECTOR_SIZE;
        // Past the end of the file but inside its last cluster: the tail of a
        // cluster is not part of the file, and must read as zeroes rather than
        // as whatever a short read left in the buffer.
        memset(sec, 0, sizeof(sec));
        if (pos < f->size) {
            uint32_t want = f->size - pos;
            if (want > FAT_SECTOR_SIZE) want = FAT_SECTOR_SIZE;
            if (g_src.read(g_src.ctx, pos, sec, want) < 0) return -1;
        }
        break;
    }

    case FAT_RGN_BEYOND:
    default:
        return -1;
    }

    if (offset >= FAT_SECTOR_SIZE) return 0;
    uint32_t n = FAT_SECTOR_SIZE - offset;
    if (n > bufsize) n = bufsize;
    memcpy(buffer, sec + offset, n);
    return (int32_t)n;
}

// --- writes -----------------------------------------------------------------
//
// One case is recognised: a new file appearing. That is not a subset of FAT
// chosen for convenience, it is the only mutation that can be identified from
// raw sector writes without implementing the filesystem — a delete, a rename
// and an edit in place all arrive as writes over regions describing existing
// files, and telling them apart means tracking what every byte of the volume
// meant before the write.
//
// What makes the one case identifiable is WHERE the write lands. Everything
// already on the device was laid out contiguously from cluster 2, so free space
// is one run starting at a cluster this code chose. A write at or past it is
// new data. A write below it is a change to something that exists, and is
// refused. The same for directory entries: slots past the last real one are
// free, slots at or below it are not.
//
// So the boundary is arithmetic rather than interpretation, which is the only
// reason this is safe to do at all.

static void drop_abort(void) {
    if (g_in.sink) storage_close_sink(g_in.sink);
    memset(&g_in, 0, sizeof(g_in));
    storage_remove(MSC_DROP_TEMP);
}

// Finish the file the host has been writing.
//
// Called when the host says it is done — a cache synchronise, or an eject.
// There is no other signal: MSC has no notion of a file, so "the copy has
// finished" has to come from the one command a host issues when it wants its
// writes durable.
static void drop_commit(void) {
    if (!g_in.active) return;
    if (g_in.failed || !g_in.name[0]) { drop_abort(); return; }

    void *sink = g_in.sink;
    g_in.sink = nullptr;
    bool ok = sink ? storage_close_sink(sink) : true;

    // The directory entry carries the real length; the data arrived in whole
    // sectors, so the last one is padded. Without the trim every dropped file
    // would be rounded up to a multiple of 512 and anything that checks a
    // length — a package, an image, a checksum — would reject it.
    uint32_t want = g_in.size;
    if (ok && want < g_in.written) ok = storage_truncate(MSC_DROP_TEMP, want);

    char dest[MSC_NAME_MAX + 2];
    snprintf(dest, sizeof(dest), "/%s", g_in.name);

    if (ok) {
        // Replacing an existing file is a rename over it, which littlefs does
        // atomically. Dropping a file that is already there is the one way to
        // overwrite one, and it is deliberate: the host asked to create it.
        storage_remove(dest);
        ok = storage_rename(MSC_DROP_TEMP, dest);
    }
    if (!ok) storage_remove(MSC_DROP_TEMP);

    memset(&g_in, 0, sizeof(g_in));
    // The view no longer matches the filesystem. Rebuilding immediately would
    // pull the ground out from under a host mid-transfer, so it is invalidated
    // and rebuilt at the next mount — same as any other change.
    msc_invalidate();
}

// Start collecting a file the host has just declared.
static void drop_begin(const char *name, uint32_t first_cluster, uint32_t size) {
    if (g_in.active) drop_commit();      // a second file: the first one is done

    memset(&g_in, 0, sizeof(g_in));
    // Host bookkeeping is refused by name. Windows and macOS both create
    // several of these within seconds of mounting, and none of it was asked
    // for. The write is still accepted so the host sees no error; it simply
    // does not land anywhere.
    if (fat_is_host_metadata(name)) return;

    g_in.active = true;
    g_in.first_cluster = first_cluster;
    g_in.next_cluster = first_cluster;
    g_in.size = size;
    snprintf(g_in.name, sizeof(g_in.name), "%s", name);
    storage_remove(MSC_DROP_TEMP);
    g_in.sink = storage_open_sink(MSC_DROP_TEMP);
    if (!g_in.sink) g_in.failed = true;
}

// The long name, if the host wrote one.
//
// A host creating "readings.json" writes the 8.3 form REPO.JSO — three
// characters of extension is all it has — preceded by long-name entries
// carrying the real thing. Reading those is what keeps a dropped file's name
// intact, and for anything with a four-letter extension it is the difference
// between a working file and a broken one.
static char g_lfn[MSC_NAME_MAX];
static bool g_lfn_valid;

static void lfn_collect(const uint8_t *e) {
    // Sequence number in the low bits; bit 6 marks the LAST entry, which is
    // written first because they are stored in reverse.
    uint32_t seq = e[0] & 0x1F;
    if (seq == 0 || seq > 4) { g_lfn_valid = false; return; }   // more than we keep
    if (e[0] & 0x40) { memset(g_lfn, 0, sizeof(g_lfn)); g_lfn_valid = true; }
    if (!g_lfn_valid) return;

    // Thirteen UTF-16 code units per entry, in three runs.
    static const uint8_t off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    uint32_t base = (seq - 1) * 13;
    for (int i = 0; i < 13; i++) {
        uint32_t idx = base + (uint32_t)i;
        if (idx >= sizeof(g_lfn) - 1) { g_lfn_valid = false; return; }
        uint16_t c = (uint16_t)(e[off[i]] | (e[off[i] + 1] << 8));
        if (c == 0xFFFF) continue;                 // padding
        // Anything outside ASCII has no place in a name this device will later
        // have to type at a shell prompt, so the long name is abandoned and the
        // 8.3 form is used instead.
        if (c > 0x7F) { g_lfn_valid = false; return; }
        g_lfn[idx] = (char)c;
    }
}

// A sector of root directory entries the host has written.
static void root_write(uint32_t sector, const uint8_t *buf) {
    const uint32_t per = FAT_SECTOR_SIZE / FAT_DIRENT_SIZE;
    // Entry 0 is the volume label and 1..n-1 are the files already here, so
    // anything at or below that is a change to something existing.
    uint32_t existing = g_count;      // label + (g_count - 1) children

    for (uint32_t s = 0; s < per; s++) {
        uint32_t idx = sector * per + s;
        const uint8_t *e = buf + s * FAT_DIRENT_SIZE;
        if (idx < existing) continue;             // not free space: refuse silently
        if (e[0] == 0x00 || e[0] == 0xE5) continue;   // free or deleted

        uint8_t attr = e[11];
        if ((attr & 0x0F) == 0x0F) { lfn_collect(e); continue; }
        if (attr & (FAT_ATTR_VOLUME_ID | FAT_ATTR_DIRECTORY)) {
            // A new directory is not something this can accept: its contents
            // would arrive as writes to a cluster with no file behind them.
            g_lfn_valid = false;
            continue;
        }

        char name[MSC_NAME_MAX];
        if (g_lfn_valid && g_lfn[0]) {
            snprintf(name, sizeof(name), "%s", g_lfn);
        } else {
            // Rebuild "NAME.EXT" from the padded on-disk form, honouring the
            // case bits the host set so a dropped file keeps its own case.
            int o = 0;
            bool lower_base = (e[12] & FAT_CASE_BASE_LOWER) != 0;
            bool lower_ext  = (e[12] & FAT_CASE_EXT_LOWER) != 0;
            for (int k = 0; k < 8 && e[k] != ' '; k++) {
                char c = (char)e[k];
                if (lower_base && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                name[o++] = c;
            }
            if (e[8] != ' ') {
                name[o++] = '.';
                for (int k = 8; k < 11 && e[k] != ' '; k++) {
                    char c = (char)e[k];
                    if (lower_ext && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    name[o++] = c;
                }
            }
            name[o] = 0;
        }
        g_lfn_valid = false;

        uint32_t first = (uint32_t)(e[26] | (e[27] << 8));
        uint32_t size  = (uint32_t)e[28] | ((uint32_t)e[29] << 8) |
                         ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);

        // The entry is commonly written twice: once when the file is created,
        // with a length of zero, and again with the real one once the data has
        // gone out. So a second sighting updates rather than restarts.
        if (g_in.active && g_in.first_cluster == first && first != 0) {
            if (size) g_in.size = size;
            continue;
        }
        drop_begin(name, first, size);
    }
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (!g_enabled || !g_ready) return -1;
    // A partial sector cannot be interpreted: both halves of this read whole
    // 32-byte entries and whole clusters. The endpoint buffer is a full sector,
    // so this does not happen; refusing is cheaper than a wrong guess.
    if (offset != 0 || bufsize < FAT_SECTOR_SIZE) return (int32_t)bufsize;

    uint32_t idx, sic;
    switch (fat_region(&g_geom, lba, &idx, &sic)) {
    case FAT_RGN_ROOT:
        root_write(idx, buffer);
        break;

    case FAT_RGN_DATA:
        // Below free space is an existing file. Accepted and dropped rather
        // than failed: the volume already says those files are read-only, and
        // stalling the endpoint would make the host mark the whole drive bad.
        if (idx < g_first_free) break;
        if (!g_in.active || g_in.failed || !g_in.sink) break;
        if (idx != g_in.next_cluster) {
            // Out of order. The sink appends, so the bytes would land at the
            // wrong offset — and a file that is silently wrong is worse than
            // one that did not arrive.
            g_in.failed = true;
            break;
        }
        if (!storage_sink_write(g_in.sink, buffer, FAT_SECTOR_SIZE)) g_in.failed = true;
        g_in.written += FAT_SECTOR_SIZE;
        g_in.next_cluster++;
        break;

    case FAT_RGN_FAT:
    case FAT_RGN_BOOT:
        // The host's own allocation bookkeeping. It is rebuilt from the
        // filesystem every time the volume is presented, so there is nothing
        // here worth keeping.
        break;

    default:
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize) {
    (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        // Nothing to lock: there is no medium to eject. Answering rather than
        // failing keeps the host from treating the device as broken.
        return 0;

    case 0x35:   // SYNCHRONIZE CACHE (10), which TinyUSB does not name
    case 0x91:   // SYNCHRONIZE CACHE (16)
        // The only "the copy has finished" signal MSC has. Hosts issue it at
        // the end of a write and again on eject.
        drop_commit();
        return 0;

    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

#endif // CFG_TUD_MSC
