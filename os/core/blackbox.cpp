#include "blackbox.h"

#include <string.h>
#include <stdio.h>

#define BB_MAGIC 0x42425831u   // 'BBX1'

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

// Clearing the task name is what marks it: the reporters all key off that
// field, so a run that ends on purpose leaves nothing for them to find.
void bb_note_clean_exit(void) {
    g_bb.task[0] = 0;
    g_bb.cmd[0]  = 0;
    g_bb.phase[0] = 0;
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
    // snprintf rather than strncpy: this runs in the scheduler and a
    // non-terminated name would be read back as garbage after a reboot, which is
    // the one moment it has to be right.
    snprintf(g_bb.task, sizeof(g_bb.task), "%s", name ? name : "?");
}

void bb_note_command(const char *line) {
    if (g_bb.magic != BB_MAGIC) bb_init();
    snprintf(g_bb.cmd, sizeof(g_bb.cmd), "%s", line ? line : "");
}

void bb_note_phase(const char *what) {
    if (g_bb.magic != BB_MAGIC) bb_init();
    snprintf(g_bb.phase, sizeof(g_bb.phase), "%s", what ? what : "");
}

const char *bb_phase(void) {
    if (g_bb.magic != BB_MAGIC) return "";
    g_bb.phase[sizeof(g_bb.phase) - 1] = 0;
    return g_bb.phase;
}

void bb_note_yield(uint32_t now_ms) {
    g_bb.last_yield_ms = now_ms;
    g_bb.yields++;
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
