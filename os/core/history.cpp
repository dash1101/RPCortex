#include "history.h"
#include <string.h>

void hist_ring_add(HistRing *r, const char *line) {
    if (!r || !line || !line[0]) return;
    if (r->count) {
        int last = (r->next - 1 + HIST_N) % HIST_N;
        if (strcmp(r->line[last], line) == 0) return;   // no consecutive dupes
    }
    strncpy(r->line[r->next], line, HIST_LINE_MAX - 1);
    r->line[r->next][HIST_LINE_MAX - 1] = 0;
    r->next = (r->next + 1) % HIST_N;
    if (r->count < HIST_N) r->count++;
}

const char *hist_ring_get(const HistRing *r, int depth) {
    if (!r || depth < 0 || depth >= r->count) return nullptr;
    int idx = (r->next - 1 - depth + 2 * HIST_N) % HIST_N;
    return r->line[idx];
}

static HistRing g_console;

void hist_reset(void) { g_console.count = 0; g_console.next = 0; }
int hist_count(void) { return g_console.count; }
void hist_add(const char *line) { hist_ring_add(&g_console, line); }
const char *hist_get(int depth) { return hist_ring_get(&g_console, depth); }
