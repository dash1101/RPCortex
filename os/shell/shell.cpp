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
#include "perms.h"
#include "blackbox.h"
#include "lineedit.h"
#include "interrupt.h"
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
void netapps_register(void);
void fetch_register(void);
void ps_register(void);
void log_register(void);
void stock_register(void);
void stock_install_once(void);
void jobs_register(void);
void jobs_run_startup(void);
void jobs_start_services(void);
void fsinit_register(void);
void fs_layout_check(bool verbose);

// --- line input -------------------------------------------------------------
//
// The editor itself is core/lineedit.cpp — pure, host-tested. What lives here is
// the two things it cannot know: how to read a byte from this device, and what
// the completion candidates are.

static int shell_getch(void *, uint32_t timeout_us) {
    // Anything intr_check pulled off the wire while a command was running is
    // real input that arrived early — a pasted block, or someone typing ahead.
    // It has to come out before anything new, and in order.
    int stashed = intr_stashed();
    if (stashed >= 0) return stashed;

    int c = getchar_timeout_us(timeout_us);
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
        // Short waits with a yield between them, rather than one long block:
        // a five-second wait feeds nothing and a slow transfer would trip the
        // watchdog part-way through writing a file.
        uint32_t waited = 0;
        do {
            c = getchar_timeout_us(1000);
            if (c == PICO_ERROR_TIMEOUT) { task_yield(); waited++; }
        } while (c == PICO_ERROR_TIMEOUT && waited < 20000);
        if (c == PICO_ERROR_TIMEOUT) {
            free(buf);
            out_err("Timed out after %u of %u bytes.", (unsigned)i, (unsigned)len);
            return 1;
        }
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



// bg — run a command as a background task.
//
// The line is copied onto the heap because the caller's buffer is a stack local
// that will be gone before the task first runs. The task frees it on the way
// out, which is the only place that can know it is finished with it.
struct BgJob { char line[128]; };

static int run_segment(char *seg);      // defined with the pipeline machinery

static int bg_entry(void *arg) {
    BgJob *job = (BgJob *)arg;
    int rc = run_segment(job->line);
    free(job);
    return rc;
}

static int cmd_bg(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: bg <command> [args...]"); return 1; }

    BgJob *job = (BgJob *)malloc(sizeof(BgJob));
    if (!job) { out_err("Not enough memory to start a task."); return 1; }
    job->line[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(job->line, " ", sizeof(job->line) - strlen(job->line) - 1);
        strncat(job->line, argv[i], sizeof(job->line) - strlen(job->line) - 1);
    }

    int pid = task_spawn(argv[1], job->line, bg_entry, job,
                         TASK_STACK_DEF, AFFINITY_ANY);
    if (pid < 0) {
        free(job);
        out_err("Could not start a task — the table is full or memory is short.");
        return 1;
    }
    out_ok("[%d] %s", pid, job->line);
    return 0;
}

// Never called: run_one handles sudo before dispatch. See the table below.
static int cmd_sudo(int argc, char **argv) {
    (void)argc; (void)argv;
    out_multi("Usage: sudo <command> [args...]");
    return 1;
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


// Defined below with the pipeline machinery, since alias expansion happens
// there; declared here so the registration table can name them.
static int cmd_alias(int argc, char **argv);
static int cmd_unalias(int argc, char **argv);
static void shell_load_aliases(void);

void shell_register_builtins(void) {
    intr_set_poll(shell_poll_byte);

    static const Command builtins[] = {
        {"run", "run <app.app> [arg]",            cmd_run, nullptr},
        {"put", "put <name> <len>  upload bytes", cmd_put, nullptr, LEVEL_ADMIN},
        {"reg", "reg | reg get K | reg set K V",  cmd_reg, nullptr, LEVEL_ADMIN},
        {"alias",   "alias name=command",         cmd_alias,   nullptr},
        {"unalias", "unalias <name>",             cmd_unalias, nullptr},
        {"bg",      "bg <command>  run in background", cmd_bg,  nullptr},
        // sudo is intercepted in run_one before dispatch, so this entry exists
        // to make it visible in help and Tab completion. Reaching the function
        // means someone got past the interception, which cannot happen.
        {"sudo",    "sudo <command>  run as an admin", cmd_sudo, nullptr},
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
    fetch_register();       // fetch / neofetch
    ps_register();          // ps / kill — the task manager
    log_register();         // logdump
    jobs_register();        // startup / task / service / watch
    fsinit_register();      // fscheck
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

// A piped stage's input, for commands that can read it instead of a file.
static const char *g_stdin;
static uint32_t    g_stdin_len;

const char *shell_stdin(void)     { return g_stdin; }
uint32_t    shell_stdin_len(void) { return g_stdin_len; }





// Run one command (no connectors, no pipes, no redirect left in it).
// Returns its exit status; a missing command is status 1.
// Set for the duration of one command by `sudo`. Not a mode: it is cleared the
// moment that command returns, so a sudo cannot leak into the next thing typed.
static bool g_elevated;

static int run_one(char *seg) {
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
        const char *repl = cmd_ualias_get(first);
        if (repl) {
            snprintf(expanded, sizeof(expanded), "%s%s", repl, sp ? sp : "");
            seg = expanded;
        }
    }

    char *raw[16];
    int n = cmdline_split_args(seg, raw, 16);
    if (n == 0) return 0;

    // sudo <command> — raise the level for exactly this one command. The shift
    // is done with an index rather than by moving the array pointer, so argv
    // stays a real array and the command sees a normal argv[0].
    char **argv = raw;
    int argc = n;
    bool elevated = g_elevated;
    if (!strcmp(argv[0], "sudo")) {
        if (argc < 2) { out_multi("Usage: sudo <command> [args...]"); return 1; }
        if (!users_is_admin(session_user())) {
            out_err("%s is not an admin. sudo cannot grant what the account does not have.",
                    session_user());
            return 1;
        }
        elevated = true;
        argv = raw + 1;
        argc = n - 1;
    }

    const Command *c = cmd_resolve(argv[0]);
    if (!c) { out_err("'%s' is not a command or executable file.", argv[0]); return 1; }

    if (!perms_allows(session_user(), users_is_admin(session_user()), c->level, elevated)) {
        out_err("'%s' needs an admin account.", c->name);
        out_multi("  Signed in as %s. An admin can run it, or use 'sudo %s'.",
                  session_user(), c->name);
        return 1;
    }

    bool saved = g_elevated;
    g_elevated = elevated;
    int rc = c->fn(argc, argv);
    g_elevated = saved;
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
    if (cmd_find(name)) { out_warn("Cannot shadow the built-in command '%s'.", name); return 1; }

    if (!cmd_ualias_set(name, value)) { out_err("Could not define '%s'.", name); return 1; }
    char key[REG_KEY_MAX];
    snprintf(key, sizeof(key), "Alias.%s", name);
    reg_set(key, value);
    out_ok("alias %s = %s", name, value);
    return 0;
}

static int cmd_unalias(int argc, char **argv) {
    if (argc < 2) { out_warn("Usage: unalias <name>"); return 1; }
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

// Run one line through the full parser. Exposed so startup, the scheduler and
// watch get pipes, chaining and redirection for free instead of each growing
// its own half-parser.
int shell_run_line_now(char *line) {
    bb_note_command(line);
    out_clear_error();
    int rc = run_segment(line);
    if (rc == 0 && out_had_error()) rc = 1;
    persist_save_dirty();
    return rc;
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
        bb_note_command(line);   // so a hang can name the command, not just the task
        run_line(line);
        bb_note_command("");     // finished cleanly; do not blame it for the next hang
    }
}
