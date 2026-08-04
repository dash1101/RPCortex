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

#include <stdio.h>
#include <string.h>

#if CFG_TUD_MSC
bool usbmsc_enabled(void);
void usbmsc_set_enabled(bool on);
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
    out_multi("  %s%s free of %s%s", C_GRAY, fr, t, C_RESET);
    return 0;
}

// The name a file should take on the other side, when none was given.
static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int cmd_usb(int argc, char **argv) {
    const char *sub = argc >= 2 ? argv[1] : "ls";

    if (strcmp(sub, "on") == 0) {
        usbmsc_set_enabled(true);
        out_ok("The drive is on.");
        out_multi("  %sUnplug and replug for the host to pick it up.%s", C_GRAY, C_RESET);
        return 0;
    }
    if (strcmp(sub, "off") == 0) {
        usbmsc_set_enabled(false);
        out_ok("The drive is off. Nothing on it will be served over USB.");
        out_multi("  %sThe host may keep showing a drive until it is unplugged; it "
                  "is empty and stays empty. The console is unaffected.%s", C_GRAY, C_RESET);
        return 0;
    }

    if (!usbmsc_enabled()) {
        out_err("The drive is off. 'usb on' to turn it back on.");
        return 1;
    }
    if (!usbdrv_ready()) {
        out_err("The transfer area could not be prepared.");
        return 1;
    }

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

    out_err("Usage: usb [ls | get <name> [dest] | put <path> [name] | rm <name> | "
            "format yes | on | off]");
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
    static const Command c{"usb",
        "usb [ls | get <name> [dest] | put <path> [name] | rm <name> | format yes | on | off]",
        cmd_usb, nullptr};
    cmd_register(&c);
    cmd_alias("usbdrive", "usb");
}
