// download — the only way the drive exists.
//
// A transfer is a MODE with a beginning and an end, not a thing running behind
// you. The drive appears when the command starts, the shell waits while the
// host does its work, and the drive and /usb are made to match before it goes
// away again.
//
// /usb IS THE DRIVE. Everything in it goes out when the mode opens, so the
// folder is what the host sees; whatever the host leaves comes back; and a file
// the host deleted is removed. The drive used to open empty, which meant the
// only way to get something OFF the device was to name it on the command line
// — backwards, for a folder whose whole purpose is transfer.
//
// Deletions are only honoured when the entire folder made it out. If the drive
// filled up part way, a file missing from it means "never arrived" rather than
// "the host removed it", and treating those the same would delete something
// nobody touched.
//
// That shape is the security model as well as the interface. There is no
// setting to leave switched on and nothing persisted, because "is the
// filesystem exposed right now" has one answer and it is on the screen. A
// device plugged into someone else's machine offers a serial console and
// nothing else, since there is nobody at the prompt to have asked for more.
//
// It is also why the area is formatted on the way IN. A session that ended in a
// reset or a pulled cable cannot leave yesterday's files on a drive somebody
// else plugs in, and every session starts from the same state.

#include "command.h"
#include "fat12.h"
#include "fmt.h"
#include "interrupt.h"
#include "out.h"
#include "path.h"
#include "storage.h"
#include "task.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#if CFG_TUD_MSC

bool     usbmsc_open(void);
void     usbmsc_close(void);
bool     usbmsc_full(void);
uint32_t usbmsc_import_all(bool *refused);
bool     usbmsc_settled(void);
bool     usbdrv_list(F12WalkFn cb, void *ctx);
bool     usbdrv_find(const char *name, F12Entry *out);
bool     usbdrv_export(const char *path, const char *name);
void     usbdrv_space(uint32_t *total, uint32_t *freebytes);

const char *fs_cwd(void);

struct Counter { uint32_t n; };
static void count_cb(void *ctx, const F12Entry *e) {
    if (!e->is_dir) ((Counter *)ctx)->n++;
}

static const char *basename_of(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

// --- /usb IS the drive -------------------------------------------------------
//
// The drive used to open EMPTY, and the only way to get something off the
// device was to name it on the command line. That is backwards: /usb exists to
// be the transfer folder, so download mode should show it.
//
// So everything in /usb goes out on the way in, and what the host leaves comes
// back on the way out. Which makes the round trip a folder rather than a
// one-way dump, and makes "put it in /usb" the whole instruction.
#define SYNC_MAX 64
#define USB_DIR  "/usb"

static char     g_sent[SYNC_MAX][F12_MAXNAME];
static uint32_t g_sent_n;
// Only true when EVERY file in /usb reached the drive. A deletion on the host
// is only meaningful against a complete picture: if the drive filled up half
// way, a file missing from it means "never arrived", not "the host removed it",
// and treating the two the same would delete things nobody touched.
static bool     g_sent_all;

// Names are collected before anything is copied. storage_walk holds the
// filesystem lock across its callback (#82) and usbdrv_export reads through the
// filesystem, so exporting from inside the walk is the deadlock that note is
// about.
struct NameList { char names[SYNC_MAX][F12_MAXNAME]; uint32_t n; bool overflow; };

static void name_cb(void *ctx, const char *name, bool is_dir, uint32_t) {
    NameList *l = (NameList *)ctx;
    if (is_dir) return;                    // FAT12 root here is flat; see fat12.h
    if (l->n >= SYNC_MAX) { l->overflow = true; return; }
    snprintf(l->names[l->n], F12_MAXNAME, "%s", name);
    l->n++;
}

// Put everything in /usb onto the drive. Returns how many went.
static uint32_t offer_usb_folder(void) {
    g_sent_n = 0;
    g_sent_all = false;

    bool is_dir = false;
    if (!storage_stat(USB_DIR, &is_dir, nullptr)) {
        // Nothing has ever been transferred. Make it, so the instruction on
        // screen refers to somewhere that exists.
        storage_mkdir(USB_DIR);
        g_sent_all = true;
        return 0;
    }
    if (!is_dir) return 0;

    static NameList list;
    list.n = 0;
    list.overflow = false;
    if (!storage_walk(USB_DIR, name_cb, &list)) return 0;

    uint32_t sent = 0;
    bool all = !list.overflow;
    for (uint32_t i = 0; i < list.n; i++) {
        char src[192];
        snprintf(src, sizeof(src), "%s/%s", USB_DIR, list.names[i]);

        uint32_t size = 0;
        if (!storage_stat(src, nullptr, &size)) { all = false; continue; }

        uint32_t total = 0, freeb = 0;
        usbdrv_space(&total, &freeb);
        if (size > freeb) {
            char a[12], b[12];
            fmt_size(size, a, sizeof(a));
            fmt_size(freeb, b, sizeof(b));
            out_warn("%s stayed behind -- needs %s, %s free on the drive.",
                     list.names[i], a, b);
            all = false;
            continue;
        }
        if (!usbdrv_export(src, list.names[i])) { all = false; continue; }

        if (g_sent_n < SYNC_MAX)
            snprintf(g_sent[g_sent_n++], F12_MAXNAME, "%s", list.names[i]);
        sent++;
        if (intr_check()) { all = false; break; }
    }
    if (list.overflow)
        out_warn("/usb holds more than %d files; only the first %d are on the "
                 "drive.", SYNC_MAX, SYNC_MAX);
    g_sent_all = all;
    return sent;
}

// A file that went out and did not come back was deleted on the host. Only
// meaningful when the whole folder made it out — see g_sent_all.
static uint32_t reap_deleted(void) {
    if (!g_sent_all) return 0;
    uint32_t gone = 0;
    for (uint32_t i = 0; i < g_sent_n; i++) {
        F12Entry e;
        if (usbdrv_find(g_sent[i], &e)) continue;      // still there
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", USB_DIR, g_sent[i]);
        if (storage_remove(path)) gone++;
    }
    return gone;
}

static int cmd_download(int argc, char **argv) {
    if (!usbmsc_open()) {
        out_err("The transfer area could not be prepared.");
        return 1;
    }

    // /usb goes out first, so the drive opens showing what is already there.
    uint32_t offered = offer_usb_folder();

    // Files named on the command line go out too, from anywhere on the device.
    // The same mode serves both directions, which is why there is no second
    // command for it.
    for (int i = 1; i < argc; i++) {
        char src[128];
        path_resolve(fs_cwd(), argv[i], src, sizeof(src));
        bool is_dir = false;
        uint32_t size = 0;
        if (!storage_stat(src, &is_dir, &size) || is_dir) {
            out_warn("Skipped %s -- no such file.", argv[i]);
            continue;
        }
        uint32_t total = 0, freeb = 0;
        usbdrv_space(&total, &freeb);
        if (size > freeb) {
            char a[12], b[12];
            fmt_size(size, a, sizeof(a));
            fmt_size(freeb, b, sizeof(b));
            out_warn("Skipped %s -- needs %s, %s free.", basename_of(src), a, b);
            continue;
        }
        if (usbdrv_export(src, basename_of(src))) offered++;
        else out_warn("Skipped %s -- could not copy it.", basename_of(src));
    }

    out_blank();
    out_multi("  %s%sDOWNLOAD MODE%s", C_BOLD, C_CYAN, C_RESET);
    out_multi("  A drive named RPCORTEX is on this computer.");
    if (offered)
        out_multi("  It holds %u file%s from /usb. Copy them off, add more, or "
                  "delete some.", (unsigned)offered, offered == 1 ? "" : "s");
    else
        out_multi("  /usb is empty. Drop files onto the drive to bring them in.");
    out_multi("  %sPress any key when you are done. The drive and /usb are made "
              "to match.%s", C_GRAY, C_RESET);
    out_blank();

    // Nothing is copied while the mode runs. Making a document in Explorer is
    // three operations -- create, rename, edit -- and a scan between them takes
    // the file twice, once under a name nobody chose. Whatever the drive holds
    // at the end is what gets taken, however many times the host changed its
    // mind getting there.
    // YIELD, do not wait.
    //
    // This loop used getchar_timeout_us(20000) ten times a frame, and that call
    // busy-waits: it does not give the core back. The usb task is pinned to the
    // same core, so it could not run for two hundred milliseconds at a stretch
    // — USB serviced five times a second, one 512-byte sector per turn, about
    // two and a half kilobytes a second. A 443 KB file would have taken three
    // minutes, and a shell task that does not yield for that long is also what
    // the stall watchdog exists to reboot.
    //
    // So the key is polled with a zero timeout and the core is handed straight
    // back. Yielding in a tight loop burns cycles doing nothing, which is
    // exactly right here: this is a transfer mode, and the only thing worth
    // spending the core on is the transfer.
    static const char *kSpin = "|/-\\";
    uint32_t frame = 0, on_drive = offered;
    bool quit = false, counted = false;
    uint32_t last_draw = 0;

    while (!quit) {
        if (usbmsc_settled() || !counted) {
            Counter c{0};
            usbdrv_list(count_cb, &c);
            on_drive = c.n;
            counted = true;
        }

        // The spinner on a clock rather than on a loop count, since the loop
        // now turns thousands of times a second.
        uint32_t now = task_now_ms();
        if (now - last_draw >= 150) {
            last_draw = now;
            printf("\r  %c  drive open   %u file%s on it   ",
                   kSpin[frame++ & 3], (unsigned)on_drive, on_drive == 1 ? "" : "s");
            fflush(stdout);
        }

        if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) quit = true;
        if (intr_check()) quit = true;
        task_yield();
    }

    // Let a copy in flight finish. A key pressed mid-transfer would otherwise
    // read half a file. Yielding, for the same reason as above: the transfer
    // being waited for needs the core to make progress.
    printf("\r  ...  finishing                          ");
    fflush(stdout);
    {
        uint32_t until = task_now_ms() + 2000;
        while (task_now_ms() < until) {
            if (usbmsc_settled()) break;
            task_yield();
        }
    }

    bool refused = false;
    uint32_t taken = usbmsc_import_all(&refused);
    // Deletions AFTER the import, so a file the host both removed and replaced
    // ends up present rather than gone.
    uint32_t removed = reap_deleted();

    out_progress_done();
    printf("\r                                          \r");
    usbmsc_close();

    out_blank();
    if (taken) out_ok("%u file%s copied into /usb", (unsigned)taken, taken == 1 ? "" : "s");
    else if (on_drive) out_multi("  %s/usb already had everything on the drive.%s",
                                 C_GRAY, C_RESET);
    else out_multi("  %sNothing arrived.%s", C_GRAY, C_RESET);
    if (removed)
        out_ok("%u file%s removed from /usb, deleted on the drive",
               (unsigned)removed, removed == 1 ? "" : "s");
    else if (!g_sent_all && g_sent_n)
        out_multi("  %sNot everything fitted on the drive, so nothing was removed "
                  "from /usb.%s", C_GRAY, C_RESET);
    if (refused)
        out_warn("Something did not fit. Free space with 'rm' and run download again.");
    out_multi("  %sThe drive is gone until the next 'download'.%s", C_GRAY, C_RESET);
    return 0;
}

#else
static int cmd_download(int argc, char **argv) {
    (void)argc; (void)argv;
    out_err("This build has no USB drive support.");
    return 1;
}
#endif

void usbdrive_register(void) {
    static const Command c{"download",
        "download [file...]  -- open the USB drive; /usb goes out, what lands comes in",
        cmd_download, nullptr};
    cmd_register(&c);
}
