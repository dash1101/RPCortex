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

// --- line input -------------------------------------------------------------
//
// Reads a line with backspace and up/down history recall. The prompt is passed
// in so a history recall can redraw the whole line (\r, clear-to-EOL, prompt,
// recalled text). Cursor-in-the-middle editing is deliberately not here yet;
// history is the recall people reach for first.

static bool read_line(const char *prompt, char *buf, size_t max) {
    size_t n = 0;
    int browse = -1;                 // -1 = the live line; 0..count-1 = history depth
    printf("%s", prompt);
    while (true) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) { sleep_ms(2); continue; }
        if (c == '\r' || c == '\n') { putchar('\n'); buf[n] = 0; return true; }
        if ((c == 8 || c == 127) && n) { n--; browse = -1; printf("\b \b"); continue; }
        if (c == 0x1b) {
            // An arrow key is ESC '[' 'A'/'B'. The two bytes follow immediately;
            // a lone ESC (nothing after) is ignored.
            int a = getchar_timeout_us(3000);
            int b = getchar_timeout_us(3000);
            if (a == '[' && (b == 'A' || b == 'B')) {
                int want = browse + (b == 'A' ? 1 : -1);
                if (want < -1) want = -1;
                if (want > hist_count() - 1) want = hist_count() - 1;
                const char *h = (want < 0) ? "" : hist_get(want);
                if (h) {
                    browse = want;
                    printf("\r\x1b[K%s%s", prompt, h);   // redraw the line
                    strncpy(buf, h, max - 1); buf[max - 1] = 0; n = strlen(buf);
                }
            }
            continue;
        }
        if (c >= 32 && c < 127 && n + 1 < max) { buf[n++] = (char)c; browse = -1; putchar(c); }
    }
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

// run — the load-run-resident flow now lives in apps_launch, shared with
// `pkg install` and boot-time package loading, so there is one code path.
static int cmd_run(int argc, char **argv) {
    if (argc < 2) { out_multi("Usage: run <app> [arg]"); return 1; }
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
    static const Command builtins[] = {
        {"run", "run <app> [arg]",                cmd_run, nullptr},
        {"put", "put <name> <len>  upload bytes", cmd_put, nullptr},
        {"reg", "reg | reg get K | reg set K V",  cmd_reg, nullptr},
    };
    for (const auto &c : builtins) cmd_register(&c);
    // Order is not significant — cmd_register refuses a duplicate name, so a
    // collision fails loudly at boot rather than silently shadowing.
    help_register();        // help + its category pages
    fs_register();          // ls / cd / cat / df / du / tree ...
    text_register();        // grep / wc / sort / uniq / hex ...
    sys_register();         // sysinfo / meminfo / date / pulse ...
    user_register();        // whoami / users / passwd / logout ...
    net_register();         // wifi
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
            last_status = run_segment(seg);
            // A command that printed an error but returned 0 still failed, as
            // far as && is concerned. v1's had_error() served the same purpose.
            if (last_status == 0 && out_had_error()) last_status = 1;
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
