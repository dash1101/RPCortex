// update — replacing the firmware over the network.
//
// This is the one operation that can leave a device unbootable, so the order is
// chosen so that every failure before the very last step costs nothing:
//
//   1. fetch the manifest and compare versions
//   2. stream the image to the FILESYSTEM, never to flash
//   3. verify its SHA-256 against the manifest
//   4. check it is a plausible image for this board
//   5. only now write it over the firmware, from RAM, with interrupts off
//   6. reboot
//
// Steps 1-4 can fail freely: the running firmware is untouched and the worst
// outcome is a wasted download. Step 5 is the only dangerous one, and it is
// deliberately as short as it can be — a straight copy with no decisions in it,
// because every decision is a chance to get it wrong while the device has no
// working firmware.
//
// CONFIRMED ON HARDWARE, 2026-08-05, on a Pico 2 W: staged, verified, wrote a
// 955 KB image over the running firmware and came back up reporting the version
// it had moved to. `update from-file` on a locally built image is the way to
// exercise step 5 with nothing depending on the network.
//
// Steps 1 to 3 (`update check`, `update install`) are in update_net.cpp, so an
// image built without Wi-Fi leaves them out. Everything from staging on is
// here, because `update from-file` and the recovery ladder's rollback need it
// in every image.

#include "command.h"
#include "out.h"
#include "storage.h"
#include "sha256.h"
#include "interrupt.h"
#include "kernel.h"
#include "blackbox.h"
#include "registry.h"
#include "persist.h"
#include "task.h"
#include "users.h"
#include "session.h"
#include "logring.h"
#include "rollback.h"
#include "update.h"
#include "imagecheck.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <initializer_list>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/regs/watchdog.h"
#include "hardware/regs/addressmap.h"
#include "pico/multicore.h"

// --- writing the firmware ---------------------------------------------------

// Copy the staged image over the firmware, one sector at a time.
//
// __not_in_flash_func is not decoration. XIP is UNAVAILABLE while flash is
// being erased or programmed, so every instruction this executes has to already
// be in RAM — and so does everything it calls. It calls nothing.
//
// It reads from a FIXED FLASH OFFSET rather than from a file, and that is the
// whole reason the staging slot exists. Source and destination are the same
// chip: the moment the firmware region is erased, littlefs is erased with it,
// so reading the image through the filesystem stops being possible exactly when
// it would be needed. A raw offset needs nothing but the address.
//
// Sector at a time, through a 4 KB buffer, because loading a 694 KB image into
// a 370 KB heap is not possible and never was. That was the previous version's
// mistake.
//
// The window in which this device cannot boot runs from the first erase to the
// last program. Nothing that can fail happens inside it: no allocation, no
// filesystem, no formatting, no decisions.
// __no_inline_not_in_flash_func, and the "no_inline" half is not optional.
//
// __not_in_flash_func places the function in RAM but does nothing to stop the
// compiler INLINING it into its caller — and its caller lives in flash. A
// static function called exactly once gets inlined every time, so the whole
// copy loop was executing from the flash it was erasing. It died on the first
// erase, which is exactly the lockup.
//
// Verified with nm rather than assumed: this symbol must resolve to 0x2xxxxxxx.
static void __no_inline_not_in_flash_func(apply_staged)(uint32_t stage_off, uint32_t len,
                                                        uint8_t *buf) {
    uint32_t total = (len + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);

    // Interrupts stay off for the WHOLE copy, not per operation.
    //
    // This is the difference between working and a dead device. The vector
    // table lives in flash at offset 0 — the first thing erased — so from the
    // moment the erase begins there is nowhere for an interrupt to go. It
    // vectors into erased flash, faults, and the fault handler has been erased
    // too. The result is a hard lockup with no output and a half-written image,
    // which is exactly what pulling the plug then leaves behind.
    //
    // Re-enabling them between sectors, as this did before, meant the first
    // timer tick after the erase killed it every time.
    //
    // Several seconds with interrupts off drops the USB connection. That is
    // expected and harmless: the reboot below brings it back.
    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(0, total);

    uint32_t done = 0;
    while (done < total) {
        // Reading the staging slot still works: it is not being erased, and the
        // SDK keeps a RAM copy of boot2 so XIP is restored after each
        // operation. That RAM copy was populated by staging, before any of
        // this — which is the only reason reading flash here is possible at all.
        const uint8_t *src = (const uint8_t *)(XIP_BASE + stage_off + done);
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++)
            buf[i] = (done + i < len) ? src[i] : 0xff;

        flash_range_program(done, buf, FLASH_SECTOR_SIZE);
        done += FLASH_SECTOR_SIZE;
    }

    // Reboot from HERE. This must never return.
    //
    // The caller lives in the flash that was just overwritten. Its return
    // address points at whatever the NEW image happens to put at that offset —
    // which is not the rest of update_apply_file, and on a real version change
    // is not a function boundary at all. Returning executes whatever is there.
    //
    // On a --force reinstall of an identical image that would accidentally
    // work, which is the worst kind of bug: it appears fine until the first
    // update that actually changes something, and then hangs with no output
    // because the write itself had already succeeded.
    //
    // Interrupts are deliberately NOT restored. The vector table on disk is the
    // new one, but nothing above has been re-entered and there is nothing left
    // to return to.
    (void)ints;
    // Reset by writing the watchdog register directly, NOT through
    // watchdog_reboot() — that function lives in flash, and flash no longer
    // holds what it did. Calling into it here branches into a region erased
    // seconds ago, and faults with no handler left to catch it.
    //
    // A single volatile store with no call in it is the only kind of sequence
    // that is safe at this point.
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    while (true) __asm volatile ("nop");
}

// Called every sector while staging. It draws the bar, and — the part that
// matters — it reaches the scheduler, which feeds the watchdog.
//
// Without it, staging 696 KB is around 174 erase-and-program cycles with
// nothing yielding: several seconds against an 8 second watchdog, which
// rebooted the device in the middle of the copy every time.
#if CFG_TUD_MSC
bool usbdrv_release_for_update(void);
#else
static inline bool usbdrv_release_for_update(void) { return false; }
#endif

static void stage_progress(void *, uint32_t done, uint32_t total) {
    out_progress("Staging", done, total);
    task_alive();
    // AND the hardware watchdog directly. task_alive is a no-op until the
    // scheduler is up, and staging also happens at boot now — on the rollback
    // path, before there is a scheduler to reach. Without this the automatic
    // rollback resets the device halfway through the copy and looks like it
    // simply does not work.
    task_watchdog_feed();
}

// Stage the image, then copy it over the firmware.
//
// Two steps because source and destination share a flash chip. Staging is safe
// and interruptible — it touches nothing the device needs — and it puts the
// image at a fixed offset the final copy can read without a filesystem.
bool update_apply_file(const char *path, const char *to_version, bool at_boot) {
    bool is_dir = false; uint32_t size = 0;
    if (!storage_stat(path, &is_dir, &size) || size == 0) {
        out_err("No image at %s.", path);
        return false;
    }
    if (size > storage_fw_slot_bytes()) {
        out_err("Image is %lu KB; the firmware slot is %lu KB.",
                (unsigned long)(size / 1024),
                (unsigned long)(storage_fw_slot_bytes() / 1024));
        return false;
    }

    // The staging slot is also the USB transfer area, and an image written over
    // a volume the host may still have mounted is the worst of both: a corrupt
    // filesystem AND a corrupt firmware image, with the update believing it
    // succeeded. So the area is given up deliberately, once, and the drive is
    // taken away from the host first.
    //
    // Not a warning to be dismissed — anything on the drive is gone, and
    // whoever put it there should be told rather than discovering it later.
    if (!at_boot && usbdrv_release_for_update()) {
        out_warn("The USB drive shares this space and has been cleared.");
        out_multi("  %sUnplug and replug after the update to get it back.%s",
                  C_GRAY, C_RESET);
    }

    out_info("Staging %lu KB...", (unsigned long)(size / 1024));
    uint32_t staged = storage_stage_file(path, stage_progress, nullptr);
    out_progress_done();
    if (staged != size) {
        out_err("Could not stage the image (%lu of %lu bytes).",
                (unsigned long)staged, (unsigned long)size);
        return false;
    }

    // Verify what actually landed in the staging slot, by reading it back the
    // same way the copier will. Everything up to here could still be undone;
    // after this nothing can, so this is the last chance to find a problem.
    Sha256Ctx c; sha256_init(&c);
    sha256_update(&c, (const uint8_t *)(XIP_BASE + storage_stage_offset()), staged);
    uint8_t d1[32]; sha256_final(&c, d1);

    // Stream the source file through the loader's positional reader rather
    // than adding another way to read a file.
    uint8_t *fbuf = (uint8_t *)malloc(4096);
    if (!fbuf) { out_err("Not enough memory to verify the staged image."); return false; }
    AppSource src{}; void *h = nullptr;
    if (!storage_open_source(path, &src, &h)) {
        free(fbuf);
        out_err("Could not reopen the image to verify it.");
        return false;
    }
    Sha256Ctx c2; sha256_init(&c2);
    uint32_t left = size, at = 0;
    bool readok = true;
    while (left && readok) {
        uint32_t n = left > 4096 ? 4096 : left;
        readok = src.read(src.ctx, at, fbuf, n) >= 0;
        if (readok) { sha256_update(&c2, fbuf, n); at += n; left -= n; }
    }
    storage_close_source(h);
    if (!readok) { free(fbuf); out_err("Could not read the image back."); return false; }
    uint8_t d2[32]; sha256_final(&c2, d2);
    if (memcmp(d1, d2, 32) != 0) {
        free(fbuf);
        out_err("The staged copy does not match the download. Nothing written.");
        return false;
    }
    // One last check on what is actually in the staging slot, because the next
    // thing that happens cannot be undone: is it firmware for THIS chip? An
    // RP2040 image and an RP2350 image both look like Cortex-M firmware, and the
    // check here used to accept either layout on either chip. imagecheck.h has
    // the layouts; host-tested against every board's release image.
    {
        const uint8_t *base = (const uint8_t *)(XIP_BASE + storage_stage_offset());
#if PICO_RP2350
        const ImageChip chip = IMAGE_CHIP_RP2350;
        const char *chip_name = "RP2350";
#else
        const ImageChip chip = IMAGE_CHIP_RP2040;
        const char *chip_name = "RP2040";
#endif
        uint32_t sp = 0, entry = 0;
        if (!image_fits_chip(base, 4096, chip, &sp, &entry)) {
            free(fbuf);
            out_err("The staged image is not firmware for this board's chip (%s).", chip_name);
            out_multi("  stack %08lx  entry %08lx", (unsigned long)sp, (unsigned long)entry);
            out_multi("  Nothing was written.");
            return false;
        }
    }

    out_ok("Staged and verified.");

    // Keep a copy of what is running, so this update can be undone.
    //
    // HERE and not earlier, for space. The downloaded file is most of a
    // megabyte and so is the copy, and holding both at once needs one and a
    // half megabytes of a filesystem that may only have two. By this point the
    // slot holds a verified copy of the download, so the download itself is
    // redundant and can go — which leaves room for the rollback image in a
    // filesystem that could not have held both.
    //
    // Only the file this command downloaded is removed. `update from-file`
    // points at somebody's own image and deleting it would be theft.
    //
    // Skipped entirely on the rollback path: that IS a saved image being put
    // back, and capturing the broken firmware over it would trade a copy that
    // works for one that does not.
    if (!at_boot && strcmp(path, ROLLBACK_IMG) != 0) {
        if (!strcmp(path, IMAGE_PATH)) storage_remove(IMAGE_PATH);
        rollback_capture(/*announce*/true);
        out_blank();
    }

    // A note to the NEXT boot, written before anything is erased.
    //
    // The firmware write leaves the filesystem alone, so the registry is the
    // one thing guaranteed to survive it. Without this an update that works is
    // indistinguishable from one that did nothing: the device reboots, comes
    // back, and says nothing about why.
    //
    // Not written for a rollback, which has left its OWN note and means the
    // opposite thing. Writing both left the update keys set behind the rollback
    // keys, and the boot after the one that reported the rollback would cheerily
    // announce having been updated to the version it had just undone.
    if (strcmp(path, ROLLBACK_IMG) != 0) {
        reg_set("System.Update_From", RPC_OS_LONG);
        reg_set("System.Update_To", to_version ? to_version : "a local image");
        // Installing it on purpose withdraws the objection to it. Whoever typed
        // --force has been told and has decided, and leaving the note behind
        // would have the device arguing with them at every check afterwards.
        if (to_version && !strcmp(reg_get("System.Rollback_Refused", ""), to_version))
            reg_set("System.Rollback_Refused", "");
        persist_save_registry();
    }

    log_addf(LOG_K_WARN, "update: writing %lu bytes of firmware", (unsigned long)size);

    // Say plainly what is about to happen, because what it LOOKS like is a
    // crash.
    //
    // The write needs interrupts off from the first erase to the last program —
    // the vector table is in the flash being erased, so there is nowhere for an
    // interrupt to go. USB is an interrupt-driven device, so the serial
    // connection drops for the whole operation: no output, no echo, nothing.
    //
    // That is several seconds of a terminal that appears dead, and pulling the
    // plug during it leaves a half-written image and a board that needs
    // BOOTSEL. Which is exactly what happens if nobody warns you.
    out_blank();
    out_warn("The console will go SILENT for up to 30 seconds now.");
    out_multi("  That is normal: USB stops while flash is being written.");
    out_multi("  %sDo not unplug the device%s, however dead it looks.", C_BOLD, C_RESET);
    out_multi("  It reboots by itself when it finishes.");
    out_blank();
    for (int i = 3; i > 0; i--) {
        out_progress("Starting in", (uint64_t)(3 - i), 3);
        sleep_ms(600);
    }
    out_progress_done();
    // THIS RESTART IS ON PURPOSE, so the next boot must not report it as a
    // crash. Without this, the first thing anyone sees after their first
    // successful update is
    //
    //   [!] [Crash] Last run stopped while running 'shell' (pid 3, core 0)
    //       Command : update from-file ...
    //       Reached : app_main returned
    //
    // — a crash report, a stale phase left by some package, and a watchdog
    // notice, all describing an update that worked. `reboot` already had this
    // problem and this is the same fix: a crash detector that cries wolf on
    // every restart is how a real crash gets scrolled past.
    //
    // Before the write rather than after, because there is no after: the code
    // that would run it has been erased by then.
    bb_note_clean_exit();
    kboot_expect_reboot();

    out_info("Writing firmware...");
    out_flush();
    sleep_ms(150);                     // let the serial buffer actually drain

    // Stop core 1 before touching flash.
    //
    // It is executing from flash, and XIP is unavailable while flash is being
    // erased — so it would fetch garbage the instant the erase began. That is
    // the same fault that corrupted the filesystem earlier in this project,
    // and here it corrupts the firmware instead: the write is left incomplete,
    // the image is invalid, and the boot ROM drops to USB.
    //
    // Reset rather than parked, since this function does not return.
    //
    // Skipped on the boot path, where core 1 has not been started yet: it is
    // still sitting in the boot ROM's wait loop, executing from ROM rather than
    // from the flash about to be erased, and there is nothing to stop.
    if (!at_boot) multicore_reset_core1();

    // And stop the watchdog. 695 KB is 174 sectors of erase and program, which
    // takes several seconds, and nothing feeds the watchdog while interrupts
    // are off. An 8 second timeout against a 5-9 second write is a reboot in
    // the middle of it.
    watchdog_disable();

    // fbuf becomes the copier's sector buffer: allocated HERE, because nothing
    // may allocate once the erase has begun.
    // Does not return: it reboots from inside, because the code that called it
    // no longer exists by the time it finishes.
    apply_staged(storage_stage_offset(), size, fbuf);
    return true;                       // unreachable
}

static int cmd_update(int argc, char **argv) {
    const char *sub = argc >= 2 ? argv[1] : "check";

    if (!strcmp(sub, "check"))  return update_check(false);
    if (!strcmp(sub, "install") || !strcmp(sub, "online")) {
        // v1's spellings: --force/-f and --yes/-y, in either order.
        bool force = false, yes = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--force") || !strcmp(argv[i], "-f")) force = true;
            else if (!strcmp(argv[i], "--yes") || !strcmp(argv[i], "-y")) yes = true;
            else { out_err("Unknown option: %s", argv[i]); return 1; }
        }
        return update_online(force, yes);
    }
    if (!strcmp(sub, "rollback") || !strcmp(sub, "revert"))
        return rollback_command(argc, argv);

    if (!strcmp(sub, "from-file")) {
        // --yes/-y answers the question, as `update online` takes it: a host tool
        // sending the image over the link runs this through EXEC, where nobody
        // is at a prompt to answer it. The path is the first other argument.
        const char *file = nullptr;
        bool yes = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--yes") || !strcmp(argv[i], "-y")) yes = true;
            else if (!file) file = argv[i];
            else { out_err("Unknown option: %s", argv[i]); return 1; }
        }
        if (!file) { out_multi("Usage: update from-file <image.bin> [--yes]"); return 1; }
        if (!users_is_admin(session_user())) { out_err("Only an admin can update the firmware."); return 1; }
        out_warn("Writing %s over the firmware, unverified.", file);
        out_multi("  A local file has no checksum to check it against.");
        if (!yes && !session_confirm("  Continue?")) return 0;
        return update_apply_file(file, nullptr, /*at_boot*/false) ? 0 : 1;
    }

    out_multi("Usage:");
    out_multi("  update check              is there a newer release");
    out_multi("  update install            fetch, verify and apply it");
    out_multi("  update install --force    reinstall the current version");
    out_multi("  update from-file <path> [--yes]   apply an image already on the device");
    out_multi("  update rollback           go back to the previous firmware");
    return 1;
}

// Called at boot. Reports a completed update once, then forgets it.
void update_report_boot(void) {
    // A rollback is reported the same way an update is, and takes precedence:
    // both keys can never be set at once, because applying either clears the
    // other before it writes.
    const char *back_to = reg_get("System.Rollback_To", "");
    if (back_to[0]) {
        const char *back_from = reg_get("System.Rollback_From", "");
        out_warn("Rolled back to %s.", back_to);
        out_multi("  %s did not start, so the saved copy was put back.",
                  back_from[0] ? back_from : "The previous firmware");
        out_multi("  Files and settings were not touched.");
        log_addf(LOG_K_WARN, "rollback: now running %s (was %s)",
                 RPC_OS_LONG, back_from);

        reg_set("System.Rollback_To", "");
        reg_set("System.Rollback_From", "");
        persist_save_registry();
        // Rollback_Refused deliberately survives this: it names the version that
        // failed, and this boot is the one that came back from it. It is cleared
        // by that version being installed again on purpose, and by nothing else.

        // The copy has been spent. Clearing it reclaims most of a megabyte, and
        // leaving it would mean the NEXT capture has to find room beside it.
        rollback_forget();
        storage_remove("/os/rollback.used");
        storage_remove(IMAGE_PATH);
        return;
    }

    const char *to = reg_get("System.Update_To", "");
    if (!to[0]) return;
    const char *from = reg_get("System.Update_From", "");

    out_ok("Updated from %s to %s.", from[0] ? from : "an earlier build", to);
    out_multi("  Now running %s %s.", RPC_OS_LONG, RPC_OS_CODENAME);
    log_addf(LOG_K_OK, "update: now running %s (was %s)", RPC_OS_LONG, from);

    reg_set("System.Update_To", "");
    reg_set("System.Update_From", "");
    persist_save_registry();

    // The downloaded image has done its job and is most of a megabyte.
    storage_remove(IMAGE_PATH);
    storage_remove(MANIFEST_PATH);
}

void update_register(void) {
    static const Command c{"update", "check for and apply a firmware update", cmd_update,
                           nullptr, LEVEL_ADMIN, FORMS_UPDATE};
    cmd_register(&c);
}
