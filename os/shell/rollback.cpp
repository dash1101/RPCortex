// Firmware rollback: keeping a copy of what worked, and going back to it.
//
// The saved image is a plain file, /os/rollback.img, written straight out of
// the running firmware's own flash. Reading XIP while the device runs is
// ordinary — it is the same memory every instruction is fetched from — so the
// copy costs nothing but the space and a few seconds.
//
// /os/rollback.cfg is written LAST and is the only thing that makes the pair
// valid. A capture interrupted halfway leaves an image with no cfg, which reads
// as "nothing saved" rather than as a plausible-looking image that would brick
// the board. Nothing here trusts the .img on its own.
//
// What this cannot do: rescue an image that does not boot at all. The restore
// runs inside the firmware being replaced, so the firmware has to get as far as
// kboot for the automatic path to fire. A truly dead image still needs BOOTSEL,
// and no amount of care here changes that — only a bootloader that never
// changes could, and this device does not have one.

#include "rollback.h"
#include "command.h"
#include "out.h"
#include "storage.h"
#include "sha256.h"
#include "registry.h"
#include "persist.h"
#include "kernel.h"
#include "task.h"
#include "users.h"
#include "session.h"
#include "logring.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"

bool update_apply_file(const char *path, const char *to_version, bool at_boot);

#define CHUNK 4096

// --- reading what is saved ---------------------------------------------------

static void parse_line(RollbackInfo *o, char *line) {
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = 0;
    const char *k = line, *v = eq + 1;
    if      (!strcmp(k, "ver"))     snprintf(o->ver, sizeof(o->ver), "%s", v);
    else if (!strcmp(k, "board"))   snprintf(o->board, sizeof(o->board), "%s", v);
    else if (!strcmp(k, "sha"))     snprintf(o->sha, sizeof(o->sha), "%s", v);
    else if (!strcmp(k, "size"))    o->size = (uint32_t)strtoul(v, nullptr, 10);
    else if (!strcmp(k, "reserve")) o->reserve = (uint32_t)strtoul(v, nullptr, 10);
}

bool rollback_read(RollbackInfo *out) {
    RollbackInfo info{};

    bool is_dir = false; uint32_t cfg_size = 0;
    if (!storage_stat(ROLLBACK_CFG, &is_dir, &cfg_size) || is_dir || cfg_size == 0)
        return false;
    if (cfg_size > 512) return false;

    AppSource src{}; void *h = nullptr;
    if (!storage_open_source(ROLLBACK_CFG, &src, &h)) return false;
    char buf[513];
    bool ok = src.read(src.ctx, 0, (uint8_t *)buf, cfg_size) >= 0;
    storage_close_source(h);
    if (!ok) return false;
    buf[cfg_size] = 0;

    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        parse_line(&info, p);
        if (!nl) break;
        p = nl + 1;
    }

    // Every one of these has a way of being wrong that would cost the device,
    // so none of them is assumed.
    if (!info.ver[0] || !info.sha[0] || !info.size) return false;

    // Another board's firmware is not a rollback, it is a brick. The pins, the
    // radio and the flash geometry all differ, and the image would be written
    // without complaint.
    if (strcmp(info.board, PICO_BOARD) != 0) return false;

    // A build that reserved a different amount of flash for itself puts the
    // filesystem somewhere else. Restoring it would leave the device looking at
    // the middle of its own files for a littlefs superblock.
    if (info.reserve != storage_reserve_bytes()) return false;

    if (info.size > storage_fw_slot_bytes()) return false;

    uint32_t img_size = 0;
    if (!storage_stat(ROLLBACK_IMG, &is_dir, &img_size) || is_dir) return false;
    if (img_size != info.size) return false;      // truncated, or a stale pair

    if (out) *out = info;
    return true;
}

void rollback_forget(void) {
    storage_remove(ROLLBACK_CFG);
    storage_remove(ROLLBACK_IMG);
}

// --- saving the running image ------------------------------------------------

bool rollback_capture(bool announce) {
    uint32_t size = storage_firmware_bytes();
    if (!size || size > storage_fw_slot_bytes()) return false;

    // Room FIRST, and measured against what the existing copy would give back.
    //
    // Deleting before checking was the obvious order and it is wrong in the one
    // case that matters: on a device with no room, it destroys a perfectly good
    // saved image to discover that the replacement will not fit. During an
    // update that means losing the copy of the version being replaced at the
    // exact moment it becomes the one worth having.
    bool was_dir = false; uint32_t existing = 0;
    storage_stat(ROLLBACK_IMG, &was_dir, &existing);
    uint32_t need = size > existing ? size - existing : 0;

    if (need && !storage_would_fit(need)) {
        if (announce) {
            out_warn("No room to keep a rollback copy (%lu KB needed, %lu KB free).",
                     (unsigned long)(size / 1024),
                     (unsigned long)((storage_free_bytes() + existing) / 1024));
            if (existing)
                out_multi("  %sThe copy already saved is left alone.%s", C_GRAY, C_RESET);
            else
                out_multi("  %sThe update continues, but there will be nothing to go back to.%s",
                          C_GRAY, C_RESET);
        }
        return false;
    }

    // Now it is safe to clear. The cfg goes first, so that from here until the
    // new one is written there is officially nothing saved — which is true, and
    // is what anything interrupted in between should conclude.
    storage_remove(ROLLBACK_CFG);
    storage_remove(ROLLBACK_IMG);

    if (announce) out_info("Saving the current firmware (%lu KB)...",
                           (unsigned long)(size / 1024));

    void *sink = storage_open_sink(ROLLBACK_IMG);
    if (!sink) {
        if (announce) out_err("Could not write %s.", ROLLBACK_IMG);
        return false;
    }

    // Hashed on the way out rather than by reading the file back afterwards.
    // The bytes going into the file are the bytes being hashed, so a write that
    // lands wrong is caught when the hash is checked at restore time, and the
    // capture stays one pass over 700 KB instead of two.
    Sha256Ctx c; sha256_init(&c);
    const uint8_t *fw = (const uint8_t *)XIP_BASE;
    uint32_t done = 0;
    bool ok = true;
    while (done < size && ok) {
        uint32_t n = size - done > CHUNK ? CHUNK : size - done;
        sha256_update(&c, fw + done, n);
        ok = storage_sink_write(sink, fw + done, n);
        done += n;
        if (announce) out_progress("Saving", done, size);
        task_alive();                 // several seconds of flash writes
        task_watchdog_feed();
    }
    if (!storage_close_sink(sink)) ok = false;
    if (announce) out_progress_done();

    if (!ok) {
        storage_remove(ROLLBACK_IMG);
        if (announce) out_err("Could not save the firmware. No rollback copy was kept.");
        return false;
    }

    uint8_t d[32]; sha256_final(&c, d);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);

    char cfg[320];
    int n = snprintf(cfg, sizeof(cfg),
                     "ver=%s\nboard=%s\nsize=%lu\nreserve=%lu\nsha=%s\n",
                     RPC_OS_VERSION, PICO_BOARD,
                     (unsigned long)size, (unsigned long)storage_reserve_bytes(), hex);

    void *csink = storage_open_sink(ROLLBACK_CFG);
    if (!csink || !storage_sink_write(csink, (const uint8_t *)cfg, (uint32_t)n) ||
        !storage_close_sink(csink)) {
        if (csink) storage_close_sink(csink);
        storage_remove(ROLLBACK_CFG);
        storage_remove(ROLLBACK_IMG);
        if (announce) out_err("Could not finish saving the rollback copy.");
        return false;
    }

    if (announce) out_ok("Rollback copy saved: %s (%lu KB).",
                         RPC_OS_VERSION, (unsigned long)(size / 1024));
    log_addf(LOG_K_OK, "rollback: saved %s (%lu bytes)",
             RPC_OS_VERSION, (unsigned long)size);
    return true;
}

// --- putting it back ---------------------------------------------------------

// Check the saved image against its own hash before staging it. Reading 700 KB
// through the filesystem takes a moment, and it is the last point at which a
// bad copy can still be refused for free.
static bool image_matches(const RollbackInfo *info) {
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    if (!buf) return false;

    AppSource src{}; void *h = nullptr;
    if (!storage_open_source(ROLLBACK_IMG, &src, &h)) { free(buf); return false; }

    Sha256Ctx c; sha256_init(&c);
    uint32_t at = 0, left = info->size;
    bool ok = true;
    while (left && ok) {
        uint32_t n = left > CHUNK ? CHUNK : left;
        ok = src.read(src.ctx, at, buf, n) >= 0;
        if (ok) { sha256_update(&c, buf, n); at += n; left -= n; }
        task_alive();
        task_watchdog_feed();
    }
    storage_close_source(h);
    free(buf);
    if (!ok) return false;

    uint8_t d[32]; sha256_final(&c, d);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return strcmp(hex, info->sha) == 0;
}

bool rollback_apply(bool at_boot) {
    RollbackInfo info;
    if (!rollback_read(&info)) {
        out_err("There is no saved firmware to go back to.");
        out_multi("  A copy is kept automatically when an update is applied,");
        out_multi("  and 'update rollback --save' keeps one now.");
        return false;
    }

    out_info("Checking the saved image...");
    if (!image_matches(&info)) {
        out_err("The saved image does not match its checksum. Refusing to write it.");
        out_multi("  Removing it, because a copy that cannot be trusted is worse");
        out_multi("  than none: it would be tried again at the next failure.");
        rollback_forget();
        return false;
    }
    out_ok("Saved image verified: %s.", info.ver);

    // The registry is the only thing that survives a firmware write, so the
    // reason for the reboot is written into it before anything is erased.
    //
    // The update keys are CLEARED here on purpose. A rollback happens after an
    // update that did not work, so those keys are still set and still say
    // "updated to <the broken version>" — and the restored firmware would
    // announce that as its good news on the very boot that undid it.
    reg_set("System.Update_From", "");
    reg_set("System.Update_To", "");
    reg_set("System.Rollback_From", RPC_OS_VERSION);
    reg_set("System.Rollback_To", info.ver);
    persist_save_registry();

    // Used once, and marked used BEFORE the write rather than after — because
    // there is no after. Renaming the cfg is what makes the pair invalid, so a
    // device that keeps failing cannot restore the same image over and over in
    // a loop that never reaches the filesystem rebuild behind it.
    //
    // A rename survives a power cut, which is the point. A flag in a scratch
    // register would not.
    storage_remove("/os/rollback.used");
    storage_rename(ROLLBACK_CFG, "/os/rollback.used");

    log_addf(LOG_K_WARN, "rollback: restoring %s over %s", info.ver, RPC_OS_VERSION);

    if (at_boot) {
        // The restored firmware deserves its own three attempts. Without this it
        // inherits a counter already at three and rebuilds the filesystem on its
        // first boot — punishing the image that was brought back to help.
        kboot_clear_strikes();
    }

    out_blank();
    out_warn("Restoring %s over %s.", info.ver, RPC_OS_VERSION);

    // Does not return when it works.
    if (update_apply_file(ROLLBACK_IMG, info.ver, at_boot)) return true;

    // It did not get as far as writing anything, so the copy is still good and
    // the notes for the next boot are now lies. Both are put back.
    storage_rename("/os/rollback.used", ROLLBACK_CFG);
    reg_set("System.Rollback_From", "");
    reg_set("System.Rollback_To", "");
    persist_save_registry();
    out_err("Nothing was written. The saved copy is still there.");
    return false;
}

// The automatic path. Deliberately quiet about what it could not do: at this
// point in a boot the caller has a filesystem rebuild lined up behind it, and
// two competing explanations of the same failure help nobody.
bool rollback_try_at_boot(void) {
    RollbackInfo info;
    if (!rollback_read(&info)) return false;

    out_blank();
    out_warn("This device has failed to start three times.");
    out_multi("  A copy of %s was saved before the last firmware change.", info.ver);
    out_multi("  Putting it back now. This takes about a minute.");
    out_blank();

    return rollback_apply(/*at_boot*/true);
}

// --- the command -------------------------------------------------------------

static void show(void) {
    RollbackInfo info;
    out_info("=== Firmware rollback ===");
    out_blank();
    out_multi("  Running    %s %s", RPC_OS_VERSION, RPC_OS_CODENAME);

    if (!rollback_read(&info)) {
        bool d = false; uint32_t orphan = 0;
        out_multi("  Saved      %snothing%s", C_WARN, C_RESET);
        out_blank();

        // Some boards cannot do this at all, and saying so is better than
        // leaving somebody to conclude their device is broken. The image has to
        // fit in the filesystem beside everything else, and on a part where the
        // firmware is twice the size of the filesystem it never will.
        if (storage_firmware_bytes() >= storage_total_bytes()) {
            out_multi("  %sThis board cannot keep one: the firmware is %lu KB and the whole%s",
                      C_GRAY, (unsigned long)(storage_firmware_bytes() / 1024), C_RESET);
            out_multi("  %sfilesystem is %lu KB. Reinstall over USB to change versions.%s",
                      C_GRAY, (unsigned long)(storage_total_bytes() / 1024), C_RESET);
            return;
        }

        if (storage_stat(ROLLBACK_IMG, &d, &orphan) && orphan)
            out_multi("  %sThere is a %lu KB image with no valid record beside it.%s",
                      C_GRAY, (unsigned long)(orphan / 1024), C_RESET);
        out_multi("  A copy is saved automatically when an update is applied.");
        out_multi("  'update rollback --save' saves one from what is running now.");
        return;
    }

    out_multi("  Saved      %s%s%s  (%lu KB)", C_CYAN, info.ver, C_RESET,
              (unsigned long)(info.size / 1024));
    out_blank();
    out_multi("  'update rollback --now' writes it back and reboots.");
    out_multi("  'update rollback --forget' deletes it and frees %lu KB.",
              (unsigned long)(info.size / 1024));
}

int rollback_command(int argc, char **argv) {
    // argv[0] is "update", argv[1] is "rollback"
    const char *opt = argc >= 3 ? argv[2] : "";

    if (!opt[0]) { show(); return 0; }

    if (!strcmp(opt, "--save") || !strcmp(opt, "save")) {
        if (!users_is_admin(session_user())) {
            out_err("Only an admin can save a firmware copy.");
            return 1;
        }
        return rollback_capture(/*announce*/true) ? 0 : 1;
    }

    if (!strcmp(opt, "--forget") || !strcmp(opt, "forget")) {
        if (!users_is_admin(session_user())) {
            out_err("Only an admin can remove the firmware copy.");
            return 1;
        }
        RollbackInfo info;
        bool had = rollback_read(&info);
        rollback_forget();
        storage_remove("/os/rollback.used");
        if (had) out_ok("Removed the saved %s image. %lu KB freed.",
                        info.ver, (unsigned long)(info.size / 1024));
        else     out_ok("There was nothing saved. Cleaned up anyway.");
        return 0;
    }

    if (!strcmp(opt, "--now") || !strcmp(opt, "now")) {
        if (!users_is_admin(session_user())) {
            out_err("Only an admin can change the firmware.");
            return 1;
        }
        RollbackInfo info;
        if (!rollback_read(&info)) {
            out_err("There is nothing saved to go back to.");
            return 1;
        }
        out_warn("This replaces %s with %s.", RPC_OS_VERSION, info.ver);
        out_multi("  Files and settings are untouched; only the firmware changes.");
        if (!session_confirm("  Continue?")) { out_info("Cancelled."); return 0; }
        return rollback_apply(/*at_boot*/false) ? 0 : 1;
    }

    out_multi("Usage:");
    out_multi("  update rollback             what is saved");
    out_multi("  update rollback --now       write it back and reboot");
    out_multi("  update rollback --save      save what is running now");
    out_multi("  update rollback --forget    delete it and free the space");
    return 1;
}
