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

// One open file, kept between reads.
//
// A host copying a file asks for its sectors in order, and reopening the file
// for each 512 bytes would walk littlefs metadata thousands of times for one
// copy. Caching the handle turns that into a seek.
static AppSource g_src;
static void     *g_src_handle;
static int32_t   g_src_node = -1;

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
    if (!fat_shortname(name, shortname)) return;  // no 8.3 form: not showable

    MscNode *n = &g_nodes[g_count++];
    memset(n, 0, sizeof(*n));
    memcpy(n->fat.name, shortname, 11);
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
        storage_walk(path, add_entry, &ctx);
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
    fat_layout(g_packed, g_count, g_geom.real_clusters);
    for (uint32_t i = 0; i < g_count; i++) g_nodes[i].fat = g_packed[i];
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

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (!g_ready) {
        // Geometry first: how much the filesystem can actually hold decides how
        // many clusters there are, and everything else is derived from that.
        if (!fat_geom_init(&g_geom, storage_total_bytes())) return false;
        LockGuard _fs(&g_fs_lock);
        if (!msc_build()) return false;
        g_ready = true;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_size  = FAT_SECTOR_SIZE;
    *block_count = g_ready ? g_geom.total_sectors : 0;
}

// Read-only for now: the write path is the next pass. Saying so here is what
// makes a host refuse a copy in its own interface, rather than accept one and
// discover later that nothing was kept.
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return false;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    (void)lun;
    if (!g_ready) return -1;

    // A read of the boot sector is what a fresh mount begins with, so it is the
    // one moment the tree can be rebuilt without contradicting a view the host
    // is already holding. Files the shell created since the last mount appear
    // on a replug, and that is the only point at which they can.
    if (lba == 0 && offset == 0) {
        LockGuard _fs(&g_fs_lock);
        msc_build();
    }

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

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    (void)lun; (void)lba; (void)offset; (void)buffer;
    // Accepted and discarded, deliberately.
    //
    // The volume reports itself read-only, so a host that respects that never
    // reaches here. One that writes anyway gets a success it can act on rather
    // than a stalled endpoint, which is what turns a refused copy into a drive
    // the host marks as failed.
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize) {
    (void)lun; (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        // Nothing to lock: there is no medium to eject. Answering rather than
        // failing keeps the host from treating the device as broken.
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

#endif // CFG_TUD_MSC
