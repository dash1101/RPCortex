#include "interrupt.h"
#include "task.h"

static volatile bool g_pending;
static IntrPollFn    g_poll;

// Bytes intr_check pulled off the input while looking for Ctrl+C.
//
// They cannot be thrown away. A command runs between the user pressing Enter and
// the prompt coming back, and anything typed in that window — or, far more
// commonly, the remaining lines of a pasted block — arrives while a COMMAND owns
// the input rather than the line editor. Discarding it looks like the shell
// dropping characters, which is a worse bug than the one interrupts fix.
//
// So they are stashed here and the line editor drains this first. A ring rather
// than a growing buffer: type-ahead past a screenful is not worth heap, and
// dropping the oldest is the least surprising thing to do when it fills.
#define STASH_N 128
static char     g_stash[STASH_N];
static uint32_t g_head, g_tail;

static void stash_put(char c) {
    uint32_t next = (g_head + 1) % STASH_N;
    if (next == g_tail) g_tail = (g_tail + 1) % STASH_N;   // full: drop the oldest
    g_stash[g_head] = c;
    g_head = next;
}

int intr_stashed(void) {
    if (g_tail == g_head) return -1;
    char c = g_stash[g_tail];
    g_tail = (g_tail + 1) % STASH_N;
    return (unsigned char)c;
}

void intr_stash_clear(void) { g_head = g_tail = 0; }

void intr_set_poll(IntrPollFn fn) { g_poll = fn; }

// Who owns the console byte stream, or 0 for "the usual arrangement". A pid
// rather than a bool so the owner can still poll — a transfer is entitled to
// its own input — and so a claim left behind by a task that has gone can be
// seen for what it is.
static int g_input_owner;

bool intr_input_claim(void) {
    int me = task_self();
    if (g_input_owner && g_input_owner != me) return false;
    g_input_owner = me;
    return true;
}

void intr_input_release(void) {
    if (g_input_owner == task_self()) g_input_owner = 0;
}

void intr_raise(void)  { g_pending = true; }
bool intr_pending(void){ return g_pending; }
void intr_clear(void)  { g_pending = false; }

bool intr_check(void) {
    // Every place that polls for Ctrl+C is also a place where it is safe to give
    // up the core — that is exactly what "safe to be interrupted here" means. So
    // the two are the same call, and adding interrupt handling earlier turned
    // out to have scattered the yield points for free.
    task_yield();

    // A task asked to stop reports the same way a Ctrl+C does, so a command
    // written to handle one handles the other with no extra code.
    if (task_should_stop()) return true;

    if (g_pending) return true;
    if (!g_poll) return false;
    // Somebody else is reading the console as DATA. Taking bytes off it here
    // would be taking them out of their file — see intr_input_claim. The flag
    // and the stop request above still work, which is everything a background
    // task actually needs.
    if (g_input_owner && g_input_owner != task_self()) return false;
    // Bounded: a command polling in a loop must not be able to spin in here on
    // a flooded input.
    for (int i = 0; i < 32; i++) {
        int c = g_poll();
        if (c < 0) break;
        if (c == 0x03) { g_pending = true; return true; }
        stash_put((char)c);
    }
    return g_pending;
}
