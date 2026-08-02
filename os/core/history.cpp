#include "history.h"
#include <string.h>

static char g_hist[HIST_N][HIST_LINE_MAX];
static int  g_count;   // how many stored (<= HIST_N)
static int  g_next;    // index the next entry will be written to (circular)

void hist_reset(void) { g_count = 0; g_next = 0; }

int hist_count(void) { return g_count; }

void hist_add(const char *line) {
    if (!line || !line[0]) return;
    if (g_count) {
        int last = (g_next - 1 + HIST_N) % HIST_N;
        if (strcmp(g_hist[last], line) == 0) return;   // no consecutive dupes
    }
    strncpy(g_hist[g_next], line, HIST_LINE_MAX - 1);
    g_hist[g_next][HIST_LINE_MAX - 1] = 0;
    g_next = (g_next + 1) % HIST_N;
    if (g_count < HIST_N) g_count++;
}

const char *hist_get(int depth) {
    if (depth < 0 || depth >= g_count) return nullptr;
    int idx = (g_next - 1 - depth + 2 * HIST_N) % HIST_N;
    return g_hist[idx];
}
