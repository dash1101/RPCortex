#include "pkg.h"
#include "apps.h"
#include "command.h"
#include "out.h"
#include "kernel.h"
#include "loader.h"
#include "storage.h"
#include "pkgindex.h"
#include "pkgslot.h"
#include "task.h"

// Declared where it is used rather than in apps.h, matching sys.cpp: the pool is
// an implementation detail of apps.cpp and only two callers have any business
// asking for it back.
uint32_t apps_pool_reclaim(void);

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PKG_DIR   "/os/pkg"
#define PKG_INDEX "/os/pkg/index.cfg"
#define IDX_BUF   2048

// Path to a package's installed image: /pkg/<name>.app
static void pkg_path(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s.app", PKG_DIR, name);
}

// --- the index: name,version per line --------------------------------------
// Thin filesystem wrappers over the pure pkgindex_* operations (host-tested).

static void index_add(const char *name, const char *version) {
    char *buf = (char *)malloc(IDX_BUF);
    if (!buf) return;
    uint32_t n = storage_read_file(PKG_INDEX, (uint8_t *)buf, IDX_BUF - 1);
    buf[n] = 0;
    uint32_t nn = pkgindex_add(buf, n, IDX_BUF, name, version);
    // ALWAYS written, never "only if the length changed".
    //
    // That guard was correct while pkgindex_add refused to touch a package it
    // already knew, because then an unchanged length really did mean unchanged
    // content. Now it rewrites the version in place — and 0.95.0 to 0.98.0 is
    // the same number of characters, so the buffer changed and the length did
    // not. The device installed 0.98.0, ran 0.98.0, and went on reporting
    // 0.95.0 in `pkg list`, which is the exact bug the rewrite was for.
    //
    // An install has just written a package to flash. One more small write is
    // not worth a cleverer comparison than "did we mean to record something".
    (void)n;
    storage_write_file(PKG_INDEX, (uint8_t *)buf, nn);
    free(buf);
}

static void index_remove(const char *name) {
    char *buf = (char *)malloc(IDX_BUF);
    if (!buf) return;
    uint32_t n = storage_read_file(PKG_INDEX, (uint8_t *)buf, IDX_BUF - 1);
    buf[n] = 0;
    uint32_t nn = pkgindex_remove(buf, n, IDX_BUF, name);
    storage_write_file(PKG_INDEX, (uint8_t *)buf, nn);
    free(buf);
}

static void index_walk(PkgIndexFn cb, void *ctx) {
    char *buf = (char *)malloc(IDX_BUF);
    if (!buf) return;
    uint32_t n = storage_read_file(PKG_INDEX, (uint8_t *)buf, IDX_BUF - 1);
    buf[n] = 0;
    pkgindex_walk(buf, n, cb, ctx);
    free(buf);
}

// --- the flash slots a package can RUN from ----------------------------------
//
// Two ways a package can be installed now, and this half is the new one. A
// position-independent package built for a sandboxed board keeps its read-only
// half — .text, .rodata and its firmware veneers — in a flash slot and executes
// it in place, so only the writable half is resident: 62 KB for a Nova D1
// instead of 184. Everything else keeps the copy-to-RAM path it always had.
//
// The decision itself is pkg_route (pkgslot.cpp), which is pure and host-tested.
// What is here is the filesystem and chip either side of it.
//
// DEVICE-UNCONFIRMED, all of it, and the list is the list because no host can
// shorten it:
//   * the erase and the program themselves (pkg_erase / pkg_program in
//     storage.cpp). A fake chip proves the ORDER, never the chip.
//   * executing from the slot at all — the CPU fetching a package's
//     instructions out of XIP flash instead of out of SRAM.
//   * unprivileged execute-from-flash. app_enter hands `image` to the memory
//     protection unit as the package's read-only executable region, and with a
//     slot that base is a flash address for the first time. mpu_v8_encode has
//     never been given one. A refusal here is a MemManage on the package's very
//     first instruction, which is why it is worth knowing to look for.
//   * the watchdog margin. A slot install is roughly forty sector erases and
//     six hundred page programs; slot_sink feeds the watchdog on every page,
//     but sixteen seconds against several is a measurement nobody has taken.

#define PKG_SLOTS_MAX 4     // more than any board carries; the table is static

// A slot, read once.
//
// pkgslot_open CRCs the whole body — 150 KB for a Nova D1 — so asking "which
// slot holds this package" once per installed package during the boot walk would
// pay for that per package. The answer only changes when this file writes a
// slot, and it forgets it there.
struct SlotView {
    bool          probed;
    bool          warned;
    PkgSlotStatus status;
    const void   *base;      // where the slot is mapped, for pkgslot_blob
    PicManifest   m;         // borrowed: its arrays point INTO the mapping
};
static SlotView g_slots[PKG_SLOTS_MAX];

static SlotView *slot_view(uint32_t i) {
    if (i >= storage_pkg_slot_count() || i >= PKG_SLOTS_MAX) return nullptr;
    SlotView *v = &g_slots[i];
    if (v->probed) return v;
    v->probed = true;
    v->base   = storage_pkg_slot_map(i);
    v->status = v->base ? pkgslot_open(v->base, storage_pkg_slot_bytes(), &v->m)
                        : PKGSLOT_EMPTY;
    // SAID OUT LOUD, once per boot. A slot that holds something unreadable is
    // the interesting case: after a firmware update whose ABI moved, every
    // slot-resident package silently reverts to loading from the filesystem and
    // costs three times the RAM it did yesterday. Nothing else would ever
    // mention it.
    if (v->status != PKGSLOT_OK && v->status != PKGSLOT_EMPTY && !v->warned) {
        v->warned = true;
        out_warnp("pkg", "The package slot could not be used: %s.",
                  pkgslot_status_str(v->status));
        out_multi("  Whatever was in it loads from the filesystem instead, into");
        out_multi("  RAM. Installing that package again puts it back in the slot.");
    }
    return v;
}

// Forget what a slot held. Called either side of a write, because between those
// two points the cached answer describes something that is being erased.
static void slot_forget(uint32_t i) {
    if (i < PKG_SLOTS_MAX) { g_slots[i].probed = false; g_slots[i].warned = false; }
}

// Which slot holds `name` right now, or -1.
static int slot_holding(const char *name) {
    for (uint32_t i = 0; i < storage_pkg_slot_count() && i < PKG_SLOTS_MAX; i++) {
        SlotView *v = slot_view(i);
        if (v && v->status == PKGSLOT_OK && !strcmp(v->m.header.name, name))
            return (int)i;
    }
    return -1;
}

// Which slot this install may WRITE, or -1.
//
// A slot is available if it is empty, if it holds something this firmware cannot
// read anyway, or if it already holds this same package — that last one being an
// upgrade in place. A slot holding a DIFFERENT working package is not available,
// and that is deliberate: with one slot per board, taking it would silently
// evict whatever was in it and triple that package's RAM cost. The new package
// takes the copy path instead, which is what it would have done on a board with
// no slots at all.
static int slot_available(const char *name, const char **occupant) {
    if (occupant) *occupant = nullptr;
    for (uint32_t i = 0; i < storage_pkg_slot_count() && i < PKG_SLOTS_MAX; i++) {
        SlotView *v = slot_view(i);
        if (!v) continue;
        if (v->status != PKGSLOT_OK) return (int)i;              // empty or unreadable
        if (!strcmp(v->m.header.name, name)) return (int)i;      // ours already
        if (occupant) *occupant = v->m.header.name;
    }
    return -1;
}

// Erase a slot's header, so it reads as empty. The package that was running from
// it must be gone first — its code IS that flash.
static bool slot_release(uint32_t i) {
    SlotFlash fl; storage_pkg_slot_flash(&fl);
    slot_forget(i);
    bool ok = pkgslot_erase(&fl, storage_pkg_slot_offset(i));
    slot_forget(i);
    return ok;
}

// The sink app_pic_install writes through, with the two things a device needs
// that the format does not: a progress bar, and something that keeps the
// watchdog from concluding the machine has stopped.
//
// A slot install is around forty sector erases and six hundred page programs
// with nothing yielding in between — several seconds against a sixteen second
// watchdog. Staging a firmware image learned this the hard way and the note is
// still on stage_progress in update.cpp: without a hook in the loop the device
// resets in the middle of the copy. Here that would look like an install that
// silently did not take, because the boot fallback would quietly load the old
// copy from the filesystem afterwards.
struct SlotSink {
    PkgSlotWriter *w;
    uint32_t       total;
    bool           quiet;
};

static bool slot_sink(void *ctx, uint32_t off, const void *data, uint32_t len) {
    SlotSink *s = (SlotSink *)ctx;
    task_alive();
    task_watchdog_feed();     // task_alive is a no-op before the scheduler is up
    if (!s->quiet) out_progress("Installing", off, s->total);
    return pkgslot_sink(s->w, off, data, len);
}

// Write `src` into slot `i`. Returns LOAD_OK, or what went wrong.
//
// THE ERASE IS INSIDE HERE, and it is the point of no return: pkgslot_begin
// destroys the slot's magic before a byte of the new package is written, on
// purpose, so an interrupted install can never leave a half-written package that
// still looks loadable. Everything that could have refused this has already
// refused it — see pkg_install_file, which does its checking before calling.
// `*erased` says which side of that line a failure happened on, because the two
// leave the device in different states and telling somebody the wrong one is
// worse than telling them nothing.
static LoadResult slot_write(const AppSource &src, uint32_t i,
                             const PicMeasure &pm, bool quiet, bool *erased) {
    *erased = false;
    SlotFlash fl; storage_pkg_slot_flash(&fl);
    // Static, not on the stack: the writer carries a 256-byte page buffer and
    // this runs on the shell's 8 KB stack. loader.cpp makes the same choice for
    // the same reason. It also means one install at a time, which is true of the
    // loader's own buffers already.
    static PkgSlotWriter w;
    static PicManifest   m;

    slot_forget(i);
    if (!pkgslot_begin(&w, &fl, storage_pkg_slot_offset(i), storage_pkg_slot_bytes()))
        return LOAD_ERR_READ;
    *erased = true;

    SlotSink sink{&w, pm.body_bound, quiet};
    LoadResult rc = app_pic_install(src, slot_sink, &sink, &m);
    if (rc == LOAD_OK) {
        task_alive();
        task_watchdog_feed();
        if (!pkgslot_commit(&w, &m)) rc = LOAD_ERR_READ;
    }
    app_pic_manifest_free(&m);          // the slot is the copy that matters now
    if (!quiet) out_progress_done();
    slot_forget(i);
    return rc;
}

// --- operations ------------------------------------------------------------

// Put the package file where it belongs, record it, and load it. Shared by both
// paths: they differ in what they did before this and in where the load comes
// from, and in nothing else. `slot` >= 0 means load it out of that slot.
static bool install_finish(const char *file, const char *name, const char *version,
                           bool quiet, bool consume, int slot) {
    char dst[40]; pkg_path(name, dst, sizeof(dst));
    if (strcmp(file, dst) != 0) {
        // Rename when the caller is handing the file over. See pkg.h: a copy
        // wants the package's size free a second time, and the download that
        // produced this file reserved eight kilobytes.
        bool moved = false;
        if (consume) {
            storage_remove(dst);          // rename onto an existing name is not portable
            moved = storage_rename(file, dst);
        }
        if (!moved && !storage_copy(file, dst)) {
            out_err("Could not put the package into %s.", PKG_DIR);
            out_multi("  %lu bytes free. A package needs its own size again to be",
                      (unsigned long)storage_free_bytes());
            out_multi("  copied in; 'pkg remove' something, or 'df' to see where it went.");
            if (slot >= 0) {
                // The slot already has the new package, so the device WILL run
                // it — but the file a reinstall or a boot fallback would read is
                // still the old one. Two versions, and only one of them said so.
                out_multi("  ");
                out_multi("  The flash slot already holds the new %s, so it is what", name);
                out_multi("  runs. The file on the filesystem is still the old one.");
            }
            return false;
        }
    }
    index_add(name, version);
    if (!quiet) out_okp("pkg", "Installed %s %s", name, version);
    // Load it now so its commands are available without a reboot — and NOT
    // quietly. A package whose commands could not register is the difference
    // between "installed" and "usable", and staying silent about it produced
    // exactly the report this comment exists because of: pkg said Installed,
    // the command did not exist, and nothing anywhere said why.
    int rc;
    if (slot >= 0) {
        SlotView *v = slot_view((uint32_t)slot);
        rc = (v && v->status == PKGSLOT_OK)
                 ? apps_launch_pic(pkgslot_blob(v->base), &v->m, 0, /*quiet*/false)
                 : -1;
    } else {
        rc = apps_launch(dst, 0, /*quiet*/false);
    }
    if (rc < 0) {
        out_warnp("pkg", "'%s' is installed but did not load.", name);
        out_multi("  It will be tried again at the next boot.");
    }
    return true;
}

bool pkg_install_file(const char *file, bool quiet, bool consume) {
    // THE NAME COMES FROM THE HEADER, NOT FROM A FULL LOAD, and the order below
    // is the whole point of this function.
    //
    // It used to validate first by loading the image, and only then unload the
    // copy that was already running. Upgrading a package therefore needed TWO
    // copies of its image resident at the same moment. A 53 KB image on a device
    // with 96 KB free and 42 KB in its largest block cannot do that, so a device
    // could not install its own update — and the message was "out of memory"
    // from a step nobody expected to allocate.
    //
    // app_peek reads the section table and the header section and allocates a
    // few hundred bytes for section names. That is enough to know what this is
    // and whether it could ever load, which is enough to decide whether to make
    // room for it.
    AppSource src; void *h = nullptr;
    if (!storage_open_source(file, &src, &h)) { out_err("No such file: %s", file); return false; }

    RpcAppHeader hdr;
    LoadResult rc = app_peek(src, &hdr);
    if (rc != LOAD_OK) {
        storage_close_source(h);
        out_err("Not a valid package: %s", load_result_str(rc));
        return false;
    }
    char name[24], version[12];
    strncpy(name, hdr.name, sizeof(name) - 1); name[sizeof(name)-1] = 0;
    strncpy(version, hdr.version, sizeof(version) - 1); version[sizeof(version)-1] = 0;

    // Nothing may be executing it. The file on flash is only half of a resident
    // package, and replacing it under a task parked inside the copy in RAM
    // leaves the two permanently disagreeing.
    int busy = apps_busy_pid(name);
    if (busy >= 0) {
        storage_close_source(h);
        out_errp("pkg", "'%s' is running right now (task %d).", name, busy);
        out_multi("  Let it finish, or stop it with 'kill %d', then try again.", busy);
        return false;
    }

    // --- WHICH PATH, and it is decided here, before anything has been touched.
    //
    // The two are genuinely different: the copy path can put back what it
    // unloaded, because the file it loaded from was never altered. The slot path
    // cannot — pkgslot_begin erases the header first by design, so the resident
    // package is gone before the new one has been proved. That asymmetry is the
    // whole reason the checking happens up here: architecture, ABI, whether the
    // package is position-independent at all, and whether it fits the slot are
    // all answered from the section headers and one relocation scan, and every
    // one of those refusals costs nothing. The download's sha256 already proved
    // the bytes; what is left after this is the flash itself.
    PicMeasure  pm{};
    PkgRouteIn  ri{};
    int         slot = -1;
    const char *occupant = nullptr;
    if (storage_pkg_slot_count()) {
        LoadResult mrc = app_pic_measure(src, &pm);
        if (mrc == LOAD_OK) {
            slot          = slot_available(name, &occupant);
            ri.pic        = pm.pic;
            ri.svc        = loader_veneer_mode() == LOADER_VENEER_SVC;
            ri.slot_free  = slot >= 0;
            ri.slot_bytes = storage_pkg_slot_bytes();
            ri.need_bytes = pm.body_bound;
        }
        // A measure that fails is not an error here. app_load is about to read
        // the same file on the copy path and will refuse it with a message that
        // names the actual problem, which is a better one than this could give.
    }
    const PkgRoute route = pkg_route(ri);

    // Both reasons a package that WANTED the slot did not get it, said before
    // the copy path runs rather than after it fails. Either one ends in a
    // 184 KB allocation on a device whose largest free block is 89, and "out of
    // memory" on its own sends people looking in entirely the wrong place.
    if (ri.pic && ri.svc && ri.slot_bytes) {
        if (route == PKG_ROUTE_TOO_BIG) {
            out_warnp("pkg", "'%s' is too big for the flash slot: %lu bytes "
                             "against %lu.", name,
                      (unsigned long)pm.body_bound,
                      (unsigned long)pkgslot_body_capacity(ri.slot_bytes));
            out_multi("  Loading it into RAM instead, which needs %lu bytes in one",
                      (unsigned long)(pm.ro_bound + pm.ram_size));
            out_multi("  piece rather than %lu.", (unsigned long)pm.ram_size);
        } else if (route == PKG_ROUTE_COPY && occupant) {
            out_warnp("pkg", "The flash slot holds '%s', so '%s' loads into RAM.",
                      occupant, name);
            out_multi("  There is one slot. 'pkg remove %s' frees it if that is", occupant);
            out_multi("  the package you would rather have out of RAM.");
        }
    }

    if (route == PKG_ROUTE_SLOT) {
        // NOTHING MAY BE EXECUTING WHAT IS ABOUT TO BE ERASED.
        //
        // apps_unload already makes the only test worth making — a task parked
        // inside the package's image — and the reason its answer is READ here
        // rather than dropped is that this path erases the code that task would
        // return into. On the copy path a refused unload costs an upgrade; on
        // this one it would be flash going away underneath a running task.
        if (apps_resident(name) && !apps_unload(name)) {
            out_errp("pkg", "'%s' is running right now.", name);
            out_multi("  Its code is IN the slot this would erase. Let it finish,");
            out_multi("  or 'kill' the task, then try again.");
            storage_close_source(h);
            return false;
        }
        // The pool, for the same reason the copy path asks for it: an install is
        // the largest thing this device does, even at a tenth of the old cost.
        apps_pool_reclaim();

        bool erased = false;
        LoadResult src_rc = slot_write(src, (uint32_t)slot, pm, quiet, &erased);
        storage_close_source(h);
        if (src_rc != LOAD_OK) {
            out_errp("pkg", "'%s' could not be written to the flash slot: %s",
                     name, load_result_str(src_rc));
            // NO SILENT STATE, AND NO WRONG ONE EITHER. Failing before the erase
            // and failing after it leave the device in different places, and the
            // difference is the whole of what somebody needs to know.
            if (!erased) {
                out_multi("  Nothing was erased — the slot still holds whatever it");
                out_multi("  held, and the package is unchanged. A flash write that");
                out_multi("  is refused outright is usually the other core: see");
                out_multi("  flash_safe_execute in the log.");
            } else {
                out_multi("  The slot is erased before the new package is written, so");
                out_multi("  a half-written one can never be run — it is empty now.");
                out_multi("  Nothing on the filesystem changed: whatever was installed");
                out_multi("  before is still there, and a reboot loads it into RAM.");
                out_multi("  'pkg install %s' tries the slot again.", name);
            }
            return false;
        }
        return install_finish(file, name, version, quiet, consume, slot);
    }

    // Room first, THEN validate. The old copy goes now; its file is untouched,
    // so if what follows fails there is something to put back.
    bool was_resident = apps_resident(name);
    apps_unload(name);

    // AND ASK FOR THE SANDBOX POOL BACK, which is where the memory actually is.
    //
    // A task that finishes normally keeps its slot so its exit status can be
    // read, and with the slot it keeps its sandbox entry — about 23 KB of stack
    // and arena. Nothing releases that automatically: the only reclaimer runs
    // when all twelve TASK SLOTS are full, and the pool it protects is exhausted
    // at FOUR. So four background runs leave the pool empty with eight slots
    // still free, the reclaimer stays dormant because by its own measure nothing
    // is scarce, and 69 KB sits held by tasks that finished minutes ago.
    //
    // apps_pool_reclaim already frees exactly that state — claimed, not lent —
    // and was wired to `freeup` and to the allocator's own retry, but not to
    // here. Loading an image is the largest single allocation this device ever
    // makes, so it is precisely where the memory should be asked for.
    //
    // This does not remove the need for a reaper. It means an install stops
    // failing for want of memory that is sitting there unowned.
    apps_pool_reclaim();

    LoadedApp probe;
    rc = app_load(src, &probe);
    storage_close_source(h);
    if (rc != LOAD_OK) {
        out_err("Not a valid package: %s%s%s", load_result_str(rc),
            probe.detail[0] ? " - " : "", probe.detail);
        if (rc == LOAD_ERR_OOM) {
            out_multi("  %lu bytes free. The image needs its size in ONE piece,",
                      (unsigned long)heap_free());
            out_multi("  so the total is not the number that decides it —");
            out_multi("  'meminfo' shows the largest block, which is.");
            out_multi("  ");
            // The specific shape this takes, said out loud, because working it
            // out from "largest block" takes longer than it should.
            //
            // Upgrading a package that GREW is the hard case: the copy being
            // replaced is unloaded first, but the hole it leaves is its own old
            // size, and the new image does not fit in it. Everything else on
            // the heap is still where it was, so the total climbs and the
            // largest block does not.
            //
            // Starting from a boot with the package not loaded at all gives one
            // large region instead of two medium ones, and that is the whole
            // difference between this failing and succeeding.
            out_multi("  An upgrade to a LARGER package is the hard case: the old");
            out_multi("  copy's space is its own old size, and the new one does");
            out_multi("  not fit in it. What works:");
            out_multi("    service clear      so nothing loads it at boot");
            out_multi("    reboot");
            out_multi("    pkg install %s", name);
            out_multi("    novad1 setup       to put the service back");
        }
        // Put back what was working. The file was never touched, so this is a
        // genuine restore rather than a hope — and a failed upgrade leaving the
        // device without the package it had is a far worse outcome than the
        // upgrade not happening.
        if (was_resident) {
            char back[40]; pkg_path(name, back, sizeof(back));
            if (apps_launch(back, 0, /*quiet*/true) >= 0) {
                out_multi("  '%s' is still installed and has been reloaded.", name);
            } else {
                // SAID OUT LOUD. The reload fails for the same reason the
                // install did — there is no room — and this branch used to be
                // silent, which left a device whose commands had just vanished
                // with nothing on screen explaining it or saying they come
                // back. The file was never touched, so this is a message
                // rather than a recovery.
                out_multi("  '%s' could not be reloaded — the same memory it "
                          "needed to install.", name);
                out_multi("  The installed copy on flash is untouched. A reboot "
                          "brings its commands back.");
            }
        }
        return false;
    }
    app_unload(&probe);      // validated; the live copy is loaded below

    return install_finish(file, name, version, quiet, consume, /*slot*/-1);
}

static bool pkg_remove(const char *name) {
    char path[40]; pkg_path(name, path, sizeof(path));
    bool known = storage_stat(path, nullptr, nullptr);
    int busy = apps_busy_pid(name);
    if (busy >= 0) {
        out_errp("pkg", "'%s' is running right now (task %d).", name, busy);
        out_multi("  Removing it would free code that task is about to return");
        out_multi("  into. Let it finish, or stop it with 'kill %d'.", busy);
        return false;
    }
    // Stop it and free it if resident. The answer is read, not dropped: a false
    // here means a task is inside the package's code, and the slot erase below
    // would take that code away from underneath it. apps_busy_pid answered above
    // so this should not happen — but "should not" and "cannot" are different,
    // and the difference is a slot erase.
    if (apps_resident(name) && !apps_unload(name)) {
        out_errp("pkg", "'%s' started running while it was being removed.", name);
        return false;
    }
    // The slot too, if it holds this package. Leaving it would keep flash
    // describing something that is no longer installed, and — because there is
    // one slot — would stop the next package from ever using it.
    int slot = slot_holding(name);
    if (slot >= 0 && !slot_release((uint32_t)slot))
        out_warnp("pkg", "'%s' was removed, but its flash slot could not be "
                         "cleared.", name);
    storage_remove(path);
    index_remove(name);
    if (!known) { out_err("Not installed: %s", name); return false; }
    out_okp("pkg", "Removed %s", name);
    return true;
}

static void list_cb(void *, const char *name, const char *version) {
    out_multi("  %s%-16s%s %-8s", C_CYAN, name, C_RESET, version);
}
static void pkg_list(void) {
    out_info("Installed packages:");
    index_walk(list_cb, nullptr);
}

// --- the machine-readable form ----------------------------------------------
//
// For a browser, not for a person. The package page on the site connects over
// Web Serial, and parsing the coloured, aligned list above would mean parsing
// escape sequences and column widths — both of which are free to change.
//
// The shape is v1's, deliberately, so ONE page reads both operating systems:
//
//     PKGS_BEGIN
//     OS:v2.0.0:pico2_w
//     PKG:httpd:2.1
//     PKGS_END
//
// The OS line is the only addition. v1 does not emit one, so its absence is how
// a page knows it is talking to v1 and should offer v1's catalogue — no version
// selector to get wrong, and no guessing from a banner that is free to change.
//
// out_multi, which is the plain line printer: no prefix, no colour, no
// indentation. Anything decorative here is something a parser has to be taught
// to ignore. It also travels the data channel, so `_pkgs > file` works.
static void manifest_cb(void *, const char *name, const char *version) {
    out_multi("PKG:%s:%s", name, version);
}

static int cmd_pkgs_manifest(int, char **) {
    out_multi("PKGS_BEGIN");
    out_multi("OS:%s:%s", RPC_OS_VERSION, PICO_BOARD);
    index_walk(manifest_cb, nullptr);
    out_multi("PKGS_END");
    return 0;
}

// --- boot loading + command ------------------------------------------------

bool pkg_is_disabled(const char *name);    // diag.cpp

static void load_cb(void *, const char *name, const char *) {
    // A package that crashes at boot can be switched off without removing it,
    // so the device comes up and the package is still there to look at. The
    // check is here rather than in the index so the record of what is installed
    // stays separate from the decision to load it.
    if (pkg_is_disabled(name)) {
        out_warnp("pkg", "'%s' is disabled and was not loaded.", name);
        return;
    }
    // ALREADY LOADED IS NOT A REASON TO LOAD IT AGAIN. pkg_install_file loads
    // what it installs, so anything the stock updater placed a moment ago is
    // resident before this walk reaches it. Loading a second copy cannot
    // register the commands the first already owns, and the failure reads as
    // the package being broken rather than as this having asked twice.
    if (apps_resident(name)) return;

    char path[40]; pkg_path(name, path, sizeof(path));

    // THE SLOT FIRST, AND THE FILE AS A FALLBACK, which is what makes a torn or
    // failed install heal itself rather than leaving a device without its
    // package.
    //
    // A slot that is empty, from another firmware's format, or fails its CRC is
    // simply not offered here — slot_holding asks pkgslot_open, which answers no
    // to all three. The .app is still on the filesystem in every one of those
    // cases, so the ordinary copy-to-RAM load takes over, and boot is exactly
    // when it can: the heap has one large contiguous block at this point, which
    // is the difference between a 184 KB image fitting and not. Mid-session the
    // same load would fail.
    int slot = slot_holding(name);
    SlotView *v = slot >= 0 ? slot_view((uint32_t)slot) : nullptr;
    if (v && v->status == PKGSLOT_OK) {
        if (apps_launch_pic(pkgslot_blob(v->base), &v->m, 0, /*quiet*/true) >= 0)
            return;
        // It was there and it would not load. Not silent: the package is about to
        // cost three times the RAM it was supposed to, and this is the only line
        // that would ever say why.
        out_warnp("pkg", "'%s' is in the flash slot but did not load from it.", name);
        out_multi("  Loading the copy on the filesystem into RAM instead.");
    }
    apps_launch(path, 0, /*quiet*/true);
}

void pkg_load_installed(void) { index_walk(load_cb, nullptr); }

struct VerLookup { const char *want; char *out; unsigned cap; bool hit; };

static void ver_cb(void *ctx, const char *name, const char *version) {
    VerLookup *v = (VerLookup *)ctx;
    if (v->hit) return;
    // Case-insensitive: the index says "RPCMark" and people type "rpcmark".
    const char *a = name, *b = v->want;
    while (*a && *b) {
        char la = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char lb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (la != lb) return;
        a++; b++;
    }
    if (*a || *b) return;
    snprintf(v->out, v->cap, "%s", version);
    v->hit = true;
}

bool pkg_installed_version(const char *name, char *out, unsigned cap) {
    char *buf = (char *)malloc(IDX_BUF);
    if (!buf) return false;
    uint32_t n = storage_read_file(PKG_INDEX, (uint8_t *)buf, IDX_BUF - 1);
    buf[n] = 0;
    VerLookup v{name, out, cap, false};
    pkgindex_walk(buf, n, ver_cb, &v);
    free(buf);
    return v.hit;
}

void pkg_init(void) { storage_mkdir(PKG_DIR); }   // no-op if it already exists

int pkg_repo_command(int argc, char **argv);   // pkgrepo.cpp

static int cmd_pkg(int argc, char **argv) {
    if (argc >= 2) {
        // The repo half first: it owns install (by name), update, search, info
        // and upgrade, and returns -1 for anything it does not handle.
        int rc = pkg_repo_command(argc, argv);
        if (rc >= 0) return rc;
    }
    if (argc >= 3 && !strcmp(argv[1], "remove")) return pkg_remove(argv[2]) ? 0 : 1;
    if (argc >= 2 && !strcmp(argv[1], "list"))   { pkg_list(); return 0; }
    out_multi("Usage:");
    out_multi("  pkg update              refresh the package list");
    out_multi("  pkg search [text]       list or search available packages");
    out_multi("  pkg info <name>         details, including whether it verifies");
    out_multi("  pkg install <name>      install from the repo");
    out_multi("  pkg install <path>      install a file already on the device");
    out_multi("  pkg upgrade             update everything with a newer version");
    out_multi("  pkg remove <name>       uninstall");
    out_multi("  pkg list                what is installed");
    out_multi("  pkg repo [add|remove]   which repositories to use");
    // `stock` is its own command, not a pkg subcommand — but `pkg stock` is
    // what anyone looking for it types first, so say where it went.
    out_multi("  stock                   restore a package built into the firmware");
    out_multi("  pkg certs [install]     trusted roots for HTTPS");
    return 1;
}

void pkg_register(void) {
    static const Command c{"pkg", "install/remove/list packages", cmd_pkg, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
    // Underscored, and with no help text, because it is not for a person. It
    // reads nothing and changes nothing, so it needs no privilege.
    static const Command m{"_pkgs", "", cmd_pkgs_manifest, nullptr};
    cmd_register(&m);
}
