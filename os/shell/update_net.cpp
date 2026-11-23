// update check / update install — the half of `update` that needs a network.
//
// Split out of update.cpp. In every image: over Wi-Fi where the image has it, and
// otherwise through the bridge, when rpc or the browser tool on the console
// offers it (httproute.h, LINK.md). Writing an image over the firmware,
// `update from-file`, `update rollback` and the boot-time report all stay in
// update.cpp, because the recovery ladder needs them whatever the image has in
// it. The order this follows, and why, is at the top of update.cpp.

#include "command.h"
#include "out.h"
#include "httpfetch.h"
#include "repoindex.h"
#include "storage.h"
#include "sha256.h"
#include "interrupt.h"
#include "kernel.h"
#include "registry.h"
#include "users.h"
#include "session.h"
#include "rollback.h"
#include "imagefeatures.h"
#include "update.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "httproute.h"

#define MANIFEST_URL  "https://raw.githubusercontent.com/dash1101/RPCortex/main/releases/latest.json"
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
    HttpVia via = http_route(&t);
    if (!via) { http_no_route(); return false; }
    if (!http_https_ready(url, via)) return false;

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
        http_report_failure(&r);
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

// --- the command ------------------------------------------------------------

int update_check(bool quiet) {
    RepoEntry e;
    if (!fetch_manifest(&e)) return 1;

    // Compared against version-plus-build, not the version alone. During a beta
    // the version is frozen, so comparing only that would report "up to date"
    // for every build ever published.
    int cmp = repo_version_cmp(e.ver, RPC_OS_BUILDVER);
    out_multi("  Installed  %s", RPC_OS_VERSION);
    out_multi("  Available  %s", e.ver);
    if (e.size) out_multi("  Size       %lu KB", (unsigned long)(e.size / 1024));

    if (cmp <= 0) {
        if (!quiet) out_ok("Already up to date.");
        return 0;
    }

    if (!strcmp(reg_get("System.Rollback_Refused", ""), e.ver)) {
        out_warn("This device already rolled back from %s.", e.ver);
        out_multi("  It was installed and would not start, so the previous version");
        out_multi("  was put back. 'sudo update install --force' installs it anyway.");
        return 0;
    }

    out_info("An update is available. 'sudo update install' to fetch and apply it.");
    return 0;
}

int update_online(bool force, bool yes) {
    if (!users_is_admin(session_user())) {
        out_err("Only an admin can update the firmware.");
        return 1;
    }

    RepoEntry e;
    if (!fetch_manifest(&e)) return 1;

    if (!force && repo_version_cmp(e.ver, RPC_OS_BUILDVER) <= 0) {
        out_ok("Already running %s. 'sudo update install --force' to reinstall.", RPC_OS_VERSION);
        return 0;
    }
    if (!force && !strcmp(reg_get("System.Rollback_Refused", ""), e.ver)) {
        out_err("%s was installed on this device before and would not start.", e.ver);
        out_multi("  The previous version was restored automatically. Installing it");
        out_multi("  again is unlikely to end differently unless something has changed.");
        out_multi("  'sudo update install --force' does it anyway.");
        return 1;
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

    // Before the download rather than part way through it. The filesystem stops
    // accepting writes at 98% full, and a transfer that reaches that point fails
    // at the last few kilobytes with no hint of why.
    //
    // The copy of the previous firmware saved by the last update is an image-sized
    // file, and on a 1.8 MB filesystem it leaves no room for the next image. The
    // one this update saves replaces it anyway, but the download arrives first, so
    // without this the second update over the air fails at the door.
    //
    // So when the download will not fit, the saved copy goes and the room is
    // measured again — not predicted from sizes first, because the sizes are not
    // always the whole story: a Pico 2 W with that copy on it was seen to report
    // itself 100% full (0 KB free, every write refused) with 1.3 MB of files on a
    // 1.8 MB filesystem, and removing the copy gave back 1164 KB at once. No sum of
    // file sizes predicts that. The RUNNING firmware is saved again before anything
    // is written, and that is the copy this update needs.
    if (e.size && !storage_would_fit((uint32_t)e.size)) {
        bool is_dir = false; uint32_t saved = 0;
        if (storage_stat(ROLLBACK_IMG, &is_dir, &saved) && !is_dir && saved) {
            out_warn("Removing the saved copy of the previous firmware (%lu KB) to make room.",
                     (unsigned long)(saved / 1024));
            out_multi("  %sThe running firmware is saved again before anything is written.%s",
                      C_GRAY, C_RESET);
            rollback_forget();
        }
        if (!storage_would_fit((uint32_t)e.size)) {
            out_err("Not enough room for the %lu KB download: %lu KB free, %u%% used.",
                    (unsigned long)(e.size / 1024),
                    (unsigned long)(storage_free_bytes() / 1024),
                    (unsigned)storage_used_percent());
            out_multi("  The filesystem stops accepting writes at 98%% full. 'df' shows what is");
            out_multi("  using it, and 'rm' clears space.");
            return 1;
        }
    }

    // A custom image is replaced by the standard one: a release publishes one
    // image per board, with that board's standard features. Said before the
    // download rather than discovered after the reboot.
    if (!features_standard()) {
        out_warn("This is a custom image (%s). The update is the standard %s image,",
                 features_variant(), MY_BOARD);
        out_multi("  so what this image was built without comes back.");
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
    // --yes is for scripts, as in v1. The admin check above still applies.
    if (!yes && !session_confirm("  Continue?")) { out_info("Cancelled. The download is kept."); return 0; }

    return update_apply_file(IMAGE_PATH, e.ver, /*at_boot*/false) ? 0 : 1;
}
