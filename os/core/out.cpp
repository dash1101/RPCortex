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


void out_clear_error(void) { task_shell()->had_error = false; }
bool out_had_error(void)   { return task_shell()->had_error; }
void out_restore_error(bool had) { if (had) task_shell()->had_error = true; }

// Defined with the rest of the capture below; declared here because emit needs
// them and reads better where it is.
static bool capturing_here(void);
static bool capture_takes_tags(void);
static void cap_append(const char *src, uint32_t len, uint8_t *st);

// The shape v1's _fmt built:
//   <colour>[<white><symbol><colour>] [<white><prefix><colour>] <reset><msg>
static void emit(const char *colour, const char *symbol, const char *p,
                 const char *fmt, va_list ap) {
    // Format once, so the console and the log get identical text and there is
    // no second format string to drift.
    char msg[LOG_LINE_MAX];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    // A package that asked for the output wants ALL of it. A pipe does not —
    // during `ls > f` an error about the listing belongs on the screen, not in
    // the file — so this is the capture's choice and not emit's.
    if (capture_takes_tags()) {
        uint8_t st = 0;
        cap_append("[", 1, &st);
        cap_append(symbol, (uint32_t)strlen(symbol), &st);
        cap_append("] ", 2, &st);
        if (p) {
            cap_append("[", 1, &st);
            cap_append(p, (uint32_t)strlen(p), &st);
            cap_append("] ", 2, &st);
        }
        cap_append(msg, (uint32_t)strlen(msg), &st);
        st = 0;
        cap_append("\n", 1, &st);
    } else {
        OutGuard _o;
        printf("%s[%s%s%s]", colour, C_WHITE, symbol, colour);
        if (p) printf(" %s[%s%s%s]", colour, C_WHITE, p, colour);
        printf(" %s%s\n", C_RESET, msg);
    }

    // Only the things worth keeping. Recording every [@] would fill the ring
    // with routine success and push out the one warning that mattered.
    LogKind kind = LOG_K_INFO;
    if      (symbol[0] == '!') kind = LOG_K_ERR;
    else if (symbol[0] == '?') kind = LOG_K_WARN;
    else return;
    // Nor a usage line. It says someone typed a command without what it needs,
    // which they saw at the time and nobody reading the log later wants. The
    // ring holds 48 lines, and a few of these were enough to push out the boot
    // and crash entries it exists to keep.
    if (!strncmp(msg, "Usage:", 6)) return;

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
    task_shell()->had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!", nullptr, fmt, ap);
    va_end(ap);
}

void out_errp(const char *p, const char *fmt, ...) {
    task_shell()->had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!", p, fmt, ap);
    va_end(ap);
}

void out_fatal(const char *fmt, ...) {
    task_shell()->had_error = true;
    va_list ap; va_start(ap, fmt);
    emit(C_FAIL, "!!!", nullptr, fmt, ap);
    va_end(ap);
}

// --- capture ----------------------------------------------------------------
//
// ONE FRAME PER CAPTURE, OWNED BY A TASK, NESTED PER TASK.
//
// There used to be one capture for the whole device, which broke in two ways.
// Across tasks: a package's fw_shell_run on core 1 held it, the shell's own
// pipeline failed to begin one, ran anyway, then ENDED the package's capture
// and read its length, and handed that many bytes of an uninitialised buffer to
// the next stage or into the redirect file. And on one task, with no race at
// all: fw_shell_run("ls | grep x") holds the capture, so the pipeline inside it
// had none of its own and the same thing happened every time.
//
// So each begin takes a frame of its own, stamped with the task. A task that is
// already capturing stacks a new frame over its old one and gets it back when
// the inner one ends, which is a pipeline inside fw_shell_run. Output goes to
// the TOP frame of the task printing it, and only that task can end it.
#define CAP_FRAMES 8

struct CapFrame {
    char    *buf;
    uint32_t size, len;
    bool     over;
    // Whether the tagged lines come too. A pipe wants the data channel only; a
    // package that asked for a command's output wants what a person would see.
    bool     tags;
    // Where the escape-stripper got to. Kept across calls because out_write is
    // handed whatever the caller had, and a sequence can straddle two writes.
    uint8_t  esc;
    int      owner;
    uint32_t seq;        // larger is newer; the owner's newest frame is its top
    bool     used;
};
static CapFrame g_caps[CAP_FRAMES];
// The owner of a frame nobody holds. Not -1: that is what task_self() says when
// no task is running (at boot, and on a host), and a frame being claimed must
// match nobody at all.
#define CAP_NOBODY (-1000)
static uint32_t g_cap_seq;
// What the last frame a task ended said about truncation, read after the end.
static int      g_last_over_owner = CAP_NOBODY;
static bool     g_last_over;

// This task's top frame, or null. Read without the lock: a task only ever
// changes its own frames, and another task's frame can never carry this task's
// id, so nothing this compares against changes under it.
//
// noinline, deliberately. pico2_w is built at -O3, which unrolled this loop and
// inlined it into every output function: 16 KB of image for a table lookup.
__attribute__((noinline)) static CapFrame *cur(void) {
    int me = task_self();
    CapFrame *top = nullptr;
    for (int i = 0; i < CAP_FRAMES; i++) {
        CapFrame *f = &g_caps[i];
        if (f->used && f->owner == me && (!top || f->seq > top->seq)) top = f;
    }
    return top;
}

bool out_capturing(void) { return cur() != nullptr; }

bool out_capture_overflowed(void) {
    CapFrame *f = cur();
    if (f) return f->over;
    return g_last_over_owner == task_self() && g_last_over;
}

static bool begin(char *buf, uint32_t cap, bool tags) {
    if (!buf || cap == 0) return false;
    // Claimed under the hardware lock: two cores looking for a free frame at the
    // same instant must not both take the same one.
    CapFrame *f = nullptr;
    lock_hw_enter();
    for (int i = 0; i < CAP_FRAMES; i++)
        if (!g_caps[i].used) { f = &g_caps[i]; f->owner = CAP_NOBODY; f->used = true; break; }
    uint32_t seq = ++g_cap_seq;
    lock_hw_exit();
    if (!f) return false;
    f->buf = buf; f->size = cap; f->len = 0; f->over = false;
    f->tags = tags; f->esc = 0;
    f->seq = seq;
    buf[0] = 0;
    f->owner = task_self();          // last: the frame is this task's from here
    return true;
}

bool out_capture_begin(char *buf, uint32_t cap)     { return begin(buf, cap, false); }
bool out_capture_begin_all(char *buf, uint32_t cap) { return begin(buf, cap, true); }

// True only for the task that started the capture.
static bool capturing_here(void) { return cur() != nullptr; }
static bool capture_takes_tags(void) { CapFrame *f = cur(); return f && f->tags; }

// Ends THIS task's newest capture, and only that. A task that is not capturing
// gets 0 and changes nothing - it used to end whichever capture was running,
// which was usually somebody else's.
uint32_t out_capture_end(void) {
    CapFrame *f = cur();
    if (!f) return 0;
    uint32_t n = f->len;
    g_last_over_owner = f->owner;
    g_last_over = f->over;
    lock_hw_enter();
    f->used = false;
    f->owner = CAP_NOBODY;
    lock_hw_exit();
    return n;
}

// A captured buffer is TEXT.
//
// Colour arrives as arguments — out_multi("  %s%2u%s  %s", C_CYAN, n, C_RESET,
// line) — so there is no format string to switch off and nothing upstream knows
// it is being captured. The escapes have to come out here, on the way in.
//
// This is not tidiness. `novad1 setup` read `service list` back to find its own
// stale entries: skip the leading spaces, expect a digit. It found 0x1b, matched
// nothing, removed nothing, and added a fourth copy of a service that was
// already listed three times. `ls | grep` fails in exactly the same way, and had
// been failing quietly for as long as pipes have existed here.
//
// CSI is ESC '[', parameter and intermediate bytes, then one final byte in
// 0x40..0x7e. Anything else after ESC is a two-byte sequence and ends there.
__attribute__((noinline)) static void cap_append(const char *src, uint32_t len, uint8_t *st) {
    CapFrame *f = cur();
    if (!f) return;
    for (uint32_t i = 0; i < len; i++) {
        char c = src[i];
        if (*st == 1)      { *st = (c == '[') ? 2 : 0; continue; }
        if (*st == 2)      { if (c >= 0x40 && c <= 0x7e) *st = 0; continue; }
        if (c == '\033')   { *st = 1; continue; }
        // Leave a byte for the terminator so the buffer is always a valid C
        // string for whatever reads it next.
        if (f->len + 1 >= f->size) { f->over = true; break; }
        f->buf[f->len++] = c;
    }
    f->buf[f->len] = 0;
}

// Flushed, always. stdout is line buffered, so a write with no newline in it
// sits in the buffer until something else happens to emit one — which for
// echoed keystrokes means the characters someone is typing do not appear until
// they press Enter. That is invisible most of the time, because something is
// usually printing; it showed up as soon as the background WiFi join went
// quiet, leaving nothing at all to flush the login prompt's echo.
//
// The callers here are interactive echo, progress lines and `echo` itself —
// none of them hot enough for the extra flush to matter. The TUI does not come
// through this path.
void out_write(const char *data, uint32_t len) {
    if (!capturing_here()) {
        OutGuard _o;
        fwrite(data, 1, len, stdout);
        fflush(stdout);
        return;
    }
    CapFrame *f = cur();
    if (f) cap_append(data, len, &f->esc);
}

void out_console(const char *data, uint32_t len) {
    OutGuard _o;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

void out_multi(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    if (!capturing_here()) {
        { OutGuard _o; vprintf(fmt, ap); printf("\n"); }
        va_end(ap);
        return;
    }
    // Format straight into the remaining space, then take the escapes back out
    // in place. vsnprintf returns the length it WANTED, so a return past the
    // room available is how truncation is detected.
    CapFrame *f = cur();
    uint32_t start = f->len;
    uint32_t room  = f->size - 1 - start;
    int want = vsnprintf(f->buf + start, room + 1, fmt, ap);
    va_end(ap);
    if (want < 0) return;
    uint32_t got = (uint32_t)want > room ? room : (uint32_t)want;
    if ((uint32_t)want > room) f->over = true;

    // Read that region back through the filter. The write index starts level
    // with the read index and only ever falls behind it, since the filter drops
    // bytes and never adds any — so this overlaps safely. A whole line is
    // formatted at once, so no escape can straddle it: a local state, and the
    // shared one is left alone for out_write's benefit.
    uint8_t st = 0;
    f->len = start;
    cap_append(f->buf + start, got, &st);
    st = 0;
    cap_append("\n", 1, &st);
}

// A blank line is data, so it goes where the rest of the data went. It used to
// putchar unconditionally, which put a stray newline on the console during a
// redirect and left the captured text missing the separator that was supposed
// to be there.
void out_blank(void) {
    if (capturing_here()) { uint8_t st = 0; cap_append("\n", 1, &st); return; }
    OutGuard _o;
    putchar('\n');
}

void out_prompt(const char *msg) {
    OutGuard _o;
    printf("%s%s %s••>  %s", C_RESET, msg, C_CYAN, C_RESET);
    fflush(stdout);      // a prompt ends without a newline; see out_write
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
