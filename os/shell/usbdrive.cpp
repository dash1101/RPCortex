// download — the only way the drive exists.
//
// A transfer is a MODE with a beginning and an end, not a thing running behind
// you. The drive appears when the command starts, the shell waits while the
// host does its work, and whatever landed is copied into /usb before the drive
// goes away again.
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

static int cmd_download(int argc, char **argv) {
    if (!usbmsc_open()) {
        out_err("The transfer area could not be prepared.");
        return 1;
    }

    // Files named on the command line go OUT: put on the drive so they can be
    // dragged off. The same mode serves both directions, which is why there is
    // no second command for it.
    uint32_t offered = 0;
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
    if (offered)
        out_multi("  %u file%s ready to copy off the RPCORTEX drive.",
                  (unsigned)offered, offered == 1 ? " is" : "s are");
    out_multi("  A drive named RPCORTEX is on this computer. Drop files onto it.");
    out_multi("  %sPress any key when you are done, and they will be copied into /usb.%s",
              C_GRAY, C_RESET);
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

    out_progress_done();
    printf("\r                                          \r");
    usbmsc_close();

    out_blank();
    if (taken) out_ok("%u file%s copied into /usb", (unsigned)taken, taken == 1 ? "" : "s");
    else if (on_drive) out_multi("  %s/usb already had everything on the drive.%s",
                                 C_GRAY, C_RESET);
    else out_multi("  %sNothing arrived.%s", C_GRAY, C_RESET);
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
        "download [file...]  -- open the USB drive; files named go out, what lands comes in",
        cmd_download, nullptr};
    cmd_register(&c);
}
