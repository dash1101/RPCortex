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

    // Single quotes, as v1 had: the other kind inside is plain text.
    char s1[] = "curl -d '{\"k\":1}' http://x";
    n = cmdline_split_args(s1, argv, 8);
    ck(n == 4, "a single-quoted argument is one token");
    eq(argv[2], "{\"k\":1}", "double quotes inside single quotes survive");
    eq(argv[3], "http://x",  "parsing resumes after the closing single quote");

    char s2[] = "echo \"it's\" 'say \"hi\"'";
    n = cmdline_split_args(s2, argv, 8);
    ck(n == 3, "each kind of quote protects the other");
    eq(argv[1], "it's",       "an apostrophe inside double quotes is text");
    eq(argv[2], "say \"hi\"", "double quotes inside single quotes are text");

    // v1's own header example.
    char s3[] = "curl -H 'Auth: x' url";
    n = cmdline_split_args(s3, argv, 8);
    ck(n == 4, "-H 'Auth: x' is one argument");
    eq(argv[2], "Auth: x", "a single-quoted header keeps its space");

    // An empty quoted token is an argument, not nothing.
    char s4[] = "set '' \"\" end";
    n = cmdline_split_args(s4, argv, 8);
    ck(n == 4, "'' and \"\" are empty arguments");
    eq(argv[1], "", "'' is empty");
    eq(argv[2], "", "\"\" is empty");
    eq(argv[3], "end", "the argument after empty ones is intact");

    // A quote opens only where a word begins. In the middle of one it is text:
    // an SSID like Dad's WiFi, or "don't", must reach the command as typed.
    char s5[] = "echo a\"b c\"d x'y z";
    n = cmdline_split_args(s5, argv, 8);
    ck(n == 5, "a quote in the middle of a word does not open one");
    eq(argv[1], "a\"b", "a\"b stays as typed");
    eq(argv[2], "c\"d", "c\"d stays as typed");
    eq(argv[3], "x'y",  "an apostrophe inside a word is text");
    char a1[] = "wifi connect Dad's WiFi";
    n = cmdline_split_args(a1, argv, 8);
    ck(n == 4, "Dad's WiFi is two words");
    eq(argv[2], "Dad's", "the apostrophe survives in Dad's");
    eq(argv[3], "WiFi",  "and does not swallow the rest of the line");
    char a2[] = "echo don't stop";
    n = cmdline_split_args(a2, argv, 8);
    ck(n == 3, "don't stop is two words");
    eq(argv[1], "don't", "don't keeps its apostrophe");

    // After a closing quote the word carries on: 'e f'g is one word.
    char s5b[] = "echo 'e f'g h";
    n = cmdline_split_args(s5b, argv, 8);
    ck(n == 3, "text after a closing quote joins the word");
    eq(argv[1], "e fg", "'e f'g is one word");
    eq(argv[2], "h",    "and the next word is separate");
    // ...and a quote after that is just text, not a second quoted section.
    char s5c[] = "echo 'a'\"b c\"";
    n = cmdline_split_args(s5c, argv, 8);
    ck(n == 3, "a quote straight after a closing quote is text");
    eq(argv[1], "a\"b", "'a'\"b is the word a\"b");

    // Unterminated single quote takes the remainder, like the double one.
    char s6[] = "echo 'never closed";
    n = cmdline_split_args(s6, argv, 8);
    ck(n == 2, "unterminated single quote yields one argument");
    eq(argv[1], "never closed", "unterminated single quote takes the remainder");

    // Quotes that close right at the end must leave a clean terminator.
    char s7[] = "x 'y'";
    n = cmdline_split_args(s7, argv, 8);
    ck(n == 2, "a quote closing at end of line");
    eq(argv[1], "y", "the last token is terminated");

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
    char g1[] = "echo 'a && b'";
    ck(cmdline_next_connector(g1, &k, &skip) == nullptr, "&& inside single quotes is text");
    char g2[] = "echo 'a ; b' ; ok";
    p = cmdline_next_connector(g2, &k, &skip);
    ck(p != nullptr && k == CON_SEQ && p == g2 + 13, "the ; in single quotes is skipped, the real one found");
    // A quote only closes on its own kind: the " here is text, so the && after
    // the closing ' is the connector.
    char g3[] = "echo 'say \"x\" ;' && ok";
    p = cmdline_next_connector(g3, &k, &skip);
    ck(p != nullptr && k == CON_AND, "the other kind of quote inside does not end it");
    char g4[] = "echo \"it's ; fine\" ; ok";
    p = cmdline_next_connector(g4, &k, &skip);
    ck(p != nullptr && p == g4 + 19, "an apostrophe inside double quotes does not open one");
    // An apostrophe inside a word is not a quote, so it hides nothing: the ;
    // after it is a connector.
    char g5[] = "echo Dad's WiFi ; ok";
    p = cmdline_next_connector(g5, &k, &skip);
    ck(p != nullptr && k == CON_SEQ && p == g5 + 16, "an apostrophe inside a word does not hide a connector");
    char g6[] = "echo hi;'a ; b'";
    p = cmdline_next_connector(g6, &k, &skip);
    ck(p == g6 + 7 && cmdline_next_connector(p + 1, &k, &skip) == nullptr, "a quote right after an operator opens one");
    // A quote straight after each operator opens one. The scan has to reach it
    // from the left, so the hidden operator is a different kind from the one
    // just before the quote.
    char o1[] = "echo a >'x ; y' && ok";
    p = cmdline_next_connector(o1, &k, &skip);
    ck(p != nullptr && k == CON_AND, "a quote right after > opens one");
    char o2[] = "echo a|'x ; y' && ok";
    p = cmdline_next_connector(o2, &k, &skip);
    ck(p != nullptr && k == CON_AND, "a quote right after | opens one");
    char o3[] = "echo a &'x ; y' && ok";
    p = cmdline_next_connector(o3, &k, &skip);
    ck(p != nullptr && k == CON_AND, "a quote right after & opens one");
    char o4[] = "echo a;'x | y' | cat";
    ck(cmdline_next_pipe(o4) == o4 + 15, "a quote right after ; opens one");
    // A scan that begins with a quote must not look before its own buffer.
    char b1[] = "'a ; b' && ok";
    p = cmdline_next_connector(b1, &k, &skip);
    ck(p == b1 + 8 && k == CON_AND, "a quote at the very start opens one (connector)");
    char b2[] = "'a | b' | cat";
    ck(cmdline_next_pipe(b2) == b2 + 8, "a quote at the very start opens one (pipe)");
    char b3[] = "'a > b' > out";
    bool bapp = false;
    eq(cmdline_split_redirect(b3, &bapp), "out", "a quote at the very start opens one (redirect)");

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

    char d2[] = "echo 'a | b'";
    ck(cmdline_next_pipe(d2) == nullptr, "a pipe inside single quotes is text");
    char d3[] = "echo \"it's\" | cat";
    p = cmdline_next_pipe(d3);
    ck(p == d3 + 12, "an apostrophe inside double quotes does not hide the pipe");

    char d4[] = "echo don't | cat";
    p = cmdline_next_pipe(d4);
    ck(p == d4 + 11, "an apostrophe inside a word does not hide the pipe");

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

    char g2[] = "echo 'a > b'";
    ck(cmdline_split_redirect(g2, &app) == nullptr, "a > inside single quotes is text");
    char g3[] = "echo 'a > b' > out";
    f = cmdline_split_redirect(g3, &app);
    eq(f, "out", "the real > is found after a quoted one");
    eq(g3, "echo 'a > b' ", "and the quoted > stays in the command");

    // A filename wrapped in quotes names the file, not the quotes.
    char q1[] = "ls > 'out.txt'";
    eq(cmdline_split_redirect(q1, &app), "out.txt", "single-quoted filename");
    char q2[] = "ls >> \"out.txt\"";
    eq(cmdline_split_redirect(q2, &app), "out.txt", "double-quoted filename");
    char q4[] = "ls > \"odd'";
    eq(cmdline_split_redirect(q4, &app), "\"odd'", "mismatched quotes are left for the caller to refuse");
    char q3[] = "ls > ''";
    ck(cmdline_split_redirect(q3, &app) == nullptr, "an empty quoted filename is not a redirect");

    char r1[] = "echo Dad's > out";
    eq(cmdline_split_redirect(r1, &app), "out", "an apostrophe inside a word does not hide the >");
    char r2[] = "echo hi >'a'";
    eq(cmdline_split_redirect(r2, &app), "a", "a quote straight after > opens one");

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
