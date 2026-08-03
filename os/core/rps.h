// .rps scripting, with the shell held at arm's length.
//
// v1's interpreter is the specification here, deliberately: scripts people
// already have should run. Same statements, same conditions, same numeric-when-
// both-look-numeric comparison rule.
//
// Everything that decides what happens — substitution, conditions, loops,
// variables — is in this file and calls out to run a command through four
// function pointers. That is what lets the whole language be tested without a
// filesystem, a shell or a device: a fake host records what it was asked to run
// and the test asserts on the sequence.
//
// Executed line by line with a block stack rather than parsed into a tree. A
// tree would want allocation and recursion; a stack of at most a few frames is
// bounded, and the language has no constructs that need more.
#ifndef RPC_RPS_H
#define RPC_RPS_H

#include <stdint.h>
#include <stdbool.h>

#define RPS_VARS       16
#define RPS_NAME_MAX   24
#define RPS_VAL_MAX    128
#define RPS_LINE_MAX   256
#define RPS_DEPTH      8     // nested if/while

struct RpsHost {
    // Run a shell command. Returns true when it succeeded. When `capture` is
    // non-null the command's DATA output goes there — the same split v1 used,
    // where status messages are not captured and pipeable output is.
    bool (*run)(void *ctx, const char *line, char *capture, uint32_t cap);

    // Read a line from whoever is running the script.
    bool (*prompt)(void *ctx, const char *msg, char *out, uint32_t cap);

    // Does this path exist? Separate from `run` because `exists` must not have
    // side effects, and routing it through a command would.
    bool (*exists)(void *ctx, const char *path);

    void (*print)(void *ctx, const char *line);

    // Called between statements. Non-zero stops the script — Ctrl+C, or a
    // watchdog that has had enough.
    int (*poll)(void *ctx);

    void *ctx;
};

struct RpsResult {
    bool     ok;
    uint32_t line;        // where it stopped, 1-based
    char     error[96];
    uint32_t executed;    // statements run, for a script that did nothing
};

// Run `text`. Returns false on an error, with the reason and line in `out`.
bool rps_run(const char *text, const RpsHost *host, RpsResult *out);

// Substitute $NAME from the variable table. Exposed because it is the part most
// worth testing directly, and the part a caller may want for one string.
struct RpsVars;
uint32_t rps_expand(const RpsVars *v, const char *in, char *out, uint32_t cap);

#endif  // RPC_RPS_H
