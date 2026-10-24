// logdump — read the log ring.
//
// v1 had this in its recovery set and it earned its place: when something goes
// wrong on a device with no screen, the question is always "what happened just
// before", and scrollback does not survive a reboot.

#include "command.h"
#include "out.h"
#include "logring.h"
#include "crashlog.h"
#include "blackbox.h"
#include "storage.h"
#include "path.h"
#include "fsguard.h"

#include <stdio.h>
#include <string.h>

const char *fs_cwd(void);

static const char *kind_colour(uint8_t k) {
    switch (k) {
        case LOG_K_ERR:  return C_FAIL;
        case LOG_K_WARN: return C_WARN;
        case LOG_K_BOOT: return C_GRAY;
        case LOG_K_OK:   return C_CYAN;
        default:         return C_HEADER;
    }
}

// One line, formatted the same way whether it is going to the console or a file.
static int format_line(const LogLine *l, char *out, uint32_t cap, bool colour) {
    unsigned s  = (unsigned)(l->at_ms / 1000);
    unsigned ms = (unsigned)(l->at_ms % 1000);
    if (colour)
        return snprintf(out, cap, "  %s%4u.%03u%s  %s[%s]%s %s",
                        C_GRAY, s, ms, C_RESET,
                        kind_colour(l->kind), log_kind_tag(l->kind), C_RESET, l->text);
    return snprintf(out, cap, "%4u.%03u  [%s] %s", s, ms, log_kind_tag(l->kind), l->text);
}

// One buffer for both `logdump save` and the crash report. Neither runs while the
// other does (the crash report is written once, at boot, before any command can),
// and a second static of this size would cost the same RAM again for nothing.
static char g_blob[CRASHLOG_MAX];

// Write the previous run's black box and the log ring to CRASHLOG_PATH, keeping
// the report before it as CRASHLOG_OLD. Called once at boot, after the
// filesystem is mounted and /os exists, and before anything can push the
// previous run's lines out of the ring. Returns false if nothing could be
// written; that is logged but never stops the boot.
//
// The text it writes is host-tested (crashlog_test). The rotate-then-write ran
// against real littlefs on the emulator's flash model, across two crashes in a
// row. DEVICE-UNCONFIRMED: it has not run on a board's flash.
bool crashlog_save(const BlackBox *bb) {
    size_t n = crashlog_format(g_blob, sizeof(g_blob), bb);
    bool is_dir = false;
    uint32_t sz = 0;
    if (storage_stat(CRASHLOG_PATH, &is_dir, &sz) && !is_dir) {
        storage_remove(CRASHLOG_OLD);                 // fine if there was none
        if (!storage_rename(CRASHLOG_PATH, CRASHLOG_OLD))
            storage_remove(CRASHLOG_PATH);            // better to lose the old than the new
    }
    if (!storage_write_file(CRASHLOG_PATH, (const uint8_t *)g_blob, (uint32_t)n)) {
        log_addf(LOG_K_WARN, "[Crash] could not write %s", CRASHLOG_PATH);
        return false;
    }
    log_addf(LOG_K_INFO, "[Crash] report saved to %s", CRASHLOG_PATH);
    return true;
}

static int cmd_logdump(int argc, char **argv) {
    // logdump save [file] — write it out while there is somewhere to write it.
    if (argc >= 2 && !strcmp(argv[1], "save")) {
        const char *name = (argc >= 3) ? argv[2] : "log.txt";
        char path[128];
        path_resolve(fs_cwd(), name, path, sizeof(path));
        if (!fs_allow_change(path)) return 1;

        // Built in one buffer and written once: appending per line would take
        // the filesystem lock dozens of times and interleave with anything else
        // writing.
        char *blob = g_blob;
        uint32_t n = 0;
        for (uint32_t i = 0; i < log_count(); i++) {
            const LogLine *l = log_at(i);
            if (!l) break;
            n += (uint32_t)format_line(l, blob + n, sizeof(g_blob) - n - 2, false);
            if (n + 2 >= sizeof(g_blob)) break;
            blob[n++] = '\n';
        }
        if (!storage_write_file(path, (const uint8_t *)blob, n)) {
            out_err("Could not write %s.", path);
            return 1;
        }
        out_ok("Wrote %u line%s to %s.", (unsigned)log_count(),
               log_count() == 1 ? "" : "s", path);
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        log_clear();
        out_ok("Log cleared.");
        return 0;
    }

    if (!log_count()) { out_multi("  (nothing logged yet)"); return 0; }

    if (log_dropped())
        out_warn("%u earlier line%s dropped — the ring holds %u.",
                 (unsigned)log_dropped(), log_dropped() == 1 ? "" : "s",
                 (unsigned)LOG_LINES);

    char line[LOG_LINE_MAX + 64];
    for (uint32_t i = 0; i < log_count(); i++) {
        const LogLine *l = log_at(i);
        if (!l) break;
        format_line(l, line, sizeof(line), true);
        out_multi("%s", line);
    }
    out_multi("  %s%u line%s%s", C_GRAY, (unsigned)log_count(),
              log_count() == 1 ? "" : "s", C_RESET);
    return 0;
}

void log_register(void) {
    static const Command c{"logdump", "logdump [save <file> | clear]", cmd_logdump, nullptr};
    cmd_register(&c);
    cmd_alias("log",  "logdump");
    cmd_alias("dmesg", "logdump");
}
