// The shell: a prompt, a line parser, and the built-in commands.
//
// The v1 launchpad in miniature and without the interpreter machinery — no
// _get_scope, no .mpy resolution, no _cmd_cache, none of the apparatus that
// existed to manage Python modules. A line is split into argv, the first token
// is looked up in the registry, and the command runs. That is the whole engine.

#include "shell.h"
#include "sha256.h"
#include "command.h"
#include "kernel.h"
#include "loader.h"
#include "storage.h"
#include "session.h"
#include "registry.h"
#include "users.h"
#include "persist.h"
#include "apps.h"
#include "pkg.h"
#include "history.h"
#include "out.h"
#include "path.h"
#include "fsguard.h"
#include "cmdline.h"
#include "perms.h"
#include "sudo.h"
#include "blackbox.h"
#include "lineedit.h"
#include "interrupt.h"
#include "idle.h"
#include "task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

// Set by the shell around app_main so a registered command is tagged with its
// app (api.cpp), and so a fault names the culprit (fault.cpp).
extern "C" void api_set_current_app(void *owner);
extern "C" volatile const char *g_current_app;

// The filesystem commands live in fs.cpp and carry the cwd.
void fs_register(void);
const char *fs_cwd(void);
void text_register(void);
void sys_register(void);
void user_register(void);
void help_register(void);
void net_register(void);
void power_register(void);
void bt_register(void);
void btaudio_register(void);
// Only on the boards the SD driver is compiled for — see os/CMakeLists.txt.
// Declared inside the guard as well as called inside it, so a build without it
// cannot accidentally link against a function that is not there.
#if defined(RPC_HAS_SD) && RPC_HAS_SD
void sd_register(void);
#endif
void nfc_register(void);
void ibutton_register(void);
void subghz_register(void);
void lora_register(void);
void http_register(void);
void net_autoconnect_report(void);
void diag_register(void);
void settings_register(void);
void editor_register(void);
void rps_register(void);
void update_register(void);
void update_report_boot(void);
void netapps_register(void);
void fetch_register(void);
void ps_register(void);
void log_register(void);
void usbdrive_register(void);
#if defined(RPC_HAS_SSH) && RPC_HAS_SSH
void ssh_register(void);
#endif
void hid_register(void);
void stock_register(void);
void stock_install_once(void);
void jobs_register(void);
void jobs_run_startup(void);
void jobs_start_services(void);
void fsinit_register(void);
void link_register(void);
void fs_layout_check(bool verbose);

// --- line input -------------------------------------------------------------
//
// The editor itself is core/lineedit.cpp — pure, host-tested. What lives here is
// the two things it cannot know: how to read a byte from this device, and what
// the completion candidates are.

// --- idle auto-logout ---------------------------------------------------------
//
// Armed only while the MAIN prompt is waiting (see read_line), so no other prompt
// that shares shell_getch can be logged out from under someone. The limit is read
// again at every prompt, which is why changing the setting needs no re-login.
// The clock is sampled once per keystroke and once per 64 empty polls: task_now_ms
// does a 64-bit division, and the poll is the hottest loop the shell has.
static uint32_t g_idle_limit_ms;    // 0 = not armed
static uint32_t g_idle_last_ms;     // when a key last arrived
static uint32_t g_idle_polls;
static bool     g_idle_fired;       // the limit passed; the prompt loop acts on it

static int shell_getch(void *, uint32_t timeout_us) {
    // Anything intr_check pulled off the wire while a command was running is
    // real input that arrived early — a pasted block, or someone typing ahead.
    // It has to come out before anything new, and in order.
    int stashed = intr_stashed();
    if (stashed >= 0) {
        if (g_idle_limit_ms) g_idle_last_ms = task_now_ms();
        return stashed;
    }

    int c = getchar_timeout_us(timeout_us);
    if (c != PICO_ERROR_TIMEOUT) {
        if (g_idle_limit_ms) g_idle_last_ms = task_now_ms();
    } else if (g_idle_limit_ms && !g_idle_fired && (++g_idle_polls & 63u) == 0 &&
               idle_expired(g_idle_last_ms, task_now_ms(), g_idle_limit_ms)) {
        // An Enter ends the line editor, and the prompt loop sees the flag and
        // discards whatever was typed before logging out.
        g_idle_fired = true;
        return '\n';
    }
    if (c == PICO_ERROR_TIMEOUT) {
        // THE yield point that matters. A prompt waiting for a keystroke is
        // almost all of a device's uptime, and handing the core over here is
        // what turns "one thing at a time" into real concurrency — a service
        // gets to run in the gaps between someone's keystrokes without the
        // shell being written any differently.
        if (timeout_us == 0) task_yield();
        return LE_NO_KEY;
    }
    return c;
}

static void shell_putch(void *, char c) { putchar(c); }
// putchar buffers; a line being edited never ends in a newline, so without
// this nothing typed appears until Enter.
static void shell_flush(void *) { fflush(stdout); }

// Non-blocking single byte, for interrupt.cpp to scan while a COMMAND owns the
// input rather than the line editor.
static int shell_poll_byte(void) {
    int c = getchar_timeout_us(0);
    return (c == PICO_ERROR_TIMEOUT) ? -1 : c;
}

static const char *shell_history(void *, int depth) { return hist_get(depth); }

// Completion. At the start of the line the candidates are commands and aliases;
// anywhere else they are filenames in the directory being typed, which is what
// makes `cat /pk<tab>` useful. Sources are walked by index rather than collected
// into a list, so completing costs no allocation.
struct CompleteWalk {
    const char *prefix;
    uint32_t    want;      // the index the caller asked for
    uint32_t    seen;      // matches passed so far
    char       *out;
    uint32_t    cap;
    bool        found;
    // What was typed up to and including the last '/', put back in front of
    // every candidate. The line editor REPLACES the word with the candidate (so
    // a quoted name completes properly), and this offered the bare name: `ls
    // /ho<Tab>` became `ls home/`, a different path. Empty for a command.
    const char *lead;
    uint32_t    lead_len;
};

// Case-INSENSITIVE matching, and the whole name.
//
// The filesystem is case-sensitive and that is not negotiable — /Packages and
// /packages are different directories. But matching what somebody TYPED is a
// different job from deciding which file they meant, and requiring the capital
// before offering the name helps nobody: they are asking the shell to tell them
// what the name is.
//
// The completion inserted is the real name, so the case-sensitivity of the
// filesystem is preserved exactly. Only the comparison is relaxed.
static bool prefix_matches(const char *name, const char *prefix) {
    for (size_t i = 0; prefix[i]; i++) {
        char a = name[i], b = prefix[i];
        if (!a) return false;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void complete_offer(CompleteWalk *w, const char *name, bool is_dir) {
    if (w->found) return;
    if (!prefix_matches(name, w->prefix)) return;
    if (w->seen++ != w->want) return;

    // A NAME WITH SPACES COMES BACK QUOTED, because the line it is going into
    // is about to be split on spaces. Completing `New Text Document.txt` into
    // three arguments is what made the name look like it had not been completed
    // at all — the whole thing was there, and then taken apart again.
    bool spaced = false;
    for (const char *p = name; *p; p++) if (*p == ' ') { spaced = true; break; }

    int ll = (int)w->lead_len;
    const char *lead = w->lead ? w->lead : "";
    if (spaced) snprintf(w->out, w->cap, "\"%.*s%s%s\"", ll, lead, name, is_dir ? "/" : "");
    else        snprintf(w->out, w->cap, "%.*s%s%s",      ll, lead, name, is_dir ? "/" : "");
    w->found = true;
}

static void complete_file_cb(void *ctx, const char *name, bool is_dir, uint32_t) {
    complete_offer((CompleteWalk *)ctx, name, is_dir);
}

bool shell_complete(void *, const char *prefix, uint32_t word_start,
                           uint32_t index, char *out, uint32_t cap) {
    CompleteWalk w{prefix, index, 0, out, cap, false, "", 0};

    if (word_start == 0) {
        for (uint32_t i = 0; i < cmd_count() && !w.found; i++)
            if (const Command *ci = cmd_at(i)) complete_offer(&w, ci->name, false);
        for (uint32_t i = 0; i < cmd_alias_count() && !w.found; i++)
            complete_offer(&w, cmd_alias_at(i)->name, false);
        return w.found;
    }

    // An argument: complete against the directory the partial path names. The
    // prefix is split at its last '/' so `cat /pkg/gr<tab>` searches /pkg for
    // "gr" rather than searching the cwd for the whole string.
    char dir[128];
    const char *slash = strrchr(prefix, '/');
    if (slash) {
        uint32_t n = (uint32_t)(slash - prefix);
        char head[128];
        if (n == 0) { head[0] = '/'; head[1] = 0; }
        else {
            if (n >= sizeof(head)) n = sizeof(head) - 1;
            memcpy(head, prefix, n); head[n] = 0;
        }
        // `ls ~/Do<Tab>`: the directory is looked up with ~ as home, while
        // what was typed (the lead) keeps its ~.
        if (head[0] == '~' && (head[1] == 0 || head[1] == '/')) {
            char home[USR_NAME_MAX + 8], full[128];
            snprintf(home, sizeof(home), "/home/%s", session_user());
            if (tilde_expand(head, home, full, sizeof(full)) < 0) return false;
            if (!path_resolve(fs_cwd(), full, dir, sizeof(dir))) return false;
        } else if (!path_resolve(fs_cwd(), head, dir, sizeof(dir))) return false;
        w.prefix   = slash + 1;
        w.lead     = prefix;
        w.lead_len = (uint32_t)(slash + 1 - prefix);
    } else {
        snprintf(dir, sizeof(dir), "%s", fs_cwd());
    }
    storage_walk(dir, complete_file_cb, &w);
    return w.found;
}

static bool read_line(const char *prompt, char *buf, size_t max) {
    LineEdit le{};
    le.io.getch     = shell_getch;
    le.io.putch     = shell_putch;
    le.io.flush     = shell_flush;
    le.io.ctx       = nullptr;
    le.complete     = shell_complete;
    le.history      = shell_history;
    le.prompt       = prompt;

    // Auto-logout is off while the device boots straight to a shell (autonomy):
    // logging out would only log straight back in.
    g_idle_fired = false;
    g_idle_limit_ms = reg_get("System.Autonomous", "")[0]
                          ? 0 : idle_timeout_ms(reg_get("Shell.Idle_Logout", "0"));
    g_idle_last_ms = task_now_ms();
    // The editor owns the console while it waits for keystrokes. Without the
    // claim, any background task that polls for a stop request — a package's
    // fw_task_should_stop lands in intr_check — takes bytes off the wire into
    // the stash behind the editor's back. A paste of about 130 bytes or more then
    // lost characters from the middle of the line, and two readers on one stream
    // are a way for characters to arrive out of order. It is released before the
    // line runs, so a command that polls for Ctrl+C is unaffected.
    bool held = intr_input_claim();
    line_edit(&le, buf, (uint32_t)max);
    if (held) intr_input_release();
    g_idle_limit_ms = 0;            // disarm: nothing after the prompt may trip it
    return true;
}

// --- built-in commands ------------------------------------------------------







// STREAMED, not buffered. The size of the file is not the size of the memory it
// needs.
//
// This used to malloc the whole length and hold it until the last byte arrived,
// which capped it at 256 KB and meant a 200 KB transfer wanted 200 KB of heap
// on a device with about 100 KB free at rest. A firmware image could not come
// through here at all, which is the one transfer most worth having.
//
// Now it fills a small buffer and hands each one to the same sink `wget` and
// the USB drive use — so the ceiling is free space rather than free memory, the
// filesystem guard applies as it does to every other write, and a transfer that
// runs out of room is refused rather than half-written.
//
// --- why bytes went missing, and what stops it -------------------------------
//
// Two 300 KB uploads arrived corrupt and neither said so. cc2bc6d made the
// damage loud by hashing what landed. This is the part that stops it happening,
// and it turned out to be two different problems wearing the same symptom.
//
// THE ONE THAT WAS ACTUALLY DOING IT. Every package calls fw_task_should_stop
// in its loop, that is intr_check, and intr_check reads up to 32 bytes off the
// console looking for a Ctrl+C. Those bytes are stashed for the line editor,
// which is right when somebody is typing and ruinous when the console is
// carrying a file: they never reach the loop below. With the Nova D1 screen
// service running, a 300 KB upload lost 735 bytes and timed out; with the same
// file, the same sender and the service stopped, it was byte-exact. The stash
// even hands the evidence back — the last 128 stolen bytes are replayed at the
// prompt when the transfer gives up. So the console is CLAIMED here, and
// intr_check stops reading while it is.
//
// THE ONE THE COMMENT PREDICTED. A flash write is slow and it parks the other
// core; bytes still arriving during it have nowhere to go. USB CDC flow control
// makes that survivable rather than certain — the endpoint is simply not armed
// when the buffer is full, so the host is told to wait — but "survivable" is a
// property of timing, and timing is what changes when a service is added. So
// the device now ACKs each chunk once it is on flash and a sender that waits
// for the ACK can never have more than one chunk in flight.
//
// The ACK is unconditional. An opt-in flag would mean the next sender written
// in a hurry silently gets the old behaviour, which is exactly how this bug
// arrived twice. A sender that ignores it (a person pasting, the old
// rpc-push.sh) behaves as before: one 0x06 per chunk, invisible in a terminal.
// tools/putfile.py is the one that waits.
#define PUT_CHUNK 256
// ASCII ACK. One byte, raw, no newline and no colour — the sender should not
// have to parse anything.
#define PUT_ACK   0x06

// One ACK, on the wire before this returns.
//
// Both flushes are needed and they do different things. fflush pushes the byte
// out of the C library's buffer into the stdio driver; stdio_flush is what
// reaches tud_task and puts it on the bus. Without the second the ACK sits in
// the TX buffer until the USB task next runs, and the USB task does not run
// while this loop is spinning on getchar — so a sender waiting for it would be
// waiting for a byte the device thinks it has already sent.
static void put_ack(void) {
    putchar_raw(PUT_ACK);
    fflush(stdout);
    stdio_flush();
}

static int cmd_put(int argc, char **argv) {
    if (argc < 3) {
        out_multi("Usage: put <name> <len> [sha256]");
        out_multi("  Raw bytes on the console. One 0x06 is sent back per %u bytes",
                  (unsigned)PUT_CHUNK);
        out_multi("  written; wait for it before sending more. tools/putfile.py does.");
        return 1;
    }
    uint32_t len = (uint32_t)strtoul(argv[2], nullptr, 10);

    // The bytes are hashed as they land. Given an expected digest this refuses
    // the file outright; given none it still prints what was stored, so whoever
    // sent it can compare. Kept even now that the two ways bytes went missing
    // are both closed: a digest is the only thing that can say a transfer was
    // right, and everything above it is a reason to believe rather than a
    // proof.
    const char *want = (argc >= 4) ? argv[3] : nullptr;
    if (want && strlen(want) != 64) {
        out_err("A sha256 is 64 hex characters.");
        return 1;
    }

    // Bounded by what the filesystem could hold rather than by a constant. The
    // point is to catch a mistyped length before the console goes quiet waiting
    // for bytes nobody is going to send, not to impose a limit of its own.
    if (len == 0 || len > storage_total_bytes()) {
        out_err("Invalid length.");
        return 1;
    }
    if (!storage_would_fit(len)) {
        out_err("Not enough room: %u bytes free, %u%% used.",
                (unsigned)storage_free_bytes(), (unsigned)storage_used_percent());
        return 1;
    }

    // Claimed BEFORE the sink is opened and before anything is printed, so
    // there is no window where the console is carrying payload and another
    // task is still entitled to read it.
    InputClaim console;
    if (!console.ok) {
        out_err("Another transfer already has the console.");
        return 1;
    }

    // Asked like every other command that writes. It was the one that did not,
    // so an admin could put a file into /etc without sudo. Relative to the
    // root, as it always was.
    {
        char abs[128];
        if (!path_resolve("/", argv[1], abs, sizeof(abs))) { out_err("Path too long."); return 1; }
        if (!fs_allow_change(abs)) return 1;
    }
    void *sink = storage_open_sink(argv[1]);
    if (!sink) { out_err("Could not open %s for writing.", argv[1]); return 1; }

    out_info("Send %u raw bytes now...", (unsigned)len);
    // Pushed out before the first byte is expected. A sender that waits for
    // this line and then starts is the whole reason the claim above has nothing
    // to race with.
    fflush(stdout);
    stdio_flush();

    uint8_t buf[PUT_CHUNK];
    uint32_t got = 0, held = 0;
    bool ok = true;
    Sha256Ctx sha; sha256_init(&sha);

    // The stash goes, rather than being read into the file.
    //
    // Feeding it in was the first attempt and it is subtly wrong: it shifts
    // every chunk boundary by however many bytes were in there, so the device
    // finishes a chunk early, acknowledges it, and leaves the sender's last few
    // bytes on the wire to land at the prompt. A sender waits for the line
    // above before sending anything, which is what makes this empty in the
    // first place; if it is not, the transfer comes up short and says so.
    intr_stash_clear();

    while (got < len && ok) {
        int c;
        // Short waits with a yield between them, rather than one long block:
        // a five-second wait feeds nothing and a slow transfer would trip the
        // watchdog part-way through writing a file.
        uint32_t waited = 0;
        do {
            c = getchar_timeout_us(1000);
            if (c == PICO_ERROR_TIMEOUT) { task_yield(); waited++; }
        } while (c == PICO_ERROR_TIMEOUT && waited < 20000);
        if (c == PICO_ERROR_TIMEOUT) {
            out_err("Timed out after %u of %u bytes.", (unsigned)got, (unsigned)len);
            ok = false;
            break;
        }
        buf[held++] = (uint8_t)c;
        got++;
        if (held == sizeof(buf)) {
            sha256_update(&sha, buf, held);
            ok = storage_sink_write(sink, buf, held);
            held = 0;
            // AFTER the write returns, never before. The ACK means "that chunk
            // is on flash and I am ready for the next", and a sender holding to
            // that can never have bytes in flight during an erase.
            if (ok) put_ack();
        }
    }
    if (ok && held) {
        sha256_update(&sha, buf, held);
        ok = storage_sink_write(sink, buf, held);
        if (ok) put_ack();
    }

    // Close either way: the handle has to be given back, and a failed close is
    // also a failure to report.
    if (!storage_close_sink(sink)) ok = false;
    if (!ok) {
        storage_remove(argv[1]);        // no half-written file left behind
        if (got >= len) out_err("Could not write %s.", argv[1]);
        return 1;
    }
    uint8_t digest[32];
    sha256_final(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);

    if (want && strcmp(hex, want) != 0) {
        // The file goes. A corrupt upload kept on disk is one somebody installs
        // later, and the fault it causes will not mention this transfer.
        storage_remove(argv[1]);
        out_err("What arrived is not what was sent — %s was not kept.", argv[1]);
        out_multi("  expected  %s", want);
        out_multi("  received  %s", hex);
        out_multi("  A sender that waits for the 0x06 after each %u bytes cannot",
                  (unsigned)PUT_CHUNK);
        out_multi("  lose any. tools/putfile.py does; send it again with that.");
        return 1;
    }

    out_ok("Wrote %s (%u bytes).", argv[1], (unsigned)len);
    out_multi("  sha256 %s%s", hex, want ? "  (matches)" : "");
    return 0;
}

// run / exec — the load-run-resident flow lives in apps_launch, shared with
// `pkg install` and boot-time package loading, so there is one code path.
//
// v1's `exec test.py` compiled and ran source ON the device. Nothing in v2 can
// do that: there is no interpreter, and a C++ compiler does not fit on a
// microcontroller. The verb is kept because the muscle memory is real, but a
// source file gets an explanation of the model rather than "no such app" —
// where someone learns how v2 works is the moment their old habit fails.
static const char *source_suffix(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return nullptr;
    static const char *src[] = {".py", ".cpp", ".c", ".cc", ".sh", ".mpy"};
    for (const auto *s : src) if (strcmp(dot, s) == 0) return dot;
    return nullptr;
}

// Does this path end with .rps, case-insensitively?
static bool is_rps(const char *p) {
    size_t n = p ? strlen(p) : 0;
    if (n < 4) return false;
    const char *e = p + n - 4;
    return e[0] == '.' &&
           (e[1] == 'r' || e[1] == 'R') &&
           (e[2] == 'p' || e[2] == 'P') &&
           (e[3] == 's' || e[3] == 'S');
}

static int cmd_run(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: run <app.app | script.rps> [arg]"); return 1; }

    // v1 let `exec`, `run` and a bare path all start a script, so all three do
    // here. A .rps is source for an interpreter rather than a loadable image,
    // and reporting "not an ELF file" for one is technically true and useless.
    if (is_rps(argv[1])) {
        char *rerun[3] = { (char *)"script", argv[1], nullptr };
        const Command *sc = cmd_resolve("script");
        if (sc && sc->fn) return sc->fn(2, rerun);
        out_err("The script interpreter is not available.");
        return 1;
    }

    const char *src = source_suffix(argv[1]);
    if (src) {
        out_err("v2 runs compiled packages, not %s source.", src);
        out_multi("  Build it on the host, then copy the .app over:");
        out_multi("    tools/rpc-push.sh %s /dev/ttyACM0", argv[1]);
        out_multi("    run myapp.app        (or 'pkg install myapp.app' to keep it)");
        return 1;
    }
    int arg = (argc >= 3) ? (int)strtol(argv[2], nullptr, 10) : 0;
    return apps_launch(argv[1], arg, /*quiet*/false) < 0 ? 1 : 0;
}



// bg — run a command as a background task.
//
// The line is copied onto the heap because the caller's buffer is a stack local
// that will be gone before the task first runs. The task frees it on the way
// out, which is the only place that can know it is finished with it.
struct BgJob { char line[128]; bool trusted; };

static int run_segment(char *seg);      // defined with the pipeline machinery

static int bg_entry(void *arg) {
    BgJob *job = (BgJob *)arg;
    // A service: the line came from /etc/services.cfg, which only somebody
    // with sudo could have written, so it runs with sudo (core/sudo.h).
    if (job->trusted) task_shell()->elevated = true;
    int rc = run_segment(job->line);
    free(job);
    return rc;
}

// Start `line` as a background task and hand back its pid, or -1. Quiet: the
// caller says what it wants to. Shared by `bg` and the service supervisor, which
// needs the pid to watch what it started. `trusted` is for the supervisor only.
int shell_bg_spawn(const char *line, bool trusted) {
    if (!line || !line[0]) return -1;
    BgJob *job = (BgJob *)malloc(sizeof(BgJob));
    if (!job) return -1;
    snprintf(job->line, sizeof(job->line), "%s", line);
    job->trusted = trusted;
    // The task is named after the command, the way `ps` has always shown them.
    char name[TASK_NAME_MAX];
    size_t w = strcspn(job->line, " ");
    if (w >= sizeof(name)) w = sizeof(name) - 1;
    memcpy(name, job->line, w);
    name[w] = 0;
    // A whole shell line, so the shell's stack: see TASK_STACK_SHELL.
    int pid = task_spawn(name, job->line, bg_entry, job, TASK_STACK_SHELL, AFFINITY_ANY);
    if (pid < 0) free(job);
    return pid;
}

static int cmd_bg(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: bg <command> [args...]"); return 1; }

    char line[sizeof(((BgJob *)nullptr)->line)];
    line[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(line, " ", sizeof(line) - strlen(line) - 1);
        strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
    }

    int pid = shell_bg_spawn(line, /*trusted*/false);
    if (pid < 0) {
        out_err("Could not start a task — the table is full or memory is short.");
        return 1;
    }
    out_ok("[%d] %s", pid, line);
    return 0;
}

// sudo's own options. `sudo <command>` itself never reaches here: run_one
// takes the word off and runs the command behind it.
//
// -l lists what needs sudo from the command table itself, so it cannot fall
// out of step with what the gate actually asks for.
static int cmd_sudo(int argc, char **argv) {
    const char *opt = argc >= 2 ? argv[1] : "";
    if (!strcmp(opt, "-v")) return sudo_validate();
    if (!strcmp(opt, "-k")) return sudo_forget();
    if (!strcmp(opt, "-l")) {
        const char *who = session_user();
        if (!users_is_admin(who)) {
            out_err("%s is not an admin, so sudo is not available.", who);
            return 1;
        }
        out_info("These need sudo, and %s's password:", who);
        for (uint32_t i = 0; i < cmd_count(); i++) {
            const Command *c = cmd_at(i);
            if (!c || c->owner) continue;
            const char *t = c->level == LEVEL_SYSTEM ? "every form" : perms_forms_text(c->forms);
            if (t) out_multi("  %s%-12s%s %s", C_CYAN, c->name, C_RESET, t);
        }
        out_multi("  %s%-12s%s changing anything in /os or /etc", C_CYAN, "(files)", C_RESET);
        out_multi("  A pass lasts %u minutes in a session. sudo -v asks now, sudo -k forgets.",
                  (unsigned)(SUDO_GRACE_MS / 60000u));
        return 0;
    }
    out_multi("Usage: sudo <command> [args...]   run one that changes the system");
    out_multi("       sudo -v | -k | -l          ask now | forget | list what needs it");
    return 1;
}

static int cmd_reg(int argc, char **argv) {
    if (argc == 1) {
        for (uint32_t i = 0; i < reg_count(); i++) {
            const char *k = reg_key_at(i);
            out_multi("  %s%s%s = %s", C_CYAN, k, C_RESET, reg_get(k, ""));
        }
        // The signed-in user's own, shown SEPARATELY and after.
        //
        // Mixing them would hide the one thing worth knowing: a "User." key
        // that appears in both lists is a personal value shadowing a device
        // default, and somebody reading a flat list has no way to tell which is
        // in force or which file a change would land in.
        if (reg_scope_count()) {
            out_blank();
            out_infop("reg", "%s's own", reg_scope_user());
            for (uint32_t i = 0; i < reg_scope_count(); i++) {
                const char *k = reg_scope_key_at(i);
                out_multi("  %s%s%s = %s", C_CYAN, k, C_RESET, reg_get(k, ""));
            }
        }
        return 0;
    }
    if (!strcmp(argv[1], "get") && argc >= 3) {
        out_multi("%s", reg_get(argv[2], "(unset)"));
        return 0;
    }
    if (!strcmp(argv[1], "set") && argc >= 4) {
        if (!reg_set(argv[2], argv[3])) { out_err("Could not set %s.", argv[2]); return 1; }
        out_ok("%s  =  %s", argv[2], argv[3]);
        return 0;   // written to flash by the shell's post-command save
    }
    out_multi("Usage: reg | reg get <key> | reg set <key> <value>");
    out_multi("  A key beginning 'User.' belongs to whoever is signed in and is");
    out_multi("  kept in their home directory. Everything else is the device's.");
    return 1;
}


// Defined below with the pipeline machinery, since alias expansion happens
// there; declared here so the registration table can name them.
static int cmd_alias(int argc, char **argv);
static int cmd_unalias(int argc, char **argv);
static void shell_load_aliases(void);

void shell_register_builtins(void) {
    intr_set_poll(shell_poll_byte);

    static const Command builtins[] = {
        {"run", "run <app.app | script.rps> [arg]", cmd_run, nullptr, LEVEL_USER, FORMS_RUN},
        {"put", "put <name> <len>  upload bytes", cmd_put, nullptr, LEVEL_ADMIN},
        {"reg", "reg | reg get K | reg set K V",  cmd_reg, nullptr, LEVEL_ADMIN, FORMS_REG},
        {"alias",   "alias name=command",         cmd_alias,   nullptr},
        {"unalias", "unalias <name>",             cmd_unalias, nullptr},
        {"bg",      "bg <command>  run in background", cmd_bg,  nullptr},
        // sudo <command> is taken apart in run_one before dispatch; this entry
        // is for help, Tab completion and sudo's own options (-v, -k, -l).
        {"sudo",    "sudo <command>  run one that changes the system", cmd_sudo, nullptr},
    };
    for (const auto &c : builtins) cmd_register(&c);
    cmd_alias("exec", "run");     // v1's verb; run explains the difference
    // Order is not significant — cmd_register refuses a duplicate name, so a
    // collision fails loudly at boot rather than silently shadowing.
    help_register();        // help + its category pages
    fs_register();          // ls / cd / cat / df / du / tree ...
    text_register();        // grep / wc / sort / uniq / hex ...
    sys_register();         // sysinfo / meminfo / date / pulse ...
    user_register();        // whoami / users / passwd / logout ...
    net_register();         // wifi, net, radio
    session_register();     // autonomy
    power_register();       // sleep
    bt_register();          // bluetooth
    btaudio_register();     // a2dp sink
    nfc_register();         // pn532 reader
    ibutton_register();     // ds1990a reader
    subghz_register();      // CC1101 sub-GHz radio
    lora_register();        // SX1276 LoRa radio
    netapps_register();     // ping / nslookup / ntp
    http_register();        // wget
    fetch_register();       // fetch / neofetch
    ps_register();          // ps / kill — the task manager
    log_register();         // logdump
    usbdrive_register();    // usbdrive — offer the filesystem over USB, or do not
#if defined(RPC_HAS_SSH) && RPC_HAS_SSH
    ssh_register();         // ssh — the client, and `ssh server`
#endif
    hid_register();         // hid / badusb — the USB keyboard
#if defined(RPC_HAS_SD) && RPC_HAS_SD
    sd_register();          // sd — the memory card, browsed at /sd
#endif
    jobs_register();        // startup / task / service / watch
    fsinit_register();      // fscheck
    diag_register();        // diag / compat / inputstat / regreset / pkgdisable / pkgenable
    settings_register();    // settings — the TUI panel
    editor_register();      // edit / nano / vi
    rps_register();         // script — .rps files
    link_register();        // link — framed mode for host tools (LINK.md)
    update_register();      // update — OTA firmware
    apps_register();        // apps / unload for resident packages
    pkg_init();             // ensure /pkg exists
    pkg_register();         // install / remove / list
    stock_register();       // stock — the packages built into the firmware

    shell_load_aliases();   // Alias.* from the registry go live before the prompt

    if (cmd_refused())
        klog(LOG_WARN, "%u command registration(s) refused - check for a duplicate name",
             (unsigned)cmd_refused());
}

// --- pipelines, chaining and redirection ------------------------------------
//
// v1 advertised `|`, `&&`, `||` and `;` in its help, and they carried real
// weight in day-to-day use, so they are here too with the same semantics:
//
//   a ; b     run b regardless
//   a && b    run b only if a succeeded
//   a || b    run b only if a failed
//   a | b     run a with its DATA output captured, hand it to b
//   a > f     write a's data output to f      (a >> f appends)
//
// Only the data channel is captured. During `ls > f` the listing goes to the
// file but an error about it still reaches the console, which is the whole
// reason out_multi and the tagged helpers were split apart.

#define PIPE_BUF 2048          // one stage's captured output

// A piped stage's input, for commands that can read it instead of a file. Kept
// per task (task_shell): lines run on more than the shell's task, and a global
// handed one task's pipe buffer to another task's command.
const char *shell_stdin(void)     { return task_shell()->stdin_buf; }
uint32_t    shell_stdin_len(void) { return task_shell()->stdin_len; }





// Run one command (no connectors, no pipes, no redirect left in it).
// Returns its exit status; a missing command is status 1. *sudo_ran says
// whether it ran after a `sudo` that was let through.
//
// sudo sets task_shell()->elevated for the duration of one command. Not a
// mode: it is put back the moment that command returns, so a sudo cannot leak
// into the next thing typed.
static int run_one(char *seg, bool *sudo_ran) {
    if (sudo_ran) *sudo_ran = false;
    // A user alias maps a name to a whole command line, so it is expanded into
    // the text BEFORE parsing: `alias ll=ls -l` has to contribute two argv
    // entries, which a dispatch-time lookup could not do.
    //
    // One hop only. An alias naming itself is a configuration mistake, and
    // following it would need loop detection to be safe.
    char expanded[192];
    char first[UALIAS_NAME];
    const char *sp = strchr(seg, ' ');
    size_t flen = sp ? (size_t)(sp - seg) : strlen(seg);
    if (flen < sizeof(first)) {
        memcpy(first, seg, flen);
        first[flen] = 0;
        char repl[UALIAS_VALUE];
        if (cmd_ualias_get_copy(first, repl, sizeof(repl))) {
            snprintf(expanded, sizeof(expanded), "%s%s", repl, sp ? sp : "");
            seg = expanded;
        }
    }

    char *raw[16];
    int n = cmdline_split_args(seg, raw, 16);
    if (n == 0) return 0;

    // sudo <command>: prove it is you, then run exactly this one command with
    // sudo (core/sudo.h). The shift is done with an index rather than by moving
    // the array pointer, so argv stays a real array and the command sees a
    // normal argv[0]. `sudo -v`, `-k` and `-l` are sudo's own.
    char **argv = raw;
    int argc = n;
    bool prefixed = false;
    if (!strcmp(argv[0], "sudo")) {
        if (argc < 2 || argv[1][0] == '-') return cmd_sudo(argc, argv);
        prefixed = true;
        argv = raw + 1;
        argc = n - 1;
    }

    // A copy, not a pointer into the table: see cmd_resolve_copy.
    CommandRef ref;
    const Command *c = cmd_resolve_copy(argv[0], &ref) ? &ref.cmd : nullptr;
    if (!c) {
        // A bare path to a script runs it, as it did on v1. Checked before the
        // error so `/os/setup.rps` works the way people already expect.
        if (is_rps(argv[0])) {
            const Command *sc = cmd_resolve("script");
            if (sc && sc->fn) {
                // `sudo /path/x.rps` is a script run with sudo, like `sudo script`.
                bool el = false;
                if (!sudo_gate(LEVEL_USER, FORMS_NONE, argc, argv, prefixed, &el)) return 1;
                char *rerun[3] = { (char *)"script", argv[0], nullptr };
                bool was = task_shell()->elevated;
                task_shell()->elevated = el;
                int r = sc->fn(2, rerun);
                task_shell()->elevated = was;
                return r;
            }
        }
        out_err("'%s' is not a command or executable file.", argv[0]);
        return 1;
    }

    // Who may run it, with these arguments, and whether that takes sudo. A
    // package's command has no say in its own level: only the built-ins carry
    // one, so a package cannot mark itself as anything.
    bool elevated = false;
    if (!sudo_gate(c->level, c->owner ? FORMS_NONE : c->forms, argc, argv, prefixed, &elevated))
        return 1;
    // A redirect after a sudo'd command is that command's output going where it
    // was asked to: run_segment writes it with the same sudo.
    if (sudo_ran) *sudo_ran = prefixed;

    bool saved = task_shell()->elevated;
    task_shell()->elevated = elevated;

    // Phase markers around the call itself. A package command jumps into code
    // the loader put in RAM, and if THAT is what fails, no checkpoint inside the
    // package can ever be reached — so the firmware records the attempt from
    // outside. "entered" appearing without "returned" means the package's own
    // code is where it stopped; "calling" without "entered" means it never got
    // there at all.
    char phase[40];
    if (c->owner) {
        snprintf(phase, sizeof(phase), "calling package '%s'", c->name);
        bb_note_phase(phase);
        // Name the package for the duration of the CALL too. It was only set
        // around app_main, so a fault in a registered command reported
        // "in firmware" — which is exactly backwards.
        g_current_app = c->name;
        // And protect it for the duration of the call, the same way app_main is
        // protected. A resident package spends nearly all of its life here
        // rather than in app_main, so covering only the load would have been
        // covering the part that barely runs — see app_run_owner.
    }
    // A package's command runs the way its app_main did: on a stack of its own,
    // sandboxed where the board allows it. A built-in is called directly.
    //
    // A package's command NEVER falls back to a direct call. app_run_owner says
    // false only when no loaded package owns it any more, which means it was
    // unloaded between being looked up and being run, and its code is freed:
    // calling it directly would jump into the heap, privileged and unsandboxed.
    int rc = 0;
    if (!c->owner) {
        rc = c->fn(argc, argv);
    } else if (!app_run_owner(c->owner, c->fn, argc, argv, &rc)) {
        out_err("'%s' was unloaded before it could run.", c->name);
        rc = 1;
    }
    g_current_app = nullptr;
    if (c->owner) {
        snprintf(phase, sizeof(phase), "'%s' returned", c->name);
        bb_note_phase(phase);
    }

    task_shell()->elevated = saved;
    return rc;
}

// alias / unalias. v1's syntax: `alias name=command line`, bare `alias` lists.
// Persisted as Alias.<name> in the registry, so they survive a reboot the way
// v1's aliases.cfg did.
static int cmd_alias(int argc, char **argv) {
    if (argc < 2) {
        if (!cmd_ualias_count()) {
            out_info("No aliases defined. Usage: alias name=command");
            return 0;
        }
        for (uint32_t i = 0; i < cmd_ualias_count(); i++) {
            const char *nm = cmd_ualias_name_at(i);
            out_multi("  %s%s%s = %s", C_CYAN, nm, C_RESET, cmd_ualias_get(nm));
        }
        return 0;
    }

    // The whole rest of the line, so `alias ll=ls -l` keeps its argument.
    char line[192];
    line[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(line, " ", sizeof(line) - strlen(line) - 1);
        strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
    }

    char *eq = strchr(line, '=');
    if (!eq) { out_warn("Usage: alias name=command   (or bare 'alias' to list)"); return 1; }
    *eq = 0;
    char *name  = cmdline_trim(line);
    char *value = cmdline_trim(eq + 1);
    if (!*name)  { out_warn("Alias name cannot be empty."); return 1; }
    if (!*value) { out_warn("Alias value cannot be empty."); return 1; }
    // An alias is expanded in EVERY user's shell, before any command is looked
    // up, so defining one changes what the next person types does — an
    // ordinary account (or a remote one) could otherwise put `exit=<anything>`
    // in front of the admin. That is the definition of an admin command here.
    if (!users_is_admin(session_user())) {
        out_err("Defining an alias needs an admin account: it applies to everyone's shell.");
        return 1;
    }
    if (cmd_find(name)) { out_warn("Cannot shadow the built-in command '%s'.", name); return 1; }
    // Nor a built-in alias (exit, id, exec...), which user aliases would win over.
    if (cmd_resolve(name)) { out_warn("Cannot shadow '%s', which is already a command.", name); return 1; }

    if (!cmd_ualias_set(name, value)) { out_err("Could not define '%s'.", name); return 1; }
    char key[REG_KEY_MAX];
    snprintf(key, sizeof(key), "Alias.%s", name);
    reg_set(key, value);
    out_ok("alias %s = %s", name, value);
    return 0;
}

static int cmd_unalias(int argc, char **argv) {
    if (argc < 2) { out_warn("Usage: unalias <name>"); return 1; }
    if (!users_is_admin(session_user())) {
        out_err("Removing an alias needs an admin account: it applies to everyone's shell.");
        return 1;
    }
    if (!cmd_ualias_remove(argv[1])) { out_warn("No alias named '%s'.", argv[1]); return 1; }
    char key[REG_KEY_MAX];
    snprintf(key, sizeof(key), "Alias.%s", argv[1]);
    reg_set(key, "");
    out_ok("Alias '%s' removed.", argv[1]);
    return 0;
}

// Restore saved aliases at boot from the registry's Alias.* keys.
static void shell_load_aliases(void) {
    for (uint32_t i = 0; i < reg_count(); i++) {
        const char *k = reg_key_at(i);
        if (strncmp(k, "Alias.", 6) != 0) continue;
        const char *v = reg_get(k, "");
        if (v && v[0]) cmd_ualias_set(k + 6, v);
    }
}

// Run one segment: its pipeline, then its redirect if it has one.
static int run_segment(char *seg) {
    bool append = false;
    char *outfile = cmdline_split_redirect(seg, &append);
    // The redirect is split off the whole segment before the pipeline is, so a
    // malformed `a > f | b` would otherwise create a file literally named
    // "f | b". Refuse rather than write it: a garbage filename on flash is
    // harder to notice, and harder to delete, than an error.
    if (outfile && (strchr(outfile, '|') || strchr(outfile, ' '))) {
        out_err("A redirect takes one filename; put it at the end of the line.");
        return 1;
    }

    // Walk the pipeline left to right. Every stage but the last is captured and
    // handed to the next; the last prints normally unless it is redirected.
    char *stage = seg;
    char *buf_a = nullptr, *buf_b = nullptr;
    int status = 0;

    // Saved and put back: a stage's command may run a line of its own through
    // fw_shell_run on this same task, and that inner pipeline must not leave the
    // outer stage reading nothing.
    TaskShell *ts = task_shell();
    const char *outer_in = ts->stdin_buf;
    uint32_t    outer_len = ts->stdin_len;
    ts->stdin_buf = nullptr; ts->stdin_len = 0;

    while (stage) {
        char *pipe = cmdline_next_pipe(stage);
        bool last = (pipe == nullptr);
        if (pipe) *pipe = 0;

        bool capturing = !last || outfile;
        if (capturing) {
            if (!buf_a) buf_a = (char *)malloc(PIPE_BUF);
            if (!buf_a) { out_err("Not enough memory for a pipeline."); status = 1; break; }
            // A capture that did not start must not be ended or read: ending it
            // would end somebody else's, and buf_a holds nothing yet.
            if (!out_capture_begin(buf_a, PIPE_BUF)) {
                out_err("Too many pipes and redirects running at once to start another.");
                status = 1;
                break;
            }
        }

        bool sudo_stage = false;
        status = run_one(cmdline_trim(stage), &sudo_stage);

        if (capturing) {
            uint32_t n = out_capture_end();
            if (out_capture_overflowed())
                out_warn("Output truncated at %u bytes.", (unsigned)(PIPE_BUF - 1));
            if (last && outfile) {
                char path[128];
                path_resolve(fs_cwd(), outfile, path, sizeof(path));
                // `sudo echo x > /etc/f` writes /etc/f with that sudo. (A shell
                // on a computer would not, and everybody trips on it once.)
                bool was = ts->elevated;
                if (sudo_stage) ts->elevated = true;
                bool ok = fs_allow_change(path) &&
                          (append ? storage_append_file(path, (const uint8_t *)buf_a, n)
                                  : storage_write_file(path, (const uint8_t *)buf_a, n));
                if (!ok) { if (fs_may_change(path)) out_err("Could not write %s.", path); status = 1; }
                ts->elevated = was;
            } else {
                // Hand this stage's output to the next as its input. The buffers
                // swap rather than copy: the next stage captures into the other
                // one while reading from this.
                if (!buf_b) buf_b = (char *)malloc(PIPE_BUF);
                if (!buf_b) { out_err("Not enough memory for a pipeline."); status = 1; break; }
                char *t = buf_a; buf_a = buf_b; buf_b = t;
                ts->stdin_buf = buf_b; ts->stdin_len = n;
            }
        }

        stage = pipe ? pipe + 1 : nullptr;
    }

    ts->stdin_buf = outer_in; ts->stdin_len = outer_len;
    free(buf_a);
    free(buf_b);
    return status;
}

// Run one line through the full parser. Exposed so startup, the scheduler and
// watch get pipes, chaining and redirection for free instead of each growing
// its own half-parser.
static int run_line(char *line, bool top);

int shell_run_line_now(char *line) {
    bb_note_command(line);
    // A line run from inside a command on this same task (fw_shell_run) clears
    // the error flag to judge itself; the outer command's error must survive it.
    bool outer_error = out_had_error();
    out_clear_error();
    // The whole line, chaining included. This called run_segment, which knows
    // pipes and redirects but not ; && ||, so every caller but the prompt
    // (scripts, bg, services, the scheduler, fw_shell_run, link) ran
    // `a ; b` as one command named a with arguments "; b".
    int rc = run_line(line, /*top*/false);
    if (rc == 0 && out_had_error()) rc = 1;
    out_restore_error(outer_error);
    persist_save_dirty();
    return rc;
}

// Execute a whole input line: ; sequencing, && / || conditionals, | pipes.
// Returns the status of the last segment that ran.
//
// `top` is the prompt. There, a Ctrl+C left over from before is cleared ahead of
// each segment, and one that interrupts a segment is reported and consumed.
// Nested (a script line, bg, a service, fw_shell_run, link's EXEC), the flag is
// not this function's to clear: the caller is interruptible too and has to see
// it, so the chain simply stops.
static int run_line(char *line, bool top) {
    Connector kind = CON_FIRST;
    int  last_status = 0;
    char *rest = line;

    while (rest && *rest) {
        Connector next_kind = CON_SEQ;
        int skip = 0;
        char *conn = cmdline_next_connector(rest, &next_kind, &skip);
        char *seg  = rest;
        if (conn) { *conn = 0; rest = conn + skip; }
        else      { rest = nullptr; }

        seg = cmdline_trim(seg);
        bool skip_it = (kind == CON_AND && last_status != 0) ||
                       (kind == CON_OR  && last_status == 0);
        if (*seg && !skip_it) {
            out_clear_error();
            if (top) intr_clear();     // a stray Ctrl+C at the prompt must not kill this
            else if (intr_pending()) return 130;
            // ~ is expanded here, a segment at a time and before its pipes and
            // redirect are split, so `cat ~/a | grep x > ~/b` works and so do
            // scripts and link. On the heap and only when there is a ~: an
            // expanded line can be longer than the buffer it came in.
            char *tx = nullptr;
            if (strchr(seg, '~')) {
                char home[USR_NAME_MAX + 8];
                snprintf(home, sizeof(home), "/home/%s", session_user());
                size_t need = tilde_expanded_len(seg, home) + 1;
                tx = (char *)malloc(need);
                if (tx && tilde_expand(seg, home, tx, need) >= 0) seg = tx;
            }
            last_status = run_segment(seg);
            free(tx);
            // A command that printed an error but returned 0 still failed, as
            // far as && is concerned. v1's had_error() served the same purpose.
            if (last_status == 0 && out_had_error()) last_status = 1;
            if (intr_pending()) {
                if (!top) return 130;       // the caller's to report and clear
                out_warn("Interrupted.");
                last_status = 130;          // 128 + SIGINT, the usual convention
                intr_clear();
            }
            persist_save_dirty();
        }
        kind = next_kind;
    }
    return last_status;
}

// A typed line from a terminal other than the console — a remote login — run
// exactly as the console runs one: connectors, pipes, redirects, the
// Interrupted message and status 130 on Ctrl+C. shell_run_line_now is the
// nested runner (scripts, bg, fw_shell_run, link): it leaves a Ctrl+C for
// its caller to see instead of reporting it.
int shell_run_input(char *line) {
    bb_note_command(line);
    int rc = run_line(line, /*top*/true);
    bb_note_command("");
    return rc;
}

// v1's prompt, colour for colour: cyan user, grey @host, blue path.
void shell_prompt_text(char *out, unsigned cap) {
    snprintf(out, cap, "%s%s%s@%s%s:%s%s%s%s>%s ",
             C_CYAN, session_user(), C_GRAY,
             reg_get("System.Device_ID", "vela"), C_RESET,
             C_BLUE, fs_cwd(), C_RESET, C_CYAN, C_RESET);
}

void shell_run(void) {
    char line[128];

    // So the watchdog and task_kill know which task must never be terminated.
    // Ending the shell leaves a device nobody can type at.
    task_mark_shell();

    while (true) {
        // A stop request is aimed at a COMMAND, but it lands on the task that
        // ran it — and this task runs every command. Left set it made
        // task_should_stop true forever, and since intr_check folds that in,
        // every interruptible loop afterwards gave up instantly: WiFi scans
        // found nothing, pings timed out at once, and the device looked broken
        // while the shell carried on fine. Reaching the prompt means whatever
        // was asked to stop has stopped.
        task_clear_stop();

        // Report a background join here rather than from its own task: printing
        // from there lands in the middle of whatever is being typed, and this
        // is the one place the shell is known to be idle.
        net_autoconnect_report();

        char prompt[80];
        shell_prompt_text(prompt, sizeof(prompt));
        if (!read_line(prompt, line, sizeof(line))) continue;
        if (g_idle_fired) {
            // Whatever was half-typed is discarded, not run and not remembered.
            g_idle_fired = false;
            out_blank();
            out_infop("session", "Logged out after %s minute%s without input.",
                      reg_get("Shell.Idle_Logout", "0"),
                      strcmp(reg_get("Shell.Idle_Logout", "0"), "1") == 0 ? "" : "s");
            session_logout();
            session_boot();          // back to the login prompt, as `logout` does
            continue;
        }
        hist_add(line);          // record before the parser chops it up
        bb_note_command(line);   // so a hang can name the command, not just the task
        run_line(line, /*top*/true);
        bb_note_command("");     // finished cleanly; do not blame it for the next hang
    }
}
