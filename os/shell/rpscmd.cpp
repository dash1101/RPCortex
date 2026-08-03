// script — running a .rps file, wired to the real shell.
//
// The language itself is core/rps.cpp and knows nothing about this device. What
// is here is the four things it needs from the outside: run a command, read a
// line, check a path, and notice Ctrl+C.
//
// Capturing a command's output reuses the shell's existing pipe machinery
// rather than a second mechanism. That matters for compatibility: `capture`
// sees exactly what a pipe would, which is what v1 promised and what makes
// `capture n wc -l file` behave the same in both.

#include "command.h"
#include "out.h"
#include "rps.h"
#include "storage.h"
#include "path.h"
#include "interrupt.h"
#include "task.h"
#include "session.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

const char *fs_cwd(void);
// The shell's full line runner: pipes, chaining and redirection included, so a
// script line behaves exactly as the same line typed at the prompt.
int shell_run_line_now(char *line);

#define SCRIPT_MAX 8192

static bool host_run(void *, const char *line, char *cap, uint32_t cn) {
    // The runner takes a mutable buffer, since the parser splits in place.
    char work[RPS_LINE_MAX];
    snprintf(work, sizeof(work), "%s", line);

    if (cap && cn) {
        cap[0] = 0;
        // Reuse the shell's own capture rather than a second mechanism, so
        // `capture` sees exactly what a pipe would. That is what makes
        // `capture n wc -l file` behave the same here as it did on v1.
        if (!out_capture_begin(cap, cn)) return shell_run_line_now(work) == 0;
        int rc = shell_run_line_now(work);
        out_capture_end();
        return rc == 0;
    }
    return shell_run_line_now(work) == 0;
}

static bool host_prompt(void *, const char *msg, char *outp, uint32_t cap) {
    char full[96];
    snprintf(full, sizeof(full), "%s", msg && msg[0] ? msg : "");
    session_prompt(full, outp, cap, /*secret*/false);
    return true;
}

static bool host_exists(void *, const char *p) {
    char full[128];
    path_resolve(fs_cwd(), p, full, sizeof(full));
    bool is_dir = false;
    return storage_stat(full, &is_dir, nullptr);
}

static void host_print(void *, const char *line) { out_multi("%s", line); }

// Ctrl+C stops a script between statements, which is the only safe place: a
// command already running gets interrupted by its own checks.
static int host_poll(void *) { return intr_check() ? 1 : 0; }

static int cmd_script(int argc, char **argv) {
    if (argc < 2) {
        out_multi("Usage: script <file.rps>");
        out_multi("  set NAME value        inc/dec NAME [n]     prompt NAME message");
        out_multi("  capture NAME command  if <cond> / else / end");
        out_multi("  while <cond> / end    break  continue  stop");
        out_multi("  conditions: eq ne gt lt ge le contains exists empty not, or any command");
        return 1;
    }

    char full[128];
    path_resolve(fs_cwd(), argv[1], full, sizeof(full));

    bool is_dir = false; uint32_t size = 0;
    if (!storage_stat(full, &is_dir, &size)) { out_err("No such file: %s", full); return 1; }
    if (is_dir) { out_err("%s is a directory.", full); return 1; }
    if (size >= SCRIPT_MAX) {
        out_err("Script is %lu bytes; the limit is %d.", (unsigned long)size, SCRIPT_MAX);
        return 1;
    }

    char *text = (char *)malloc(SCRIPT_MAX);
    if (!text) { out_err("Not enough memory to read the script."); return 1; }
    uint32_t n = storage_read_file(full, (uint8_t *)text, SCRIPT_MAX - 1);
    text[n] = 0;

    RpsHost h{};
    h.run = host_run; h.prompt = host_prompt; h.exists = host_exists;
    h.print = host_print; h.poll = host_poll;

    RpsResult r;
    bool ok = rps_run(text, &h, &r);
    free(text);

    if (!ok) {
        out_err("%s:%lu: %s", argv[1], (unsigned long)r.line, r.error);
        return 1;
    }
    return 0;
}

void rps_register(void) {
    static const Command c{"script", "run a .rps script", cmd_script};
    cmd_register(&c);
}
