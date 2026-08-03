// logdump — read the log ring.
//
// v1 had this in its recovery set and it earned its place: when something goes
// wrong on a device with no screen, the question is always "what happened just
// before", and scrollback does not survive a reboot.

#include "command.h"
#include "out.h"
#include "logring.h"
#include "storage.h"
#include "path.h"

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

static const char *kind_tag(uint8_t k) {
    switch (k) {
        case LOG_K_ERR:  return "!";
        case LOG_K_WARN: return "?";
        case LOG_K_BOOT: return ".";
        case LOG_K_OK:   return "@";
        default:         return ":";
    }
}

// One line, formatted the same way whether it is going to the console or a file.
static int format_line(const LogLine *l, char *out, uint32_t cap, bool colour) {
    unsigned s  = (unsigned)(l->at_ms / 1000);
    unsigned ms = (unsigned)(l->at_ms % 1000);
    if (colour)
        return snprintf(out, cap, "  %s%4u.%03u%s  %s[%s]%s %s",
                        C_GRAY, s, ms, C_RESET,
                        kind_colour(l->kind), kind_tag(l->kind), C_RESET, l->text);
    return snprintf(out, cap, "%4u.%03u  [%s] %s", s, ms, kind_tag(l->kind), l->text);
}

static int cmd_logdump(int argc, char **argv) {
    // logdump save [file] — write it out while there is somewhere to write it.
    if (argc >= 2 && !strcmp(argv[1], "save")) {
        const char *name = (argc >= 3) ? argv[2] : "log.txt";
        char path[128];
        path_resolve(fs_cwd(), name, path, sizeof(path));

        // Built in one buffer and written once: appending per line would take
        // the filesystem lock dozens of times and interleave with anything else
        // writing.
        static char blob[LOG_LINES * (LOG_LINE_MAX + 24)];
        uint32_t n = 0;
        for (uint32_t i = 0; i < log_count(); i++) {
            const LogLine *l = log_at(i);
            if (!l) break;
            n += (uint32_t)format_line(l, blob + n, sizeof(blob) - n - 2, false);
            if (n + 2 >= sizeof(blob)) break;
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
