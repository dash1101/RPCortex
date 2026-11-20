#include "interrupt.h"
#include "task.h"
#include "lock.h"

static volatile bool g_pending;          // domain 0: the console
static IntrPollFn    g_poll;

// The other domains. Four: two remote logins at most, plus room for a login
// that has ended while a job it started is still running.
#define INTR_DOMAINS 4
static struct {
    volatile bool pending;
    bool          used;
    uint8_t       gen;
} g_dom[INTR_DOMAINS];

// A handle is (generation << 8) | (index + 1), so 0 is never a remote domain.
static volatile bool *flag_of(uint16_t d) {
    if (d == 0) return &g_pending;
    uint32_t i = (uint32_t)(d & 0xFF) - 1;
    if (i >= INTR_DOMAINS || !g_dom[i].used || g_dom[i].gen != (uint8_t)(d >> 8)) return nullptr;
    return &g_dom[i].pending;
}

uint16_t intr_domain_new(void) {
    uint16_t h = 0;
    lock_hw_enter();
    for (uint32_t i = 0; i < INTR_DOMAINS; i++) {
        if (g_dom[i].used) continue;
        g_dom[i].used = true;
        g_dom[i].gen++;
        g_dom[i].pending = false;
        h = (uint16_t)(((uint16_t)g_dom[i].gen << 8) | (i + 1));
        break;
    }
    lock_hw_exit();
    return h;
}

void intr_domain_free(uint16_t d) {
    if (!d) return;
    uint32_t i = (uint32_t)(d & 0xFF) - 1;
    lock_hw_enter();
    if (i < INTR_DOMAINS && g_dom[i].used && g_dom[i].gen == (uint8_t)(d >> 8)) {
        g_dom[i].used = false;
        g_dom[i].pending = false;
    }
    lock_hw_exit();
}

void intr_raise_in(uint16_t d)   { if (volatile bool *f = flag_of(d)) *f = true; }
bool intr_pending_in(uint16_t d) { volatile bool *f = flag_of(d); return f && *f; }

static uint16_t my_dom(void) { return task_shell()->intr_dom; }
static const TaskTerm *my_term(void) { return task_shell()->term; }

// Bytes intr_check pulled off the input while looking for Ctrl+C.
//
// They cannot be thrown away. A command runs between the user pressing Enter and
// the prompt coming back, and anything typed in that window — or, far more
// commonly, the remaining lines of a pasted block — arrives while a COMMAND owns
// the input rather than the line editor. Discarding it looks like the shell
// dropping characters, which is a worse bug than the one interrupts fix.
//
// So they are stashed here and the line editor drains this first. A ring rather
// than a growing buffer: type-ahead past a screenful is not worth heap.
//
// WHEN IT FILLS, STOP READING; do not make room by dropping the oldest. The
// bytes left on the wire sit in the USB receive buffer, which holds them and
// pushes back on the host when it fills, so nothing is lost and order is kept.
// Dropping the oldest looked harmless and was not: a pasted script behind a
// command that polls for Ctrl+C (`sleep 1` was enough) lost its first lines and
// ran the rest, which is the worst thing a paste can do. The price of stopping
// is that a Ctrl+C typed behind a full stash of type-ahead is not seen until the
// command ends; that is a narrow case against a common one.
#define STASH_N 512
static char     g_stash[STASH_N];
static uint32_t g_head, g_tail;

static bool stash_full(void) { return (g_head + 1) % STASH_N == g_tail; }

// The caller has checked there is room.
static void stash_put(char c) {
    g_stash[g_head] = c;
    g_head = (g_head + 1) % STASH_N;
}

// The stash is the CONSOLE's type-ahead. A remote session has its own input
// ring and never drains this — a Ctrl+C at a remote prompt used to be able to
// throw away what somebody had typed ahead at the serial port.
int intr_stashed(void) {
    if (my_term()) return -1;
    if (g_tail == g_head) return -1;
    char c = g_stash[g_tail];
    g_tail = (g_tail + 1) % STASH_N;
    return (unsigned char)c;
}

void intr_stash_clear(void) { if (!my_term()) g_head = g_tail = 0; }

// Only the shell's task reads keys, for the reason intr_check gives: anything
// else reading the wire takes keystrokes meant for whoever is at the prompt.
// `bg download` would have. And not while a transfer has claimed the console.
static bool may_read_keys(void);

int intr_getkey(void) {
    // A task attached to a remote terminal reads that terminal and nothing
    // else. This is the one door every key-reading command goes through, so a
    // prompt, a pager or a full-screen app run over SSH reads the right keys.
    if (const TaskTerm *t = my_term()) return t->getkey ? t->getkey(t->ctx) : -1;
    if (!may_read_keys()) return -1;
    int c = intr_stashed();
    if (c >= 0) return c;
    return g_poll ? g_poll() : -1;
}

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

void intr_raise(void)  { if (volatile bool *f = flag_of(my_dom())) *f = true; }
bool intr_pending(void){ volatile bool *f = flag_of(my_dom()); return f && *f; }
void intr_clear(void)  { if (volatile bool *f = flag_of(my_dom())) *f = false; }

static bool may_read_keys(void) {
    int shell = task_shell_pid();
    if (shell && task_self() != shell) return false;
    return intr_input_may_read(task_self());
}

bool intr_check(void) {
    // Every place that polls for Ctrl+C is also a place where it is safe to give
    // up the core — that is exactly what "safe to be interrupted here" means. So
    // the two are the same call, and adding interrupt handling earlier turned
    // out to have scattered the yield points for free.
    task_yield();

    // A task asked to stop reports the same way a Ctrl+C does, so a command
    // written to handle one handles the other with no extra code.
    if (task_should_stop()) return true;

    // This task's own domain: a remote session's Ctrl+C, or the console's.
    if (intr_pending()) return true;
    if (!g_poll) return false;
    // A task attached to a remote terminal never reads the console. Its
    // Ctrl+C arrives through the domain above, raised by the task that reads
    // its connection.
    if (my_term() || my_dom() != 0) return false;
    // Only the shell's own task reads the wire. It is the task that runs every
    // command, a foreground package included, so a command polling for Ctrl+C
    // still sees it. Any other task — a service, a background job, Nova D1's input
    // and display tasks, all of which call this through fw_task_should_stop — would
    // be taking keystrokes meant for whoever is at the prompt, and with nowhere to
    // put them but the stash. A password typed at the login prompt came through
    // with characters missing; a pasted line of 130 bytes lost some in the middle.
    // They still see the flag and a kill request above, which is all a background
    // task wants from this call. Before the shell exists (early boot) there is no
    // one to defer to.
    int shell = task_shell_pid();
    if (shell && task_self() != shell) return false;
    // Somebody else is reading the console as DATA. Taking bytes off it here
    // would be taking them out of their file — see intr_input_claim. The flag
    // and the stop request above still work, which is everything a background
    // task actually needs.
    if (!intr_input_may_read(task_self())) return false;
    // Bounded: a command polling in a loop must not be able to spin in here on
    // a flooded input.
    for (int i = 0; i < 32 && !stash_full(); i++) {
        int c = g_poll();
        if (c < 0) break;
        if (c == 0x03) { g_pending = true; return true; }
        stash_put((char)c);
    }
    return g_pending;
}
