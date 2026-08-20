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

// Who owns the console byte stream.
//
// Held as a flag AND a pid rather than "pid or a sentinel": task_self() has no
// meaningful value before the scheduler has a current task, so folding "nobody
// holds it" into the pid would make a claim taken that early quietly do nothing.
// The flag says whether it is held; the pid is the ownership identity — who may
// read, who may release.
//
// The SLOT is kept too, and it is what the automatic reclaim keys on. A claim
// that is never released — because the task holding it faulted, or was killed,
// or was abandoned mid-call — used to own the console until a reboot, and that
// dead claim is the whole reason a package could not be trusted to read a line.
// task_slot_recycled hands out the slot of a task that has gone, exactly as it
// does for the sandbox pool and the network slots, and this is swept the same
// way. The slot is recorded at claim time, while the task is alive, so nothing
// has to translate a pid that no longer exists — see intr_input_task_ended.
static bool     g_input_held;
static int      g_input_owner  = -1;
static int      g_input_slot   = -1;

// A deadline is the backstop for the case the slot hook cannot see: a claim held
// on the SHELL task by a package call that is then contained mid-read. The shell
// does not end, so its slot is never recycled, and without this the console
// would stay claimed until reboot. A timed claim past its deadline is treated as
// nobody's. Plain intr_input_claim carries NO deadline and never expires, so
// `put` — which is shipped, and holds the console across a whole transfer while
// yielding — is completely unaffected: only a caller that opts in with
// intr_input_claim_for can time out.
static bool     g_input_timed;
static uint32_t g_input_deadline;

// The clock, behind a seam for the same reason the poll is: the pure core has no
// millisecond source of its own, and a host test needs to drive time by hand
// rather than wait for it. Defaults to the real one; never left null, because a
// null clock is a silent "never expires" — the exact shape of failure this is
// meant to remove.
static IntrClockFn g_clock = task_now_ms;
void intr_set_clock(IntrClockFn fn) { g_clock = fn ? fn : task_now_ms; }

// Is the current claim still in force? False when nobody holds it, and false
// when a timed claim has run out — the wrap-safe way, so it still works across
// the ~49-day rollover of a 32-bit millisecond clock.
static bool claim_live(void) {
    if (!g_input_held) return false;
    if (g_input_timed && (int32_t)(g_clock() - g_input_deadline) >= 0) return false;
    return true;
}

static bool claim_take(bool timed, uint32_t ms) {
    int me = task_self();
    // A live claim by somebody else is the only refusal. An expired one, or the
    // owner re-claiming, both fall through and (re)arm it — the owner re-claiming
    // is how a long reader pushes its deadline out.
    if (claim_live() && g_input_owner != me) return false;
    g_input_held     = true;
    g_input_owner    = me;
    g_input_slot     = task_slot_index();
    g_input_timed    = timed;
    if (timed) g_input_deadline = g_clock() + ms;
    return true;
}

bool intr_input_claim(void)              { return claim_take(false, 0); }
bool intr_input_claim_for(uint32_t ms)   { return claim_take(true, ms); }

void intr_input_renew(uint32_t ms) {
    if (g_input_held && g_input_owner == task_self() && g_input_timed)
        g_input_deadline = g_clock() + ms;
}

void intr_input_release(void) {
    if (g_input_held && g_input_owner == task_self()) {
        g_input_held  = false;
        g_input_timed = false;
    }
}

// The reclaim hook. Called from task_slot_recycled, which fires on the spawn and
// reap paths — so most of the time it is some unrelated task ending and there is
// nothing to do. When it IS the console's owner, the claim goes with it, however
// the task ended: a clean exit, a kill, or a fault whose slot was reused.
void intr_input_task_ended(int slot) {
    if (g_input_held && g_input_slot == slot) {
        g_input_held  = false;
        g_input_timed = false;
    }
}

bool intr_input_may_read(int pid) {
    return !claim_live() || g_input_owner == pid;
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
    if (!intr_input_may_read(task_self())) return false;
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
