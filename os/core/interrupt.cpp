#include "interrupt.h"

static volatile bool g_pending;
static IntrPollFn    g_poll;

void intr_set_poll(IntrPollFn fn) { g_poll = fn; }

void intr_raise(void)  { g_pending = true; }
bool intr_pending(void){ return g_pending; }
void intr_clear(void)  { g_pending = false; }

bool intr_check(void) {
    if (g_pending) return true;
    if (!g_poll) return false;
    // Drain whatever is waiting, looking for 0x03. Everything else is discarded
    // rather than queued: a command that is not a line editor has nowhere to put
    // a keystroke, and silently buffering it would make it appear on the prompt
    // afterwards as though the user had typed it there.
    for (int i = 0; i < 32; i++) {
        int c = g_poll();
        if (c < 0) break;
        if (c == 0x03) { g_pending = true; return true; }
    }
    return g_pending;
}
