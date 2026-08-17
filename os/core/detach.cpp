#include "detach.h"

#include <string.h>

static DetachRun g_runs[DETACH_MAX];
static uint32_t  g_seq;

// The one output buffer, and which run has it. See the note in the header for
// why there is one rather than one each.
static char g_capture[DETACH_OUT_MAX];
static int  g_capture_slot = -1;

// --- handles ------------------------------------------------------------------
//
// slot + generation, packed by arithmetic rather than by shifting. A shift and a
// mask would need the two field widths to agree in three places; this needs them
// to agree nowhere, and the test can read it.
//
// The generation wraps at 256, so a handle held across 256 claims of the same
// slot could match again. That is a package holding a handle through hundreds of
// its own commands and then polling it, and it costs a stale answer rather than
// anything unsafe.
static int handle_of(uint32_t slot, uint8_t gen) {
    return (int)(gen * (uint32_t)DETACH_MAX + slot);
}

static DetachRun *lookup(int handle) {
    if (handle < 0) return nullptr;
    uint32_t slot = (uint32_t)handle % DETACH_MAX;
    uint8_t  gen  = (uint8_t)((uint32_t)handle / DETACH_MAX);
    DetachRun *r = &g_runs[slot];
    if (!r->used || r->gen != gen) return nullptr;
    return r;
}

static uint32_t slot_index(const DetachRun *r) {
    return (uint32_t)(r - g_runs);
}

static void free_slot(DetachRun *r) {
    uint32_t slot = slot_index(r);
    if (g_capture_slot == (int)slot) g_capture_slot = -1;
    uint8_t gen = r->gen;              // kept: it is what makes a stale handle stale
    memset(r, 0, sizeof(*r));
    r->gen = gen;
}

// --- claiming -----------------------------------------------------------------

static DetachRun *free_or_reclaimed(void) {
    for (uint32_t i = 0; i < DETACH_MAX; i++)
        if (!g_runs[i].used) return &g_runs[i];

    // Everything is taken. A run that FINISHED and was never collected is fair
    // game — its answer has been sitting unread and the package that asked has
    // moved on. Oldest first, so a screen firing a command per keypress loses
    // the stalest answer rather than an arbitrary one.
    DetachRun *oldest = nullptr;
    for (uint32_t i = 0; i < DETACH_MAX; i++) {
        if (!g_runs[i].done) continue;
        if (!oldest || g_runs[i].seq < oldest->seq) oldest = &g_runs[i];
    }
    if (!oldest) return nullptr;       // all still running: genuinely no room
    free_slot(oldest);
    return oldest;
}

int detach_claim(const void *owner, const char *line,
                 char *pkg_out, uint32_t pkg_cap) {
    if (!owner || !line || !line[0]) return -1;

    // Refused rather than truncated. A shell line cut in half is a different
    // command, and one that happens to still parse is the worst kind.
    uint32_t n = 0;
    while (line[n] && n < DETACH_LINE_MAX) n++;
    if (n >= DETACH_LINE_MAX) return -1;

    bool wants_output = pkg_out && pkg_cap;
    // One capture in the OS, so one capturing run. Said here, at the point of
    // asking, rather than letting the second one run and hand back an empty
    // buffer that reads exactly like a command which printed nothing.
    if (wants_output && g_capture_slot >= 0) return -1;

    DetachRun *r = free_or_reclaimed();
    if (!r) return -1;

    uint8_t gen = (uint8_t)(r->gen + 1);
    memset(r, 0, sizeof(*r));
    r->used  = 1;
    r->gen   = gen;
    r->owner = owner;
    r->pid   = -1;
    r->seq   = ++g_seq;
    memcpy(r->line, line, n);
    r->line[n] = 0;

    if (wants_output) {
        r->pkg_out   = pkg_out;
        r->pkg_cap   = pkg_cap;
        r->capturing = 1;
        g_capture_slot = (int)slot_index(r);
        g_capture[0] = 0;
    }
    return handle_of(slot_index(r), gen);
}

// --- looking one up -----------------------------------------------------------

DetachRun *detach_find(int handle, const void *owner) {
    DetachRun *r = lookup(handle);
    // A null owner never matches. A run whose package has unloaded has its owner
    // cleared, and letting null match would make every one of those collectable
    // by anybody who guessed a handle.
    if (!r || !owner || r->owner != owner) return nullptr;
    return r;
}

DetachRun *detach_of(int handle) { return lookup(handle); }

char *detach_capture_buffer(int handle) {
    DetachRun *r = lookup(handle);
    if (!r || !r->capturing) return nullptr;
    if (g_capture_slot != (int)slot_index(r)) return nullptr;
    return g_capture;
}

// --- finishing ----------------------------------------------------------------

void detach_finish(int handle, int status) {
    DetachRun *r = lookup(handle);
    if (!r || r->done) return;
    r->status = status;
    // The flag LAST, after everything a reader will look at. A poll only touches
    // status and the buffer once it has seen this, so the order these are
    // written in is the whole of the synchronisation between the two tasks.
    r->done = 1;
}

void detach_release(int handle) {
    DetachRun *r = lookup(handle);
    if (r) free_slot(r);
}

void detach_forget_owner(const void *owner) {
    if (!owner) return;
    for (uint32_t i = 0; i < DETACH_MAX; i++) {
        if (!g_runs[i].used || g_runs[i].owner != owner) continue;
        // The package's buffer goes with the package. Clearing it here is what
        // stops anything writing into memory the heap has taken back, even if a
        // poll somehow reached this run.
        g_runs[i].owner   = nullptr;
        g_runs[i].pkg_out = nullptr;
        g_runs[i].pkg_cap = 0;
        // A finished run is nobody's now, so give the slot back at once. One
        // still going keeps it: the task is real and this is where it reports.
        if (g_runs[i].done) free_slot(&g_runs[i]);
    }
}

uint32_t detach_active(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < DETACH_MAX; i++) if (g_runs[i].used) n++;
    return n;
}

void detach_reset(void) {
    memset(g_runs, 0, sizeof(g_runs));
    g_capture_slot = -1;
    g_seq = 0;
}
