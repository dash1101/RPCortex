// Command-line parsing: argv, quotes, pipes, chaining, redirection.
//
// An off-by-one here does not crash — it runs the wrong command, or writes the
// output to the wrong file. That class of bug is nearly invisible on a device
// with no debugger, so the parser is pure and every edge is pinned down here.

#include "cmdline.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;

static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (got && want && strcmp(got, want) == 0) return;
    if (!got && !want) return;
    fails++;
    printf("  FAIL: %s  (got '%s', want '%s')\n", what,
           got ? got : "(null)", want ? want : "(null)");
}

// --- argv -------------------------------------------------------------------

static void t_args(void) {
    char *argv[8];
    char a[] = "ls -l /pkg";
    int n = cmdline_split_args(a, argv, 8);
    ck(n == 3, "argc for a plain line");
    eq(argv[0], "ls",   "argv[0]");
    eq(argv[2], "/pkg", "argv[2]");

    char b[] = "   spaced    out   ";
    n = cmdline_split_args(b, argv, 8);
    ck(n == 2, "leading/trailing/repeated spaces collapse");
    eq(argv[0], "spaced", "first token past leading spaces");
    eq(argv[1], "out",    "last token before trailing spaces");

    char c[] = "echo \"hello world\" tail";
    n = cmdline_split_args(c, argv, 8);
    ck(n == 3, "a quoted argument is one token");
    eq(argv[1], "hello world", "quotes are removed, the space survives");
    eq(argv[2], "tail",        "parsing resumes after the closing quote");

    // An unterminated quote must consume the rest rather than run off the end.
    char d[] = "echo \"unterminated";
    n = cmdline_split_args(d, argv, 8);
    ck(n == 2, "unterminated quote yields one argument");
    eq(argv[1], "unterminated", "unterminated quote takes the remainder");

    char e[] = "";
    ck(cmdline_split_args(e, argv, 8) == 0, "an empty line has no arguments");

    // More tokens than the array holds must stop, not overflow.
    char f[] = "a b c d e f g h i j k l";
    char small[4];
    char *sv[4];
    (void)small;
    n = cmdline_split_args(f, sv, 4);
    ck(n == 4, "argv is capped at max");
}

// --- connectors -------------------------------------------------------------

static void t_connectors(void) {
    Connector k = CON_FIRST;
    int skip = 0;

    char a[] = "one && two";
    char *p = cmdline_next_connector(a, &k, &skip);
    ck(p != nullptr && k == CON_AND && skip == 2, "&& is found as CON_AND");
    ck(p == a + 4, "&& is found at the right offset");

    char b[] = "one || two";
    p = cmdline_next_connector(b, &k, &skip);
    ck(p != nullptr && k == CON_OR && skip == 2, "|| is CON_OR, not a pipe");

    char c[] = "one ; two";
    p = cmdline_next_connector(c, &k, &skip);
    ck(p != nullptr && k == CON_SEQ && skip == 1, "; is CON_SEQ");

    // A single pipe is NOT a connector — it is handled one level down.
    char d[] = "one | two";
    ck(cmdline_next_connector(d, &k, &skip) == nullptr, "a single | is not a connector");

    char e[] = "plain command";
    ck(cmdline_next_connector(e, &k, &skip) == nullptr, "no connector in a plain line");

    // Quoted connectors are text, not syntax.
    char f[] = "echo \"a && b\"";
    ck(cmdline_next_connector(f, &k, &skip) == nullptr, "&& inside quotes is text");
    char g[] = "echo \"a ; b\" && ok";
    p = cmdline_next_connector(g, &k, &skip);
    ck(p != nullptr && k == CON_AND, "the ; in quotes is skipped, the && is found");
}

// --- pipes ------------------------------------------------------------------

static void t_pipes(void) {
    char a[] = "cat f | grep x";
    char *p = cmdline_next_pipe(a);
    ck(p == a + 6, "a single pipe is found");

    // The critical one: '||' must not be mistaken for a pipe, or `a || b`
    // silently becomes a pipeline into an empty command.
    char b[] = "cmd || other";
    ck(cmdline_next_pipe(b) == nullptr, "|| is not a pipe");

    char c[] = "a || b | c";
    p = cmdline_next_pipe(c);
    ck(p == c + 7, "a real pipe after a || is still found");

    char d[] = "echo \"a | b\"";
    ck(cmdline_next_pipe(d) == nullptr, "a pipe inside quotes is text");

    char e[] = "a | b | c";
    p = cmdline_next_pipe(e);
    ck(p == e + 2, "the FIRST pipe is returned");
}

// --- redirection ------------------------------------------------------------

static void t_redirect(void) {
    bool app = false;

    char a[] = "ls > out.txt";
    char *f = cmdline_split_redirect(a, &app);
    eq(f, "out.txt", "> filename");
    ck(!app, "> is not append");
    eq(cmdline_trim(a), "ls", "the segment is truncated at the >");

    char b[] = "ls >> out.txt";
    app = false;
    f = cmdline_split_redirect(b, &app);
    eq(f, "out.txt", ">> filename");
    ck(app, ">> is append");

    char c[] = "ls -l";
    ck(cmdline_split_redirect(c, &app) == nullptr, "no redirect in a plain segment");

    // A dangling '>' is a typo, not a redirect to a file with no name.
    char d[] = "ls >";
    ck(cmdline_split_redirect(d, &app) == nullptr, "a dangling > is not a redirect");

    // The last redirect wins, as it does in every other shell.
    char e[] = "echo a > b > c";
    f = cmdline_split_redirect(e, &app);
    eq(f, "c", "the last > wins");

    char g[] = "echo \"a > b\"";
    ck(cmdline_split_redirect(g, &app) == nullptr, "a > inside quotes is text");

    char h[] = "cat f>g";
    f = cmdline_split_redirect(h, &app);
    eq(f, "g", "> needs no surrounding spaces");
    eq(h, "cat f", "the segment ends at the >");
}

// --- trim -------------------------------------------------------------------

static void t_trim(void) {
    char a[] = "   padded   ";
    eq(cmdline_trim(a), "padded", "trim both ends");
    char b[] = "none";
    eq(cmdline_trim(b), "none", "trim leaves a clean string alone");
    char c[] = "     ";
    eq(cmdline_trim(c), "", "an all-space string trims to empty");
    char d[] = "";
    eq(cmdline_trim(d), "", "an empty string trims to empty");
}

int main(void) {
    t_args();
    t_connectors();
    t_pipes();
    t_redirect();
    t_trim();
    printf("  cmdline: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
