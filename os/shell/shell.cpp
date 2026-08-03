// The shell: a prompt, a line parser, and the built-in commands.
//
// The v1 launchpad in miniature and without the interpreter machinery — no
// _get_scope, no .mpy resolution, no _cmd_cache, none of the apparatus that
// existed to manage Python modules. A line is split into argv, the first token
// is looked up in the registry, and the command runs. That is the whole engine.

#include "shell.h"
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
#include "cmdline.h"
#include "lineedit.h"
#include "interrupt.h"

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
void netapps_register(void);

// --- line input -------------------------------------------------------------
//
// The editor itself is core/lineedit.cpp — pure, host-tested. What lives here is
// the two things it cannot know: how to read a byte from this device, and what
// the completion candidates are.

static int shell_getch(void *, uint32_t timeout_us) {
    int c = getchar_timeout_us(timeout_us);
    if (c == PICO_ERROR_TIMEOUT) {
        if (timeout_us == 0) sleep_ms(2);   // idle poll: do not spin the core
        return LE_NO_KEY;
    }
    return c;
}

static void shell_putch(void *, char c) { putchar(c); }

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
};

static void complete_offer(CompleteWalk *w, const char *name, bool is_dir) {
    if (w->found) return;
    if (strncmp(name, w->prefix, strlen(w->prefix)) != 0) return;
    if (w->seen++ != w->want) return;
    snprintf(w->out, w->cap, "%s%s", name, is_dir ? "/" : "");
    w->found = true;
}

static void complete_file_cb(void *ctx, const char *name, bool is_dir, uint32_t) {
    complete_offer((CompleteWalk *)ctx, name, is_dir);
}

static bool shell_complete(void *, const char *prefix, uint32_t word_start,
                           uint32_t index, char *out, uint32_t cap) {
    CompleteWalk w{prefix, index, 0, out, cap, false};

    if (word_start == 0) {
        for (uint32_t i = 0; i < cmd_count() && !w.found; i++)
            complete_offer(&w, cmd_at(i)->name, false);
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
        path_resolve(fs_cwd(), head, dir, sizeof(dir));
        w.prefix = slash + 1;
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
    le.io.ctx       = nullptr;
    le.complete     = shell_complete;
    le.history      = shell_history;
    le.prompt       = prompt;
    line_edit(&le, buf, (uint32_t)max);
    return true;
}

// --- built-in commands ------------------------------------------------------







static int cmd_put(int argc, char **argv) {
    if (argc < 3) { out_multi("Usage: put <name> <len>"); return 1; }
    uint32_t len = (uint32_t)strtoul(argv[2], nullptr, 10);
    if (len == 0 || len > 256 * 1024) { out_err("Invalid length."); return 1; }
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) { out_err("Not enough memory for %u bytes.", (unsigned)len); return 1; }
    out_info("Send %u raw bytes now...", (unsigned)len);
    for (uint32_t i = 0; i < len; i++) {
        int c;
        do { c = getchar_timeout_us(5000000); } while (c == PICO_ERROR_TIMEOUT);
        buf[i] = (uint8_t)c;
    }
    bool ok = storage_write_file(argv[1], buf, len);
    free(buf);
    if (ok) out_ok("Wrote %s (%u bytes).", argv[1], (unsigned)len);
    else    out_err("Could not write %s.", argv[1]);
    return ok ? 0 : 1;
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

static int cmd_run(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: run <app.app> [arg]"); return 1; }

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



static int cmd_reg(int argc, char **argv) {
    if (argc == 1) {
        for (uint32_t i = 0; i < reg_count(); i++) {
            const char *k = reg_key_at(i);
            out_multi("  %s%s%s = %s", C_CYAN, k, C_RESET, reg_get(k, ""));
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
    return 1;
}


void shell_register_builtins(void) {
    intr_set_poll(shell_poll_byte);

    static const Command builtins[] = {
        {"run", "run <app.app> [arg]",            cmd_run, nullptr},
        {"put", "put <name> <len>  upload bytes", cmd_put, nullptr},
        {"reg", "reg | reg get K | reg set K V",  cmd_reg, nullptr},
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
    net_register();         // wifi
    netapps_register();     // ping / nslookup / ntp
    apps_register();        // apps / unload for resident packages
    pkg_init();             // ensure /pkg exists
    pkg_register();         // install / remove / list

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

// A piped stage's input, for commands that can read it instead of a file.
static const char *g_stdin;
static uint32_t    g_stdin_len;

const char *shell_stdin(void)     { return g_stdin; }
uint32_t    shell_stdin_len(void) { return g_stdin_len; }





// Run one command (no connectors, no pipes, no redirect left in it).
// Returns its exit status; a missing command is status 1.
static int run_one(char *seg) {
    char *argv[16];
    int argc = cmdline_split_args(seg, argv, 16);
    if (argc == 0) return 0;
    const Command *c = cmd_resolve(argv[0]);
    if (!c) { out_err("'%s' is not a command or executable file.", argv[0]); return 1; }
    return c->fn(argc, argv);
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

    g_stdin = nullptr; g_stdin_len = 0;

    while (stage) {
        char *pipe = cmdline_next_pipe(stage);
        bool last = (pipe == nullptr);
        if (pipe) *pipe = 0;

        bool capturing = !last || outfile;
        if (capturing) {
            if (!buf_a) buf_a = (char *)malloc(PIPE_BUF);
            if (!buf_a) { out_err("Not enough memory for a pipeline."); status = 1; break; }
            out_capture_begin(buf_a, PIPE_BUF);
        }

        status = run_one(cmdline_trim(stage));

        if (capturing) {
            uint32_t n = out_capture_end();
            if (out_capture_overflowed())
                out_warn("Output truncated at %u bytes.", (unsigned)(PIPE_BUF - 1));
            if (last && outfile) {
                char path[128];
                path_resolve(fs_cwd(), outfile, path, sizeof(path));
                bool ok = append ? storage_append_file(path, (const uint8_t *)buf_a, n)
                                 : storage_write_file(path, (const uint8_t *)buf_a, n);
                if (!ok) { out_err("Could not write %s.", path); status = 1; }
            } else {
                // Hand this stage's output to the next as its input. The buffers
                // swap rather than copy: the next stage captures into the other
                // one while reading from this.
                if (!buf_b) buf_b = (char *)malloc(PIPE_BUF);
                if (!buf_b) { out_err("Not enough memory for a pipeline."); status = 1; break; }
                char *t = buf_a; buf_a = buf_b; buf_b = t;
                g_stdin = buf_b; g_stdin_len = n;
            }
        }

        stage = pipe ? pipe + 1 : nullptr;
    }

    g_stdin = nullptr; g_stdin_len = 0;
    free(buf_a);
    free(buf_b);
    return status;
}

// Execute a whole input line: ; sequencing, && / || conditionals, | pipes.
static void run_line(char *line) {
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
            intr_clear();      // a stray Ctrl+C at the prompt must not kill this
            last_status = run_segment(seg);
            // A command that printed an error but returned 0 still failed, as
            // far as && is concerned. v1's had_error() served the same purpose.
            if (last_status == 0 && out_had_error()) last_status = 1;
            if (intr_pending()) {
                out_warn("Interrupted.");
                last_status = 130;          // 128 + SIGINT, the usual convention
                intr_clear();
            }
            persist_save_dirty();
        }
        kind = next_kind;
    }
}

void shell_run(void) {
    char line[128];

    while (true) {
        char prompt[80];
        // v1's prompt, colour for colour: cyan user, grey @host, blue path.
        snprintf(prompt, sizeof(prompt), "%s%s%s@%s%s:%s%s%s%s>%s ",
                 C_CYAN, session_user(), C_GRAY,
                 reg_get("System.Device_ID", "vela"), C_RESET,
                 C_BLUE, fs_cwd(), C_RESET, C_CYAN, C_RESET);
        if (!read_line(prompt, line, sizeof(line))) continue;
        hist_add(line);          // record before the parser chops it up
        run_line(line);
    }
}
