#include "blackbox.h"

#include <string.h>

// BUMPED WHEN THE LAYOUT CHANGES, and the layout is why it is checked at all.
// This struct is read out of memory the reset does not clear, so the first boot
// after a firmware update is reading bytes the PREVIOUS firmware wrote. Leaving
// the magic alone across a layout change means those bytes are reinterpreted
// under the new field offsets and reported as a crash that never happened.
// A mismatch costs one boot's worth of history, which is the right price.
#define BB_MAGIC 0x42425832u   // 'BBX2'

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
  #define BB_NOINIT __attribute__((section(".uninitialized_data.rpcbb")))
#else
  #define BB_NOINIT
#endif

static BB_NOINIT BlackBox g_bb;
// The snapshot of the PREVIOUS run, taken before g_bb is reset. A plain global,
// because it only has to survive this run.
static BlackBox g_prev;
static bool     g_have_prev;

// A bounded copy, not snprintf.
//
// Every one of these fields was filled with snprintf(dst, cap, "%s", src), which
// is a full vfprintf — and newlib's wants over a kilobyte of stack. That cost is
// paid in the two places least able to afford it: bb_note_task runs inside the
// scheduler, and bb_note_phase is reachable from fw_millis, which runs on a
// PACKAGE's stack. A checkpoint that can overflow the stack it is describing is
// not a diagnostic, and the whole point of these notes is to be safe to put
// anywhere, including inside a fault.
static void copy_into(char *dst, unsigned cap, const char *src) {
    unsigned n = 0;
    if (src) while (n + 1 < cap && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

// Clearing the task name is what marks it: the reporters all key off that
// field, so a run that ends on purpose leaves nothing for them to find.
void bb_note_clean_exit(void) {
    g_bb.task[0] = 0;
    g_bb.cmd[0]  = 0;
    g_bb.phase[0] = 0;
    g_bb.stuck   = BB_STUCK_NO;
}

void bb_init(void) {
    if (g_bb.magic == BB_MAGIC) {
        g_prev = g_bb;
        g_have_prev = true;
    }
    memset(&g_bb, 0, sizeof(g_bb));
    g_bb.magic = BB_MAGIC;
}

const BlackBox *bb_previous(void) { return g_have_prev ? &g_prev : nullptr; }

void bb_note_task(int pid, uint8_t core, const char *name,
                  uint32_t stack_used, uint32_t stack_size) {
    if (g_bb.magic != BB_MAGIC) bb_init();
    g_bb.pid  = pid;
    g_bb.core = core;
    g_bb.stack_used = stack_used;
    g_bb.stack_size = stack_size;
    copy_into(g_bb.task, sizeof(g_bb.task), name ? name : "?");
}

void bb_note_command(const char *line) {
    if (g_bb.magic != BB_MAGIC) bb_init();
    copy_into(g_bb.cmd, sizeof(g_bb.cmd), line);
}

void bb_note_phase(const char *what) {
    if (g_bb.magic != BB_MAGIC) bb_init();
    copy_into(g_bb.phase, sizeof(g_bb.phase), what);
}

const char *bb_phase(void) {
    if (g_bb.magic != BB_MAGIC) return "";
    g_bb.phase[sizeof(g_bb.phase) - 1] = 0;
    return g_bb.phase;
}

void bb_note_hw_twice(uint8_t core, uint32_t pc) {
    if (g_bb.magic != BB_MAGIC) bb_init();
    // The FIRST one is the one worth keeping: everything after it is a
    // consequence of a core that has already stopped.
    if (!g_bb.hw_twice) {
        g_bb.hw_twice_core = core;
        g_bb.hw_twice_pc   = pc;
    }
    g_bb.hw_twice++;
}

void bb_note_stall(uint32_t ms, bool crit, uint32_t pc) {
    if (g_bb.magic != BB_MAGIC) return;    // too early to matter, and not worth a reset
    if (ms <= g_bb.max_stall_ms) return;
    g_bb.max_stall_ms = ms;
    g_bb.stall_crit   = crit ? 1 : 0;
    g_bb.max_stall_pc = pc;
}

void bb_note_yield(uint32_t now_ms) {
    g_bb.last_yield_ms = now_ms;
    g_bb.yields++;
    // See the header: a device that stalled and then came back must not carry
    // the flag into the next thing that goes wrong.
    g_bb.stuck = BB_STUCK_NO;
}

void bb_note_stuck(uint8_t why) {
    // No magic check, deliberately. Everything else here calls bb_init when the
    // struct looks unfamiliar, and bb_init memsets — which is a great deal to
    // do from an interrupt that fired inside a wedged task. A single store into
    // a struct that has not been initialised is harmless; the run it would be
    // reported against does not exist.
    g_bb.stuck = why;
}

uint32_t bb_stall_ms(uint32_t now_ms) {
    if (g_bb.magic != BB_MAGIC || g_bb.last_yield_ms == 0) return 0;
    // Signed difference, then clamp.
    //
    // This struct survives a reset, so after a watchdog reboot last_yield_ms
    // holds a timestamp from the PREVIOUS run — say 16000 — while task_now_ms
    // has restarted near zero. The unsigned subtraction wrapped to about 4.29
    // billion, which read as a 49-day stall and tripped every escalation stage
    // instantly, forever. A clock that has gone backwards means "no stall
    // measurable", not "the longest stall imaginable".
    int32_t d = (int32_t)(now_ms - g_bb.last_yield_ms);
    return d > 0 ? (uint32_t)d : 0;
}
