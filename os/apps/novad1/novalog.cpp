// Desc: A capped event log on flash.
// File: novalog.cpp
#include "novalog.h"
#include "novacore.h"

#include "rpc_app.h"
#include <string.h>
#include <stdio.h>

namespace nova {
namespace log {

#define LOG_PATH  NOVA_LOGS "/nova.log"
#define LINE_MAX  96
// Sixty lines of ninety-six is under 6 KB, which is small enough to read whole,
// trim and write back without an allocation and without a second copy on disk.
#define BUF_MAX   (MAX_LINES * LINE_MAX)

static char g_buf[BUF_MAX];

// Lines are held newest-LAST in the file, the way anyone reading it with `cat`
// expects, and returned newest-FIRST by line(), the way a screen shows them.

static unsigned load(void) {
    unsigned n = fw_file_read(LOG_PATH, g_buf, sizeof(g_buf) - 1);
    if (n >= sizeof(g_buf)) n = sizeof(g_buf) - 1;
    g_buf[n] = 0;
    return n;
}

void write(const char *msg) {
    if (!msg || !*msg) return;

    char stamp[16];
    if (!nova::time_hhmm(stamp, sizeof(stamp))) nova::copy(stamp, sizeof(stamp), "--:--");

    char entry[LINE_MAX];
    snprintf(entry, sizeof(entry), "%s %s", stamp, msg);
    // A newline inside an entry would make one event read as two, and the
    // trimmer counts lines.
    for (char *p = entry; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';

    unsigned n = load();

    // Trim from the FRONT until there is room, before appending. Doing it after
    // would mean a moment where the file is over the cap, and on a device that
    // can lose power at any instant "a moment" is a state somebody finds later.
    unsigned need = (unsigned)strlen(entry) + 1;
    int lines = 0;
    for (unsigned i = 0; i < n; i++) if (g_buf[i] == '\n') lines++;

    unsigned from = 0;
    while ((lines >= MAX_LINES || n - from + need >= sizeof(g_buf)) && from < n) {
        const char *nl = strchr(g_buf + from, '\n');
        if (!nl) { from = n; break; }
        from = (unsigned)(nl - g_buf) + 1;
        lines--;
    }
    if (from) { memmove(g_buf, g_buf + from, n - from); n -= from; }

    memcpy(g_buf + n, entry, strlen(entry));
    n += (unsigned)strlen(entry);
    g_buf[n++] = '\n';
    fw_file_write(LOG_PATH, g_buf, n);
}

int count(void) {
    unsigned n = load();
    int lines = 0;
    for (unsigned i = 0; i < n; i++) if (g_buf[i] == '\n') lines++;
    return lines;
}

bool line(int i, char *out, unsigned cap) {
    if (!out || !cap || i < 0) return false;
    unsigned n = load();
    // Walk backwards for the i-th newline from the end, which is where the
    // newest entries are.
    int seen = -1;
    unsigned end = n;
    for (int p = (int)n - 1; p >= 0; p--) {
        if (g_buf[p] != '\n') continue;
        seen++;
        if (seen == i) { end = (unsigned)p; continue; }
        if (seen == i + 1) {
            unsigned start = (unsigned)p + 1;
            unsigned len = end > start ? end - start : 0;
            if (len >= cap) len = cap - 1;
            memcpy(out, g_buf + start, len);
            out[len] = 0;
            return true;
        }
    }
    // The very first line has no newline before it.
    if (seen == i && end > 0) {
        unsigned len = end < cap ? end : cap - 1;
        memcpy(out, g_buf, len);
        out[len] = 0;
        return true;
    }
    return false;
}

void clear(void) { fw_file_remove(LOG_PATH); }

}  // namespace log
}  // namespace nova
