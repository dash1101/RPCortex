#include "out.h"
#include "lock.h"
#include "task.h"
#include "logring.h"

// Output is shared hardware, and stdio is not reentrant.
//
// newlib's stdout carries a buffer pointer and a count. Two tasks inside
// printf at once corrupt them, and the result is a hard fault at an address
// that is not code — 696672fe in one report, which is ASCII, because it came
// from a mangled buffer pointer rather than anywhere real.
//
// It happened the moment a background WiFi join printed "Scanning..." while
// the shell was echoing a password. Any background task can print, so the
// answer belongs here rather than in a rule about who is allowed to.
//
// Output is serialised, but NOT with the yielding lock the rest of the OS uses.
// Recursive, because the tagged helpers below are built from each other.
//
// lock_acquire yields when contended, which would make every print a scheduling
// point. That is wrong here for two reasons, both of which have bitten:
//
//   * The watchdog reports a stalled task from INSIDE reschedule. A print that
//     yields there re-enters the scheduler from the middle of itself.
//   * The stack-overflow reporter runs on a task already known to be corrupt.
//     Yielding back into the scheduler is the last thing it should do.
//
// Spinning is safe here in a way it is not for the filesystem lock, because
// nothing inside a single out_* call ever yields. On one core the holder
// therefore runs to completion before anything else can ask for it, so the only
// possible contention is the other core, and only for the length of one write.
//
// Keyed on CORE rather than task for the same reason: two tasks on one core
// cannot both be inside out_*. Preemption does not break that — it redirects a
// task to task_forced_exit rather than suspending it, and crit_enter below is
// what makes it defer rather than strand the lock held.
static volatile uint32_t g_out_owner;    // core + 1; 0 means free
static volatile uint32_t g_out_depth;    // nested out_* calls on that core

// Once the system is on its way down, stop synchronising altogether. A fault
// reporter that spins on a lock held by a core that has already died would hang
// instead of saying what happened, and interleaved output beats none.
static volatile bool g_out_panic;
void out_panic_mode(void) { g_out_panic = true; }

struct OutGuard {
    bool held;
    OutGuard() {
        held = false;
        if (g_out_panic) return;
        // BEFORE the acquire, not after. Preemption landing between taking
        // ownership and marking the task busy redirects it to task_forced_exit,
        // which never runs this destructor — so the lock would stay owned by a
        // task that no longer exists and the other core would spin on it for
        // good. The window is small and it is the whole failure.
        crit_enter();
        uint32_t me = lock_hw_core() + 1;
        while (true) {
            lock_hw_enter();
            if (g_out_owner == 0 || g_out_owner == me) {
                g_out_owner = me;
                g_out_depth++;
                lock_hw_exit();
                held = true;
                return;
            }
            lock_hw_exit();
            if (g_out_panic) { crit_leave(); return; }   // the holder died mid-write
        }
    }
    ~OutGuard() {
        if (!held) return;
        lock_hw_enter();
        if (g_out_depth && --g_out_depth == 0) g_out_owner = 0;
        lock_hw_exit();
        crit_leave();    // busy until it is fully released, not until nearly
    }
};

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static bool g_had_error;

void out_clear_error(void) { g_had_error = false; }
bool out_had_error(void)   { return g_had_error; }

// The shape v1's _fmt built:
//   <colour>[<white><symbol><colour>] [<white><prefix><colour>] <reset><msg>
static void emit(const char *colour, const char *symbol, const char *p,
                 const char *fmt, va_list ap) {
    // Format once, so the console and the log get identical text and there is
    // no second format string to drift.
    char msg[LOG_LINE_MAX];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    OutGuard _o;

    printf("%s[%s%s%s]", colour, C_WHITE, symbol, colour);
    if (p) printf(" %s[%s%s%s]", colour, C_WHITE, p, colour);
    printf(" %s%s\n", C_RESET, msg);

    // Only the things worth keeping. Recording every [@] would fill the ring
    // with routine success and push out the one warning that mattered.
    LogKind kind = LOG_K_INFO;
    if      (symbol[0] == '!') kind = LOG_K_ERR;
    else if (symbol[0] == '?') kind = LOG_K_WARN;
    else return;

    if (p) log_addf(kind, "[%s] %s", p, msg);
    else   log_add(kind, msg);
}

#define TAGGED(name, colour, symbol)                       \
    void name(const char *fmt, ...) {                      \
        va_list ap; va_start(ap, fmt);                     \
        emit(colour, symbol, nullptr, fmt, ap);            \
        va_end(ap);                                        \
    }
#define TAGGED_P(name, colour, symbol)                     \
    void name(const char *p, const char *fmt, ...) {       \
        va_list ap; va_start(ap, fmt);                     \
        emit(colour, symbol, p, fmt, ap);                  \
        va_end(ap);                                        \
    }

TAGGED  (out_ok,    C_CYAN,   "@")
TAGGED  (out_info,  C_HEADER, ":")
TAGGED  (out_warn,  C_WARN,   "?")
TAGGED_P(out_okp,   C_CYAN,   "@")
TAGGED_P(out_infop, C_HEADER, ":")
TAGGED_P(out_warnp, C_WARN,   "?")

// The two that set the error flag get explicit bodies rather than the macro.
void out_err(const char *fmt, ...) {
    g_had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!", nullptr, fmt, ap);
    va_end(ap);
}

void out_errp(const char *p, const char *fmt, ...) {
    g_had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!", p, fmt, ap);
    va_end(ap);
}

void out_fatal(const char *fmt, ...) {
    g_had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!!!", nullptr, fmt, ap);
    va_end(ap);
}

// --- capture ----------------------------------------------------------------

static char    *g_cap;
static uint32_t g_cap_size, g_cap_len;
static bool     g_cap_over;
// Whose pipeline the capture belongs to. A capture is per-COMMAND, so a
// background task printing during `ls > f` must go to the console rather than
// into someone else's redirect — its output has nothing to do with that file.
static int      g_cap_owner;

bool out_capturing(void)           { return g_cap != nullptr; }
bool out_capture_overflowed(void)  { return g_cap_over; }

bool out_capture_begin(char *buf, uint32_t cap) {
    if (g_cap || !buf || cap == 0) return false;
    g_cap = buf; g_cap_size = cap; g_cap_len = 0; g_cap_over = false;
    g_cap_owner = task_self();
    g_cap[0] = 0;
    return true;
}

// True only for the task that started the capture.
static bool capturing_here(void) { return g_cap && g_cap_owner == task_self(); }

uint32_t out_capture_end(void) {
    uint32_t n = g_cap_len;
    g_cap = nullptr;
    return n;
}

void out_write(const char *data, uint32_t len) {
    if (!capturing_here()) { OutGuard _o; fwrite(data, 1, len, stdout); return; }
    // Leave a byte for the terminator so the buffer is always a valid C string
    // for whatever reads it next.
    uint32_t room = g_cap_size - 1 - g_cap_len;
    if (len > room) { len = room; g_cap_over = true; }
    memcpy(g_cap + g_cap_len, data, len);
    g_cap_len += len;
    g_cap[g_cap_len] = 0;
}

void out_multi(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (!capturing_here()) {
        { OutGuard _o; vprintf(fmt, ap); printf("\n"); }
        va_end(ap);
        return;
    }
    // Format straight into the remaining space. vsnprintf returns the length it
    // WANTED, so a return past the room available is how truncation is detected.
    uint32_t room = g_cap_size - 1 - g_cap_len;
    int want = vsnprintf(g_cap + g_cap_len, room + 1, fmt, ap);
    va_end(ap);
    if (want < 0) return;
    if ((uint32_t)want > room) { g_cap_len = g_cap_size - 1; g_cap_over = true; }
    else                        g_cap_len += (uint32_t)want;
    out_write("\n", 1);
}

void out_blank(void) { OutGuard _o; putchar('\n'); }

void out_prompt(const char *msg) {
    OutGuard _o;
    printf("%s%s %s••>  %s", C_RESET, msg, C_CYAN, C_RESET);
}

// Copy `s` and append spaces until it is `width` visible characters wide,
// skipping ANSI CSI sequences ("\033[" ... final byte in @-~) when counting.
void out_pad(const char *s, int width, char *dst, int cap) {
    int vis = 0, o = 0;
    for (const char *p = s; *p && o + 1 < cap; ) {
        if (p[0] == '\033' && p[1] == '[') {
            while (*p && o + 1 < cap) {                 // copy the escape, count none
                char c = *p++;
                dst[o++] = c;
                if (c >= '@' && c <= '~' && c != '[') break;
            }
            continue;
        }
        // UTF-8 continuation bytes are part of the character before them.
        if (((unsigned char)*p & 0xC0) != 0x80) vis++;
        dst[o++] = *p++;
    }
    while (vis < width && o + 1 < cap) { dst[o++] = ' '; vis++; }
    dst[o] = 0;
}

// --- progress ---------------------------------------------------------------

void out_flush(void) {
    if (capturing_here()) return;      // a capture is a buffer, not a terminal
    OutGuard _o;
    fflush(stdout);
}

#define BAR_CELLS 20

void out_progress(const char *label, uint64_t done, uint64_t total) {
    if (capturing_here()) return;      // a bar down a pipe is noise, not data

    char line[120];
    int n = 0;
    line[n++] = '\r';
    n += snprintf(line + n, sizeof(line) - n, "  %-12s [", label ? label : "");

    // Percent first, so the bar and the number can never disagree.
    unsigned pct = total ? (unsigned)((done * 100) / total) : 0;
    if (pct > 100) pct = 100;
    unsigned filled = (pct * BAR_CELLS) / 100;
    for (unsigned i = 0; i < BAR_CELLS && n + 1 < (int)sizeof(line); i++)
        line[n++] = (i < filled) ? '#' : '-';

    if (total) {
        // KB once it is past a kilobyte; bytes below that, since "0/0 KB" tells
        // nobody anything.
        if (total >= 1024)
            n += snprintf(line + n, sizeof(line) - n, "] %3u%%   %lu/%lu KB   ",
                          pct, (unsigned long)(done / 1024), (unsigned long)(total / 1024));
        else
            n += snprintf(line + n, sizeof(line) - n, "] %3u%%   %lu/%lu B   ",
                          pct, (unsigned long)done, (unsigned long)total);
    } else {
        n += snprintf(line + n, sizeof(line) - n, "] %lu KB   ",
                      (unsigned long)(done / 1024));
    }
    out_write(line, (uint32_t)n);
    out_flush();
}

void out_spinner(const char *label, uint32_t elapsed_ms) {
    if (capturing_here()) return;
    // ASCII, because a terminal that disagrees about a character set turns a
    // spinner into line noise — and this is the first thing anyone sees.
    static const char frames[] = "|/-\\";
    char line[96];
    int n = snprintf(line, sizeof(line), "\r  %-12s %c  %lus   ",
                     label ? label : "",
                     frames[(elapsed_ms / 150) & 3],
                     (unsigned long)(elapsed_ms / 1000));
    out_write(line, (uint32_t)n);
    out_flush();
}

void out_progress_done(void) {
    if (capturing_here()) return;
    // Overwrite the bar rather than leaving it above: a finished operation
    // reports its result, and a stale bar next to it just invites doubt.
    char clear[100];
    int n = snprintf(clear, sizeof(clear), "\r%*s\r", 78, "");
    out_write(clear, (uint32_t)n);
    out_flush();
}
