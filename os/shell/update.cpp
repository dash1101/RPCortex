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
// DEVICE-UNCONFIRMED. The download, the hash and the checks are exercised by
// the same code the package manager uses. Writing the firmware region can only
// be proven by doing it.

#include "command.h"
#include "out.h"
#include "httpfetch.h"
#include "repoindex.h"
#include "storage.h"
#include "sha256.h"
#include "interrupt.h"
#include "kernel.h"
#include "registry.h"
#include "persist.h"
#include "task.h"
#include "users.h"
#include "session.h"
#include "logring.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <initializer_list>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/regs/addressmap.h"
#include "pico/multicore.h"

bool update_apply_file(const char *path, const char *to_version);   // defined below

bool http_transport_get(HttpTransport *t);
bool http_tls_available(void);
const char *http_tls_why(void);
void http_last_detail(char *out, unsigned cap);

#define MANIFEST_URL  "https://raw.githubusercontent.com/dash1101/RPCortex/main/releases/latest.json"
#define IMAGE_PATH    "/os/update.bin"
#define MANIFEST_PATH "/os/update.json"
#define MANIFEST_MAX  2048

// Which board this image is for. An image for another one boots into nothing.
#if defined(RASPBERRYPI_PICO2_W)
#define MY_BOARD "pico2_w"
#elif defined(RASPBERRYPI_PICO_W)
#define MY_BOARD "pico_w"
#elif defined(RASPBERRYPI_PICO2)
#define MY_BOARD "pico2"
#else
#define MY_BOARD "pico"
#endif

// --- downloading ------------------------------------------------------------

struct Sink { void *fh; Sha256Ctx sha; };

static int sink_write(void *ctx, const uint8_t *d, uint32_t n) {
    Sink *s = (Sink *)ctx;
    sha256_update(&s->sha, d, n);
    return storage_sink_write(s->fh, d, n) ? 0 : -1;
}

static int poll_intr(void *) { return intr_check() ? 1 : 0; }

static void progress(void *, uint64_t got, uint64_t total) {
    // Every 4 KB rather than every packet: often enough to look continuous,
    // rare enough that drawing it does not slow the thing it is measuring.
    static uint64_t last;
    if (got < last) last = 0;
    if (got - last < 4096 && got != total) return;
    last = got;
    out_progress("Downloading", got, total);
}

static void hex_of(const uint8_t *d, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i * 2] = h[d[i] >> 4]; out[i * 2 + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

static bool download(const char *url, const char *dest, char *hex, uint64_t *bytes) {
    HttpTransport t;
    if (!http_transport_get(&t)) { out_err("No network. Connect first."); return false; }
    if (!strncmp(url, "https://", 8) && !http_tls_available()) {
        out_err("HTTPS cannot be verified: %s", http_tls_why());
        return false;
    }

    Sink s{nullptr, {}};
    sha256_init(&s.sha);
    s.fh = storage_open_sink(dest);
    if (!s.fh) { out_err("Could not write %s.", dest); return false; }

    FetchOpts o{};
    o.poll = poll_intr;
    o.progress = progress;
    uint32_t room = storage_free_bytes();
    o.max_bytes = room > 16384 ? room - 16384 : 1;

    FetchResult r;
    bool good = http_fetch(&t, url, sink_write, &s, &o, &r);
    if (!storage_close_sink(s.fh) && good) { good = false; r.error = FETCH_ERR_SINK; }
    out_progress_done();

    if (!good) {
        storage_remove(dest);
        out_err("%s%s%s", fetch_error_str(r.error), r.detail[0] ? " - " : "", r.detail);
        char d[96]; http_last_detail(d, sizeof(d));
        out_multi("  %s", d);
        return false;
    }
    uint8_t digest[32];
    sha256_final(&s.sha, digest);
    hex_of(digest, hex);
    *bytes = r.bytes;
    return true;
}

// --- the manifest -----------------------------------------------------------
//
// Same shape as a package index entry, so the same scanner reads it: one
// "packages" array whose entries are per board. That is deliberate — a second
// format would be a second parser to get wrong.

static bool fetch_manifest(RepoEntry *out) {
    out_info("Checking for an update...");
    char hex[65]; uint64_t n = 0;
    if (!download(MANIFEST_URL, MANIFEST_PATH, hex, &n)) return false;

    char *buf = (char *)malloc(MANIFEST_MAX);
    if (!buf) { out_err("Not enough memory."); return false; }
    uint32_t len = storage_read_file(MANIFEST_PATH, (uint8_t *)buf, MANIFEST_MAX - 1);
    buf[len] = 0;
    bool hit = repo_find(buf, len, MY_BOARD, out);
    free(buf);

    if (!hit) {
        out_err("No release listed for this board (%s).", MY_BOARD);
        return false;
    }
    return true;
}

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
static void __not_in_flash_func(apply_staged)(uint32_t stage_off, uint32_t len,
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

    restore_interrupts(ints);
}

// --- the command ------------------------------------------------------------

static int do_check(bool quiet) {
    RepoEntry e;
    if (!fetch_manifest(&e)) return 1;

    int cmp = repo_version_cmp(e.ver, RPC_OS_VERSION + 1);   // skip the leading 'v'
    out_multi("  Installed  %s", RPC_OS_VERSION);
    out_multi("  Available  %s", e.ver);
    if (e.size) out_multi("  Size       %lu KB", (unsigned long)(e.size / 1024));

    if (cmp <= 0) {
        if (!quiet) out_ok("Already up to date.");
        return 0;
    }
    out_info("An update is available. 'update install' to fetch and apply it.");
    return 0;
}

static int do_install(bool force) {
    if (!users_is_admin(session_user())) {
        out_err("Only an admin can update the firmware.");
        return 1;
    }

    RepoEntry e;
    if (!fetch_manifest(&e)) return 1;

    if (!force && repo_version_cmp(e.ver, RPC_OS_VERSION + 1) <= 0) {
        out_ok("Already running %s. 'update install --force' to reinstall.", RPC_OS_VERSION);
        return 0;
    }
    if (!e.sha256[0]) {
        // Refused rather than warned. A package installed unverified costs a
        // package; firmware installed unverified costs the device.
        out_err("The manifest publishes no checksum. Refusing to flash unverified.");
        return 1;
    }
    if (e.size && e.size > storage_fw_slot_bytes()) {
        out_err("The image is %lu KB; the firmware slot is %lu KB.",
                (unsigned long)(e.size / 1024),
                (unsigned long)(storage_fw_slot_bytes() / 1024));
        return 1;
    }

    out_info("Downloading %s (%lu KB)...", e.ver, (unsigned long)(e.size / 1024));
    char hex[65]; uint64_t got = 0;
    if (!download(e.url, IMAGE_PATH, hex, &got)) return 1;

    if (strcmp(hex, e.sha256) != 0) {
        storage_remove(IMAGE_PATH);
        out_err("Checksum mismatch. Nothing was written.");
        out_multi("  expected  %s", e.sha256);
        out_multi("  received  %s", hex);
        return 1;
    }
    out_ok("Checksum verified.");

    if (got == 0 || got > storage_fw_slot_bytes()) {
        storage_remove(IMAGE_PATH);
        out_err("The download is %lu bytes, which cannot be a firmware image.",
                (unsigned long)got);
        return 1;
    }

    out_blank();
    out_warn("About to replace the firmware. Do not remove power.");
    out_multi("  The device reboots into %s when it finishes.", e.ver);
    if (!session_confirm("  Continue?")) { out_info("Cancelled. The download is kept."); return 0; }

    return update_apply_file(IMAGE_PATH, e.ver) ? 0 : 1;
}

// Called every sector while staging. It draws the bar, and — the part that
// matters — it reaches the scheduler, which feeds the watchdog.
//
// Without it, staging 696 KB is around 174 erase-and-program cycles with
// nothing yielding: several seconds against an 8 second watchdog, which
// rebooted the device in the middle of the copy every time.
static void stage_progress(void *, uint32_t done, uint32_t total) {
    out_progress("Staging", done, total);
    task_alive();
}

// Stage the image, then copy it over the firmware.
//
// Two steps because source and destination share a flash chip. Staging is safe
// and interruptible — it touches nothing the device needs — and it puts the
// image at a fixed offset the final copy can read without a filesystem.
bool update_apply_file(const char *path, const char *to_version) {
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
    // One last sanity check on what is actually in the staging slot, because
    // the next thing that happens cannot be undone.
    //
    // A Cortex-M image begins with an initial stack pointer and a reset vector:
    // the stack must point into SRAM, the entry point into flash. Anything else
    // is not firmware for this chip, and writing it leaves a board only BOOTSEL
    // can recover.
    //
    // WHERE that pair sits differs by chip, which is worth stating because
    // checking only one place rejects half the boards. An RP2040 image begins
    // with the 256-byte second-stage bootloader and its vector table follows at
    // 0x100; an RP2350 image starts with the table. Both are accepted, and a
    // real image for either was checked against this before it shipped.
    {
        const uint8_t *base = (const uint8_t *)(XIP_BASE + storage_stage_offset());
        bool sane = false;
        uint32_t sp = 0, entry = 0;
        for (uint32_t off : {0u, 0x100u}) {
            const uint32_t *v = (const uint32_t *)(base + off);
            if (v[0] >= 0x20000000u && v[0] < 0x20090000u &&
                v[1] >= 0x10000000u && v[1] < 0x10200000u) {
                sane = true;
                sp = v[0]; entry = v[1];
                break;
            }
            if (!sp) { sp = v[0]; entry = v[1]; }
        }
        if (!sane) {
            free(fbuf);
            out_err("The staged image does not look like firmware for this board.");
            out_multi("  stack %08lx  entry %08lx", (unsigned long)sp, (unsigned long)entry);
            out_multi("  Nothing was written.");
            return false;
        }
    }

    out_ok("Staged and verified.");

    // A note to the NEXT boot, written before anything is erased.
    //
    // The firmware write leaves the filesystem alone, so the registry is the
    // one thing guaranteed to survive it. Without this an update that works is
    // indistinguishable from one that did nothing: the device reboots, comes
    // back, and says nothing about why.
    reg_set("System.Update_From", RPC_OS_VERSION);
    reg_set("System.Update_To", to_version ? to_version : "a local image");
    persist_save_registry();

    log_addf(LOG_K_WARN, "update: writing %lu bytes of firmware", (unsigned long)size);
    out_info("Writing firmware. Do not remove power.");
    sleep_ms(100);                     // let the serial buffer drain

    // Stop core 1 before touching flash.
    //
    // It is executing from flash, and XIP is unavailable while flash is being
    // erased — so it would fetch garbage the instant the erase began. That is
    // the same fault that corrupted the filesystem earlier in this project,
    // and here it corrupts the firmware instead: the write is left incomplete,
    // the image is invalid, and the boot ROM drops to USB.
    //
    // Reset rather than parked, since this function does not return.
    multicore_reset_core1();

    // And stop the watchdog. 695 KB is 174 sectors of erase and program, which
    // takes several seconds, and nothing feeds the watchdog while interrupts
    // are off. An 8 second timeout against a 5-9 second write is a reboot in
    // the middle of it.
    watchdog_disable();

    // fbuf becomes the copier's sector buffer: allocated HERE, because nothing
    // may allocate once the erase has begun.
    apply_staged(storage_stage_offset(), size, fbuf);

    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

static int cmd_update(int argc, char **argv) {
    const char *sub = argc >= 2 ? argv[1] : "check";

    if (!strcmp(sub, "check"))  return do_check(false);
    if (!strcmp(sub, "install") || !strcmp(sub, "online")) {
        bool force = (argc >= 3 && !strcmp(argv[2], "--force"));
        return do_install(force);
    }
    if (!strcmp(sub, "from-file")) {
        if (argc < 3) { out_multi("Usage: update from-file <image.bin>"); return 1; }
        if (!users_is_admin(session_user())) { out_err("Only an admin can update the firmware."); return 1; }
        out_warn("Writing %s over the firmware, unverified.", argv[2]);
        out_multi("  A local file has no checksum to check it against.");
        if (!session_confirm("  Continue?")) return 0;
        return update_apply_file(argv[2], nullptr) ? 0 : 1;
    }

    out_multi("Usage:");
    out_multi("  update check              is there a newer release");
    out_multi("  update install            fetch, verify and apply it");
    out_multi("  update install --force    reinstall the current version");
    out_multi("  update from-file <path>   apply an image already on the device");
    return 1;
}

// Called at boot. Reports a completed update once, then forgets it.
void update_report_boot(void) {
    const char *to = reg_get("System.Update_To", "");
    if (!to[0]) return;
    const char *from = reg_get("System.Update_From", "");

    out_ok("Updated from %s to %s.", from[0] ? from : "an earlier build", to);
    out_multi("  Now running %s %s.", RPC_OS_VERSION, RPC_OS_CODENAME);
    log_addf(LOG_K_OK, "update: now running %s (was %s)", RPC_OS_VERSION, from);

    reg_set("System.Update_To", "");
    reg_set("System.Update_From", "");
    persist_save_registry();

    // The downloaded image has done its job and is most of a megabyte.
    storage_remove(IMAGE_PATH);
    storage_remove(MANIFEST_PATH);
}

void update_register(void) {
    static const Command c{"update", "check for and apply a firmware update", cmd_update,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
