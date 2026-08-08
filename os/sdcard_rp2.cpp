// The card on an RP2350: the SPI transport, the mount, and where "/sd" goes.
//
// DEVICE-UNCONFIRMED, ALL OF IT.
//
// Nothing in this file has been run with a card attached. The protocol above it
// is host-tested against a fake card that answers as the specification says
// (sdproto_test) and the filesystem above that against volumes fsck.fat has
// approved of (fatro_test), so what is unproven here is specifically the
// electrical and timing layer: whether the bus is wired the way the profile
// says, whether a real card tolerates this clock, and whether the pull-ups are
// where they need to be.
//
// WHAT TO TRY FIRST, in this order, because each one rules out the layer below:
//
//   1. `sd status` with NO card in the slot. It must say "no card" within about
//      a tenth of a second. A long pause means the CMD0 probe window is being
//      spent on a bus that is not answering the way "absent" looks, and every
//      later test will be slow and confusing.
//   2. `sd mount` with a card in. If it fails it names the step: "no card
//      answered" is wiring or the 74-clock preamble; "garbled CMD8" is the bus
//      itself — clock too fast, no MISO pull-up, or MOSI and MISO swapped;
//      "never finished initialising" is usually power.
//   3. `sd info`. Check the CAPACITY against what is printed on the card. If it
//      is out by a factor of 512 or nonsense, the CSD version branch is wrong.
//      Check "addressing": an SDHC card must say BLOCK.
//   4. `ls /sd`. Then `sd info` again — if `crc errors` is climbing, drop the
//      clock (RUN_HZ in sdproto.cpp) before believing anything else.
//   5. Pull the card out during `tree /sd`. It must give an error and hand the
//      prompt back, not hang.
//
// THE BUS IS SHARED. On the reference Nova D1 profile SPI0 carries the CC1101,
// the SX1276 and the card, on separate chip selects. This driver re-applies the
// baud rate and the frame format every time it asserts CS, because a package
// that used the bus in between will have left it set up for its own device.
// What it does NOT do is arbitrate: a package mid-transaction with a radio
// while a read happens here is a collision, and the fix for that is a bus lock
// in the SPI layer rather than something this file can do on its own.
#include "sdcard.h"

#if defined(RPC_HAS_SD) && RPC_HAS_SD

#include "sdproto.h"
#include "fatro.h"
#include "storage.h"
#include "registry.h"
#include "lock.h"
#include "task.h"

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// --- pins --------------------------------------------------------------------
//
// The same keys the Nova D1 package writes, and the same defaults its Pico 2 W
// profile carries — SPI0 on 18/19/16 with the card selected by GPIO 9. The
// registry wins, so a board somebody has already wired and configured is not
// re-pinned by a firmware update that ships a different idea.
//
// sd_cd is NOT in any profile. A card-detect switch is a nice thing to have and
// the reference board does not wire one, so it is honoured when it is
// configured and the driver probes when it is not.
#define KEY_CS   "Apps.NovaD1_PIN_sd_cs"
#define KEY_SCK  "Apps.NovaD1_PIN_spi_sck"
#define KEY_MOSI "Apps.NovaD1_PIN_spi_mosi"
#define KEY_MISO "Apps.NovaD1_PIN_spi_miso"
#define KEY_CD   "Apps.NovaD1_PIN_sd_cd"

#define DEF_CS   9
#define DEF_SCK  18
#define DEF_MOSI 19
#define DEF_MISO 16

struct Pins { int cs, sck, mosi, miso, cd; };

static Pins pins_now(void) {
    Pins p;
    p.cs   = reg_get_int(KEY_CS,   DEF_CS);
    p.sck  = reg_get_int(KEY_SCK,  DEF_SCK);
    p.mosi = reg_get_int(KEY_MOSI, DEF_MOSI);
    p.miso = reg_get_int(KEY_MISO, DEF_MISO);
    p.cd   = reg_get_int(KEY_CD,   -1);
    return p;
}

// RP2 has no GPIO matrix: which SPI controller a pin belongs to is fixed by the
// pin, and it is (gpio / 8) % 2. Deriving it from SCK rather than storing it
// removes a way for the two to disagree.
static spi_inst_t *spi_for(int sck) { return (((sck / 8) % 2) == 0) ? spi0 : spi1; }

// --- the transport -----------------------------------------------------------

struct Bus {
    spi_inst_t *spi;
    Pins        p;
    uint32_t    hz;
    bool        up;
};
static Bus g_bus;

static void bus_format(void) {
    spi_set_baudrate(g_bus.spi, g_bus.hz);
    // Mode 0, 8 bits, MSB first — the only mode an SD card speaks.
    spi_set_format(g_bus.spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

static void tr_xfer(void *, const uint8_t *tx, uint8_t *rx, uint32_t len) {
    if (tx && rx)      spi_write_read_blocking(g_bus.spi, tx, rx, len);
    else if (tx)       spi_write_blocking(g_bus.spi, tx, len);
    else if (rx)       spi_read_blocking(g_bus.spi, 0xFF, rx, len);
}

static void tr_cs(void *, bool select) {
    if (select) bus_format();     // the bus is shared; take it back every time
    gpio_put((uint)g_bus.p.cs, select ? 0 : 1);
}

static void tr_baud(void *, uint32_t hz) {
    g_bus.hz = hz;
    spi_set_baudrate(g_bus.spi, hz);
}

static void tr_yield(void *, uint32_t ms) { task_sleep_ms(ms ? ms : 1); }
static uint32_t tr_now(void *) { return task_now_ms(); }

static bool bus_up(void) {
    Pins p = pins_now();
    if (p.cs < 0 || p.sck < 0 || p.mosi < 0 || p.miso < 0) return false;
    if (g_bus.up && p.cs == g_bus.p.cs && p.sck == g_bus.p.sck &&
        p.mosi == g_bus.p.mosi && p.miso == g_bus.p.miso) return true;

    g_bus.p = p;
    g_bus.spi = spi_for(p.sck);
    g_bus.hz = 400000;
    spi_init(g_bus.spi, g_bus.hz);
    gpio_set_function((uint)p.sck,  GPIO_FUNC_SPI);
    gpio_set_function((uint)p.mosi, GPIO_FUNC_SPI);
    gpio_set_function((uint)p.miso, GPIO_FUNC_SPI);
    // A card leaves MISO floating until it is selected, and a floating input
    // reads as whatever the last edge left behind — which is indistinguishable
    // from a card answering rubbish.
    gpio_pull_up((uint)p.miso);

    // CS is a plain GPIO, deliberately. The SPI block's own chip select drops
    // between bytes, and an SD command is a multi-byte transaction that must be
    // held selected throughout.
    gpio_init((uint)p.cs);
    gpio_set_dir((uint)p.cs, GPIO_OUT);
    gpio_put((uint)p.cs, 1);

    if (p.cd >= 0) {
        gpio_init((uint)p.cd);
        gpio_set_dir((uint)p.cd, GPIO_IN);
        gpio_pull_up((uint)p.cd);
    }
    g_bus.up = true;
    return true;
}

// A card-detect switch closes to ground when a card is seated, so with the
// pull-up above a card reads LOW. Without one configured, say "maybe" and let
// the probe decide.
static bool cd_says_present(void) {
    if (!g_bus.up || g_bus.p.cd < 0) return true;
    return gpio_get((uint)g_bus.p.cd) == 0;
}

// --- the mount ---------------------------------------------------------------

static RpcLock  g_sd_lock;
static SdCard   g_card;
static FatRo    g_fs;
static bool     g_mounted;
static bool     g_manual;            // unmounted on purpose
static uint32_t g_last_poll;
static uint32_t g_removed_at;
static bool     g_was_mounted;       // for the "recently removed" window
static uint32_t g_blocks_read;
// Bumped by every mount and every unmount. Anything cached about the volume —
// a resolved directory entry, a cluster cursor — is only valid for the
// generation it was taken in, so a different card in the same slot cannot be
// read through the last card's directory.
static uint32_t g_mount_gen = 1;
static uint8_t  g_last_error = SD_ERR_NO_CARD;
// The card talks and there is no filesystem on it this can read. Its own flag
// rather than an SD error code, because it is not an SD failure — the block
// layer worked perfectly — and telling somebody their card is broken when it is
// merely exFAT sends them to replace hardware that is fine.
static bool     g_no_filesystem;

// How often autodetect is allowed to actually touch the card. A probe is one
// command; doing it on every directory entry would be silly, and doing it never
// means a card inserted while the browser is open is never noticed.
#define POLL_MS   1500
// How long a removed card keeps being offered with present=0, so a browser can
// say "card removed" instead of the row vanishing under the cursor. The
// contract in rpc_app.h asks for exactly this.
#define STICKY_MS 8000

static bool fs_read(void *, uint32_t lba, void *buf) {
    if (!g_mounted && !g_card.ready) return false;
    if (!sd_read_block(&g_card, lba, buf)) return false;
    g_blocks_read++;
    return true;
}

// Everything below assumes the lock is held.

static void unmount_locked(bool manual) {
    if (g_mounted) {
        g_was_mounted = true;
        g_removed_at = task_now_ms();
    }
    g_mounted = false;
    g_mount_gen++;
    fatro_unmount(&g_fs);
    sd_deinit(&g_card);
    g_manual = manual;
    if (manual) g_was_mounted = false;    // no "removed" row for a deliberate one
}

static bool mount_locked(void) {
    if (g_mounted) return true;
    g_no_filesystem = false;
    if (!bus_up()) { g_last_error = SD_ERR_NO_CARD; return false; }
    if (!cd_says_present()) { g_last_error = SD_ERR_NO_CARD; return false; }

    SdXfer x{};
    x.xfer = tr_xfer; x.cs = tr_cs; x.baud = tr_baud;
    x.yield_ms = tr_yield; x.now_ms = tr_now;
    if (!sd_init(&g_card, &x)) { g_last_error = g_card.last_error; return false; }

    FatRoIo io{nullptr, fs_read};
    if (!fatro_mount(&g_fs, &io)) {
        // A card over 32 GB out of a packet is exFAT, which is a real and
        // ordinary state and not a broken card. Said in those words rather than
        // as an SD error.
        sd_deinit(&g_card);
        g_last_error = SD_OK;
        g_no_filesystem = true;
        return false;
    }
    g_mounted = true;
    g_mount_gen++;
    g_manual = false;
    g_was_mounted = false;
    g_last_error = SD_OK;
    return true;
}

static void poll_locked(bool force) {
    uint32_t now = task_now_ms();
    if (!force && g_last_poll && (now - g_last_poll) < POLL_MS) return;
    g_last_poll = now;

    if (g_mounted) {
        // One command, no data transfer. A card that has been pulled out stops
        // answering this, which is how removal is noticed at all without a
        // card-detect pin.
        if (!cd_says_present() || !sd_alive(&g_card)) unmount_locked(false);
        return;
    }
    if (g_manual) return;            // somebody asked for it to be off
    mount_locked();
}

// --- paths -------------------------------------------------------------------

bool sd_owns_path(const char *path) {
    if (!path) return false;
    if (path[0] != '/' || path[1] != 's' || path[2] != 'd') return false;
    return path[3] == 0 || path[3] == '/';
}

// The part of the path inside the volume. "/sd" becomes "/".
static const char *inside(const char *path) {
    const char *p = path + 3;
    return *p ? p : "/";
}

// Ready the card for an operation. Returns false when there is nothing to work
// with, which every caller turns into an ordinary "cannot read that" rather
// than an error about hardware.
static bool ready_locked(void) {
    poll_locked(false);
    return g_mounted;
}

bool sd_walk(const char *path, SdWalkFn cb, void *ctx) {
    LockGuard _sd(&g_sd_lock);
    if (!ready_locked()) return false;
    struct Ctx { SdWalkFn cb; void *ctx; } c{cb, ctx};
    bool ok = fatro_list(&g_fs, inside(path), [](void *v, const FatRoEntry *e) {
        Ctx *c = (Ctx *)v;
        c->cb(c->ctx, e->name, e->is_dir, e->size);
    }, &c);
    // A walk can fail because the card went away mid-listing, and dropping the
    // mount then is right — the poll would find it eventually and eventually is
    // not good enough while a browser is redrawing.
    //
    // But it ALSO fails for a path that is not there and for a path that is a
    // file, and unmounting for those would be a disaster: one tab-completion on
    // a name that does not exist would take the card away, the poll's rate
    // limit would keep it away, and fw_storage_roots would spend eight seconds
    // telling a browser the card had been removed while it sat in the slot.
    // So the card is ASKED before it is written off.
    if (!ok && !sd_alive(&g_card)) unmount_locked(false);
    return ok;
}

bool sd_stat(const char *path, bool *is_dir, uint32_t *size) {
    LockGuard _sd(&g_sd_lock);
    if (!ready_locked()) return false;
    FatRoEntry e;
    if (!fatro_stat(&g_fs, inside(path), &e)) return false;
    if (is_dir) *is_dir = e.is_dir;
    if (size)   *size = e.size;
    return true;
}

// The last file read from, kept between calls.
//
// TWO things are cached, and leaving either out is quadratic.
//
// The CURSOR saves walking the cluster chain from the start on every call — the
// bug fat12.h documents at length, where a 955 KB file cost 3.6 million sector
// reads and presented as the device hanging.
//
// The ENTRY saves re-resolving the PATH. A read arrives as a path, and resolving
// "/sd/DCIM/SUB/FILE.WAV" means scanning three directories; a caller reading a
// file 256 bytes at a time would pay that on every single call, over SPI. This
// is the same shape of mistake one level up, and it is the one that would bite
// the media player first.
//
// Both are invalidated by the mount generation, so a different card in the same
// slot cannot be read through the previous card's directory entry.
static char        g_cur_path[64];
static FatRoEntry  g_cur_entry;
static FatRoCursor g_cur;
static uint32_t    g_cur_gen;

uint32_t sd_read_at(const char *path, uint32_t off, uint8_t *buf, uint32_t cap) {
    LockGuard _sd(&g_sd_lock);
    if (!ready_locked()) return 0;

    bool same = g_cur_gen == g_mount_gen && g_cur_path[0] &&
                strncmp(g_cur_path, path, sizeof(g_cur_path)) == 0;
    if (!same) {
        FatRoEntry e;
        if (!fatro_stat(&g_fs, inside(path), &e) || e.is_dir) return 0;
        // A path too long to hold is read without the cache rather than with a
        // truncated key that could match a different file.
        if (strlen(path) >= sizeof(g_cur_path)) {
            FatRoCursor tmp; fatro_cursor_init(&tmp);
            return fatro_read(&g_fs, &e, &tmp, off, buf, cap);
        }
        snprintf(g_cur_path, sizeof(g_cur_path), "%s", path);
        g_cur_entry = e;
        g_cur_gen = g_mount_gen;
        fatro_cursor_init(&g_cur);
    }
    return fatro_read(&g_fs, &g_cur_entry, &g_cur, off, buf, cap);
}

uint32_t sd_mtime(const char *path) {
    LockGuard _sd(&g_sd_lock);
    if (!ready_locked()) return 0;
    FatRoEntry e;
    if (!fatro_stat(&g_fs, inside(path), &e)) return 0;
    return e.mtime;
}

bool sd_copy_out(const char *from, const char *to) {
    // The card lock is taken inside sd_read_at, not held across the whole copy:
    // a large file would otherwise block every other task's access to the card
    // for the duration, and littlefs has its own lock to take on the other side.
    bool is_dir = false;
    uint32_t size = 0;
    if (!sd_stat(from, &is_dir, &size) || is_dir) return false;

    void *sink = storage_open_sink(to);
    if (!sink) return false;
    uint8_t buf[256];
    uint32_t off = 0;
    bool ok = true;
    while (off < size) {
        uint32_t n = sd_read_at(from, off, buf, sizeof(buf));
        if (!n) { ok = false; break; }
        if (!storage_sink_write(sink, buf, n)) { ok = false; break; }
        off += n;
        task_alive();
    }
    if (!storage_close_sink(sink)) ok = false;
    if (!ok) storage_remove(to);
    return ok;
}

// --- lifecycle, from outside -------------------------------------------------

bool sd_mount(void) {
    LockGuard _sd(&g_sd_lock);
    g_manual = false;
    g_last_poll = task_now_ms();
    return mount_locked();
}

void sd_unmount(bool manual) {
    LockGuard _sd(&g_sd_lock);
    unmount_locked(manual);
}

bool sd_present(void) {
    LockGuard _sd(&g_sd_lock);
    return ready_locked();
}

void sd_poll(void) {
    LockGuard _sd(&g_sd_lock);
    poll_locked(false);
}

void sd_info(SdInfo *out) {
    LockGuard _sd(&g_sd_lock);
    memset(out, 0, sizeof(*out));
    poll_locked(false);

    Pins p = g_bus.up ? g_bus.p : pins_now();
    out->pin_cs = p.cs; out->pin_sck = p.sck; out->pin_mosi = p.mosi;
    out->pin_miso = p.miso; out->pin_cd = p.cd;
    out->spi_bus = (p.sck >= 0) ? ((p.sck / 8) % 2) : -1;

    out->manual = g_manual;
    out->last_error = g_no_filesystem ? "no FAT filesystem this can read (exFAT?)"
                                      : sd_error_text(g_last_error);
    out->recently_removed =
        !g_mounted && g_was_mounted && (task_now_ms() - g_removed_at) < STICKY_MS;
    out->mounted = g_mounted;
    if (!g_mounted) return;

    out->card_type   = sd_type_name(&g_card);
    out->fs_type     = fatro_type_name(&g_fs);
    out->card_bytes  = sd_capacity_bytes(&g_card);
    out->volume_bytes = fatro_total_bytes(&g_fs);
    out->free_bytes  = fatro_free_bytes(&g_fs);
    out->crc_errors  = g_card.crc_errors;
    out->retries     = g_card.retries;
    out->blocks_read = g_blocks_read;
    out->part_lba    = g_fs.part_lba;
    snprintf(out->label, sizeof(out->label), "%s", g_fs.label);
}

#endif  // RPC_HAS_SD
