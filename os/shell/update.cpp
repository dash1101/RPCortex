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
#include "task.h"
#include "users.h"
#include "session.h"
#include "logring.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

bool update_apply_file(const char *path);   // defined below

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
    static uint64_t last;
    if (got < last) last = 0;
    if (got - last < 8192 && got != total) return;
    last = got;
    char line[64];
    int n = total
        ? snprintf(line, sizeof(line), "\r  %lu / %lu KB (%lu%%)   ",
                   (unsigned long)(got / 1024), (unsigned long)(total / 1024),
                   (unsigned long)(got * 100 / total))
        : snprintf(line, sizeof(line), "\r  %lu KB   ", (unsigned long)(got / 1024));
    if (n > 0) out_write(line, (uint32_t)n);
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
    out_write("\n", 1);

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

// Runs from RAM, with interrupts off and the other core parked.
//
// __not_in_flash_func is not decoration: XIP is UNAVAILABLE while flash is being
// erased or programmed, so any instruction still being fetched from flash would
// fault. Everything this touches has to be in RAM already, which is why it is a
// straight copy with no calls out of it.
//
// The window between the first erase and the last write is the only time this
// device cannot boot. It is kept as short as the operation allows, and nothing
// that could fail is done inside it.
static void __not_in_flash_func(apply_image)(const uint8_t *src, uint32_t len) {
    uint32_t sectors = (len + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    uint32_t total = sectors * FLASH_SECTOR_SIZE;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(0, total);
    flash_range_program(0, src, (len + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1));
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
    if (e.size && e.size > storage_reserve_bytes()) {
        out_err("The image is %lu KB; the firmware region is %lu KB.",
                (unsigned long)(e.size / 1024),
                (unsigned long)(storage_reserve_bytes() / 1024));
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

    if (got == 0 || got > storage_reserve_bytes()) {
        storage_remove(IMAGE_PATH);
        out_err("The download is %lu bytes, which cannot be a firmware image.",
                (unsigned long)got);
        return 1;
    }

    out_blank();
    out_warn("About to replace the firmware. Do not remove power.");
    out_multi("  The device reboots into %s when it finishes.", e.ver);
    if (!session_confirm("  Continue?")) { out_info("Cancelled. The download is kept."); return 0; }

    return update_apply_file(IMAGE_PATH) ? 0 : 1;
}

// Load the image into RAM, then write it. Loading first is what keeps the
// dangerous window short: reading a file involves the filesystem, which
// involves flash, which cannot be touched once the erase has started.
bool update_apply_file(const char *path) {
    bool is_dir = false; uint32_t size = 0;
    if (!storage_stat(path, &is_dir, &size) || size == 0) {
        out_err("No image at %s.", path);
        return false;
    }
    if (size > storage_reserve_bytes()) {
        out_err("Image is larger than the firmware region.");
        return false;
    }

    uint8_t *img = (uint8_t *)malloc(size);
    if (!img) {
        out_err("Not enough memory to stage a %lu KB image.", (unsigned long)(size / 1024));
        out_multi("  Reboot and try again before opening anything else.");
        return false;
    }
    uint32_t n = storage_read_file(path, img, size);
    if (n != size) { free(img); out_err("Could not read the image back."); return false; }

    log_addf(LOG_K_WARN, "update: writing %lu bytes of firmware", (unsigned long)size);
    out_info("Writing firmware...");
    // Everything below is unrecoverable if interrupted, so nothing below can
    // fail: no allocation, no filesystem, no formatting.
    sleep_ms(50);                      // let the serial buffer drain first

    apply_image(img, size);

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
        return update_apply_file(argv[2]) ? 0 : 1;
    }

    out_multi("Usage:");
    out_multi("  update check              is there a newer release");
    out_multi("  update install            fetch, verify and apply it");
    out_multi("  update install --force    reinstall the current version");
    out_multi("  update from-file <path>   apply an image already on the device");
    return 1;
}

void update_register(void) {
    static const Command c{"update", "check for and apply a firmware update", cmd_update,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
