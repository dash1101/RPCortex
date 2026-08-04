// usb — the transfer area, from the device's side.
//
// The drive is a real FAT12 volume in its own flash region, not a view of the
// filesystem. So there is nothing to keep in step: the host writes it, this
// reads the same bytes, and a file is either there or it is not. Getting
// something onto the device is a drag from the host and then `usb get`; getting
// something off is `usb put` and then a drag the other way.
//
// It is also the widest thing this device exposes, which is why it can be
// turned off and why that setting is remembered.

#include "command.h"
#include "fat12.h"
#include "fmt.h"
#include "out.h"
#include "path.h"
#include "storage.h"

#include "interrupt.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#if CFG_TUD_MSC
bool usbmsc_enabled(void);
void usbmsc_set_mode(int mode);
int  usbmsc_mode(void);
bool usbmsc_full(void);
bool usbdrv_ready(void);
bool usbdrv_list(F12WalkFn cb, void *ctx);
bool usbdrv_find(const char *name, F12Entry *out);
bool usbdrv_remove(const char *name);
bool usbdrv_format(void);
void usbdrv_space(uint32_t *total, uint32_t *freebytes);
bool usbdrv_import(const char *name, const char *dest);
bool usbdrv_export(const char *path, const char *name);

const char *fs_cwd(void);

// Collected under the drive's lock and printed afterwards.
//
// Not a stylistic choice: printing takes the console's mutex, the USB task
// holds that mutex while it services the device stack, and the drive's lock is
// taken on both sides of it. Printing while holding it is the deadlock this
// design was rebuilt to avoid, so the listing is gathered first and rendered
// second.
#define LIST_MAX 48
struct Listing {
    F12Entry e[LIST_MAX];
    uint32_t n;
    uint32_t skipped;
};

static void collect(void *ctx, const F12Entry *e) {
    Listing *l = (Listing *)ctx;
    if (l->n < LIST_MAX) l->e[l->n++] = *e;
    else l->skipped++;
}

static int cmd_usb_ls(void) {
    Listing l;
    l.n = 0; l.skipped = 0;
    if (!usbdrv_list(collect, &l)) { out_err("The drive is not available."); return 1; }

    uint32_t total = 0, freeb = 0;
    usbdrv_space(&total, &freeb);

    if (!l.n) {
        out_multi("  %s(empty -- drag a file onto the RPCORTEX drive)%s", C_GRAY, C_RESET);
    } else {
        out_multi("  TYPE   SIZE     MODIFIED             NAME");
        out_multi("  ----------------------------------------------------------");
        for (uint32_t i = 0; i < l.n; i++) {
            char size_s[12], when[24];
            if (l.e[i].is_dir) snprintf(size_s, sizeof(size_s), "-");
            else               fmt_size(l.e[i].size, size_s, sizeof(size_s));
            fmt_time(l.e[i].mtime, when, sizeof(when));
            out_multi("  %-5s  %-7s  %-19s  %s%s",
                      l.e[i].is_dir ? "DIR" : "FILE", size_s, when,
                      l.e[i].name, l.e[i].is_dir ? "/" : "");
        }
        if (l.skipped)
            out_warn("%u more not shown -- the listing holds %d.",
                     (unsigned)l.skipped, LIST_MAX);
    }
    char t[12], fr[12];
    fmt_size(total, t, sizeof(t));
    fmt_size(freeb, fr, sizeof(fr));

    static const char *kMode[] = { "off", "on", "on when plugged into a computer" };
    int m = usbmsc_mode();
    out_multi("  %s%s free of %s  ·  drive is %s%s", C_GRAY, fr, t,
              kMode[m < 0 || m > 2 ? 0 : m], C_RESET);

    // Files land in /usb by themselves; saying where is more use than leaving
    // someone to find out.
    out_multi("  %sAnything dropped here is copied to /usb, where the rest of the "
              "OS can read it.%s", C_GRAY, C_RESET);
    if (usbmsc_full())
        out_warn("The device is out of room, so the drive is write protected until "
                 "something is deleted.");
    return 0;
}

// The name a file should take on the other side, when none was given.
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// --- download mode ----------------------------------------------------------
//
// The drive appears, the shell waits, and files are taken as they land. It ends
// when told to.
//
// This replaced a background scan that ran on a timer, and the reasons are worth
// keeping. The scan lived on the usb task, whose stack could not hold the FAT12
// call chain — three nested 512-byte sector buffers — plus its own listing
// array. It overflowed, hard-faulted, and the watchdog restarted the board, in a
// loop. Raising the stack would have stopped the crash without fixing the
// actual problem: work nobody asked for, at a moment nobody chose, on the one
// task that must never stall.
//
// Doing it in the foreground is better on every count. It runs on the shell
// task, which has the stack for it. It runs when someone wants it to. Failures
// are visible rather than inferred from a reboot. And the drive is only exposed
// while somebody is standing there watching it, which is what the whole
// off-by-default posture was for.

uint32_t usbmsc_import_all(bool *refused);
bool usbmsc_settled(void);

static int cmd_download(int argc, char **argv) {
    (void)argc; (void)argv;
#if !CFG_TUD_MSC
    out_err("This build has no USB drive support.");
    return 1;
#else
    int was = usbmsc_mode();
    usbmsc_set_mode(1);                       // on, for as long as this lasts
    if (!usbdrv_ready()) {
        usbmsc_set_mode(was);
        out_err("The transfer area could not be prepared.");
        return 1;
    }

    out_blank();
    out_multi("  %s%sDOWNLOAD MODE%s", C_BOLD, C_CYAN, C_RESET);
    out_multi("  A drive named RPCORTEX is on this computer. Drop files onto it.");
    out_multi("  %sThey are copied into /usb as they arrive. Press any key when done.%s",
              C_GRAY, C_RESET);
    out_blank();

    // A spinner rather than a progress bar: there is no total to measure
    // against, because the host never says how much it intends to send.
    static const char *kSpin = "|/-\\";
    uint32_t frame = 0, taken = 0;
    bool refused = false, quit = false;

    while (!quit) {
        // A finished copy shows up as the host going quiet, which is the only
        // signal MSC offers short of a cache flush that not every host sends.
        if (usbmsc_settled()) {
            bool r = false;
            uint32_t n = usbmsc_import_all(&r);
            taken += n;
            refused = refused || r;
            if (n) {
                out_progress_done();
                out_ok("Took %u file%s into /usb", (unsigned)n, n == 1 ? "" : "s");
            }
            if (r) out_warn("Out of room -- the drive is now write protected.");
        }

        printf("\r  %c  waiting for files   %u taken   ",
               kSpin[frame++ & 3], (unsigned)taken);
        fflush(stdout);

        // Any key ends it, which is what the banner promised. Ctrl+C too, since
        // that is what fingers do.
        for (int i = 0; i < 10 && !quit; i++) {
            int c = getchar_timeout_us(20000);
            if (c != PICO_ERROR_TIMEOUT) quit = true;
        }
        if (intr_check()) quit = true;
    }

    // One last sweep, for anything that landed while the key was being pressed.
    bool r = false;
    taken += usbmsc_import_all(&r);
    refused = refused || r;

    out_progress_done();
    printf("\r                                          \r");
    usbmsc_set_mode(was);

    out_blank();
    if (taken) out_ok("%u file%s in /usb", (unsigned)taken, taken == 1 ? "" : "s");
    else       out_multi("  %sNothing arrived.%s", C_GRAY, C_RESET);
    if (refused)
        out_warn("Something did not fit. Free space with 'rm' and try again.");
    out_multi("  %sThe drive is %s again.%s", C_GRAY,
              was == 0 ? "hidden" : was == 1 ? "on" : "automatic", C_RESET);
    return 0;
#endif
}

static int cmd_usb(int argc, char **argv) {
    const char *sub = argc >= 2 ? argv[1] : "ls";

    if (strcmp(sub, "on") == 0) {
        usbmsc_set_mode(1);
        out_ok("The drive is on.");
        out_multi("  %sIt should appear within a second or two; unplug and replug "
                  "if the host does not notice.%s", C_GRAY, C_RESET);
        return 0;
    }
    if (strcmp(sub, "off") == 0) {
        usbmsc_set_mode(0);
        out_ok("The drive is off. Nothing on it will be served over USB.");
        out_multi("  %sThe host may keep showing a drive until it is unplugged; it "
                  "is empty and stays empty. The console is unaffected.%s", C_GRAY, C_RESET);
        return 0;
    }
    if (strcmp(sub, "auto") == 0) {
        usbmsc_set_mode(2);
        out_ok("The drive will appear whenever this is plugged into a computer.");
        out_multi("  %sAnd stay hidden on a charger or a battery, where there is "
                  "nobody to show it to.%s", C_GRAY, C_RESET);
        return 0;
    }

    if (!usbmsc_enabled()) {
        // The commands below still work when the drive is not being offered —
        // the transfer area is on the device either way, and being able to
        // empty it without exposing it first is the point.
        if (strcmp(sub, "ls") != 0 && strcmp(sub, "status") != 0 &&
            strcmp(sub, "get") != 0 && strcmp(sub, "rm") != 0 &&
            strcmp(sub, "format") != 0) {
            out_err("The drive is off. 'usb on' to offer it, or 'usb auto'.");
            return 1;
        }
    }
    if (!usbdrv_ready()) {
        out_err("The transfer area could not be prepared.");
        return 1;
    }

    if (strcmp(sub, "download") == 0) return cmd_download(0, nullptr);
    if (strcmp(sub, "ls") == 0 || strcmp(sub, "status") == 0) return cmd_usb_ls();

    if (strcmp(sub, "get") == 0) {
        if (argc < 3) { out_err("Usage: usb get <name> [destination]"); return 1; }
        char dest[128];
        if (argc >= 4) path_resolve(fs_cwd(), argv[3], dest, sizeof(dest));
        else snprintf(dest, sizeof(dest), "/%s", basename_of(argv[2]));

        F12Entry e;
        if (!usbdrv_find(argv[2], &e)) { out_err("No '%s' on the drive.", argv[2]); return 1; }
        if (e.is_dir) { out_err("'%s' is a directory.", argv[2]); return 1; }
        if (e.size > storage_free_bytes()) {
            out_err("Not enough room on the device for %s.", argv[2]);
            return 1;
        }
        if (!usbdrv_import(argv[2], dest)) { out_err("Could not copy '%s'.", argv[2]); return 1; }
        char s[12];
        fmt_size(e.size, s, sizeof(s));
        out_ok("Copied %s (%s) to %s", argv[2], s, dest);
        return 0;
    }

    if (strcmp(sub, "put") == 0) {
        if (argc < 3) { out_err("Usage: usb put <path> [name]"); return 1; }
        char src[128];
        path_resolve(fs_cwd(), argv[2], src, sizeof(src));
        const char *name = argc >= 4 ? argv[3] : basename_of(src);

        bool is_dir = false;
        uint32_t size = 0;
        if (!storage_stat(src, &is_dir, &size)) { out_err("No such file: %s", src); return 1; }
        if (is_dir) { out_err("'%s' is a directory.", src); return 1; }

        uint32_t total = 0, freeb = 0;
        usbdrv_space(&total, &freeb);
        if (size > freeb) {
            char a[12], b[12];
            fmt_size(size, a, sizeof(a));
            fmt_size(freeb, b, sizeof(b));
            out_err("%s needs %s and the drive has %s free.", name, a, b);
            return 1;
        }
        if (!usbdrv_export(src, name)) { out_err("Could not copy '%s'.", src); return 1; }
        char s[12];
        fmt_size(size, s, sizeof(s));
        out_ok("Copied %s (%s) to the drive as %s", src, s, name);
        out_multi("  %sReplug for the host to see it.%s", C_GRAY, C_RESET);
        return 0;
    }

    if (strcmp(sub, "rm") == 0) {
        if (argc < 3) { out_err("Usage: usb rm <name>"); return 1; }
        if (!usbdrv_remove(argv[2])) { out_err("No '%s' on the drive.", argv[2]); return 1; }
        out_ok("Removed %s from the drive.", argv[2]);
        return 0;
    }

    if (strcmp(sub, "format") == 0) {
        if (argc < 3 || strcmp(argv[2], "yes") != 0) {
            out_warn("This erases everything on the drive.");
            out_multi("  Run 'usb format yes' to go ahead.");
            return 1;
        }
        if (!usbdrv_format()) { out_err("Could not format the drive."); return 1; }
        out_ok("The drive is empty.");
        return 0;
    }

    out_err("Usage: usb [ls | download | get <name> [dest] | put <path> [name] | "
            "rm <name> | format yes | on | off | auto]");
    return 1;
}
#else
static int cmd_usb(int argc, char **argv) {
    (void)argc; (void)argv;
    out_err("This build has no USB drive support.");
    return 1;
}
#endif

void usbdrive_register(void) {
    static const Command d{"download",
        "download  -- show the drive and take what lands on it", cmd_download, nullptr};
    cmd_register(&d);

    static const Command c{"usb",
        "usb [ls | get <name> [dest] | put <path> [name] | rm <name> | format yes | on | off | auto]",
        cmd_usb, nullptr};
    cmd_register(&c);
    cmd_alias("usbdrive", "usb");
}
