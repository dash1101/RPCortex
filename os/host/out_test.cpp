// The output layer's exact bytes.
//
// A terminal looking right is not a check — the escape sequences are what
// make it look right, and a wrong one shows up as visible garbage on someone
// else's. So this captures stdout and compares against the literal strings v1
// emitted, character for character.
//
// Run with -v to also print a rendered sample, for eyeballing colour choices.

#include "out.h"
#include "logring.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int checks = 0, fails = 0;

static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

// Run `fn` with stdout redirected into `buf`.
static void capture(void (*fn)(void), char *buf, size_t cap) {
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    FILE *tmp = tmpfile();
    dup2(fileno(tmp), STDOUT_FILENO);
    fn();
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    rewind(tmp);
    size_t n = fread(buf, 1, cap - 1, tmp);
    buf[n] = 0;
    fclose(tmp);
}

static void eq(const char *got, const char *want, const char *what) {
    checks++;
    if (strcmp(got, want) == 0) return;
    fails++;
    printf("  FAIL: %s\n", what);
    printf("        got  : ");
    for (const char *p = got;  *p; p++) printf(*p == 27 ? "\\e" : "%c", *p);
    printf("\n        want : ");
    for (const char *p = want; *p; p++) printf(*p == 27 ? "\\e" : "%c", *p);
    printf("\n");
}

// --- the line shapes --------------------------------------------------------

static void emit_ok(void)    { out_ok("Ready"); }
static void emit_err(void)   { out_err("Nope"); }
static void emit_warn(void)  { out_warn("Careful"); }
static void emit_info(void)  { out_info("Working"); }
static void emit_fatal(void) { out_fatal("Gone"); }
static void emit_okp(void)   { out_okp("WiFi", "Connected"); }
static void emit_usage(void) { out_warn("Usage: thing <file>"); }
static void emit_multi(void) { out_multi("plain %d", 7); }
static void emit_multi_coloured(void) {
    out_multi("  %s%2u%s  %s", "\033[96m", 3u, "\033[0m", "novad1 gui --bg");
}
static void emit_prompt(void){ out_prompt("Username"); }
static void emit_blank(void) { out_blank(); }

int main(int argc, char **argv) {
    char buf[512];

    // v1's _fmt: <colour>[<white><symbol><colour>] <reset><message>
    capture(emit_ok,   buf, sizeof(buf));
    eq(buf, "\033[96m[\033[97m@\033[96m] \033[0mReady\n", "out_ok line shape");

    capture(emit_err,  buf, sizeof(buf));
    eq(buf, "\033[91m[\033[97m!\033[91m] \033[0mNope\n", "out_err line shape");

    capture(emit_warn, buf, sizeof(buf));
    eq(buf, "\033[93m[\033[97m?\033[93m] \033[0mCareful\n", "out_warn line shape");

    capture(emit_info, buf, sizeof(buf));
    eq(buf, "\033[95m[\033[97m:\033[95m] \033[0mWorking\n", "out_info line shape");

    capture(emit_fatal, buf, sizeof(buf));
    eq(buf, "\033[91m[\033[97m!!!\033[91m] \033[0mGone\n", "out_fatal line shape");

    // The prefix variant puts a second bracket between tag and message.
    capture(emit_okp, buf, sizeof(buf));
    eq(buf, "\033[96m[\033[97m@\033[96m] \033[96m[\033[97mWiFi\033[96m] \033[0mConnected\n",
       "out_okp prefix shape");

    // multi is the data channel: no tag, no colour, no decoration at all. This
    // is what makes piping possible later, so it must stay literally bare.
    capture(emit_multi, buf, sizeof(buf));
    eq(buf, "plain 7\n", "out_multi is undecorated");

    capture(emit_blank, buf, sizeof(buf));
    eq(buf, "\n", "out_blank is one newline");

    // v1's prompt: "<msg> ••>  " with the bullets in cyan.
    capture(emit_prompt, buf, sizeof(buf));
    eq(buf, "\033[0mUsername \033[96m••>  \033[0m", "out_prompt shape");

    // --- the error flag, which && / || will read ---------------------------
    out_clear_error();
    ck(!out_had_error(), "flag clear after out_clear_error");
    capture(emit_multi, buf, sizeof(buf));
    ck(!out_had_error(), "out_multi does not set the error flag");
    capture(emit_warn, buf, sizeof(buf));
    ck(!out_had_error(), "out_warn does not set the error flag");
    capture(emit_err, buf, sizeof(buf));
    ck(out_had_error(), "out_err sets the error flag");
    out_clear_error();
    capture(emit_fatal, buf, sizeof(buf));
    ck(out_had_error(), "out_fatal sets the error flag");
    out_clear_error();

    // --- out_pad counts VISIBLE width --------------------------------------
    char dst[64];

    out_pad("abc", 6, dst, sizeof(dst));
    eq(dst, "abc   ", "pad plain string");

    // The whole point: printf's %-6s would count the 9 escape bytes and emit no
    // padding at all. This must still add 3 spaces.
    out_pad("\033[96mabc\033[0m", 6, dst, sizeof(dst));
    eq(dst, "\033[96mabc\033[0m   ", "pad ignores ANSI escapes");

    // A multi-byte character is one column, not three.
    out_pad("ab•", 5, dst, sizeof(dst));
    eq(dst, "ab•  ", "pad counts UTF-8 characters, not bytes");

    out_pad("toolongvalue", 4, dst, sizeof(dst));
    eq(dst, "toolongvalue", "pad never truncates");

    out_pad("", 3, dst, sizeof(dst));
    eq(dst, "   ", "pad an empty string");

    // A destination too small must still terminate rather than run off.
    char tiny[4];
    out_pad("abcdefgh", 8, tiny, sizeof(tiny));
    ck(strlen(tiny) < sizeof(tiny), "pad respects the destination cap");

    // --- what lands in a capture -------------------------------------------
    //
    // A capture is read by a parser, so what goes into it must be text. Every
    // check below is a bug that shipped: `novad1 setup` looked for a digit at
    // the start of a `service list` row, found 0x1b, matched nothing, and added
    // a fourth copy of a service that was already listed three times.

    char cap[256];

    ck(out_capture_begin(cap, sizeof(cap)), "a capture starts");
    out_multi("  %s%2u%s  %s", "\033[96m", 3u, "\033[0m", "novad1 gui --bg");
    out_capture_end();
    eq(cap, "   3  novad1 gui --bg\n", "out_multi captures without the colour");

    // The same line on the console keeps every escape. Stripping is what the
    // BUFFER is for, and must not reach the terminal.
    capture(emit_multi_coloured, buf, sizeof(buf));
    eq(buf, "  \033[96m 3\033[0m  novad1 gui --bg\n", "the console still gets colour");

    // out_write is handed whatever the caller had, so a sequence can arrive in
    // pieces. The state has to live across the calls or half of it lands.
    ck(out_capture_begin(cap, sizeof(cap)), "a capture starts");
    out_write("ab\033", 3);
    out_write("[96mcd", 6);
    out_capture_end();
    eq(cap, "abcd", "an escape split across two writes is still removed");

    // The tagged lines are the status channel: a pipe must not get them, since
    // during `ls > f` an error about the listing belongs on the screen.
    ck(out_capture_begin(cap, sizeof(cap)), "a capture starts");
    out_info("Services:");
    out_multi("  one");
    out_capture_end();
    eq(cap, "  one\n", "a plain capture takes the data channel only");

    // fw_shell_run's form takes both, because a package that asked for a
    // command's output means all of it. A listing whose header is an out_info
    // and whose rows are out_multi arrives half-captured otherwise — and half a
    // listing reads exactly like a complete one.
    ck(out_capture_begin_all(cap, sizeof(cap)), "a full capture starts");
    out_info("Services:");
    out_multi("  one");
    out_blank();
    out_capture_end();
    eq(cap, "[:] Services:\n  one\n\n", "a full capture takes the tagged lines too");

    // Truncation must be reported. A short read of a list is a shorter list,
    // and the one caller that matters is trying to remove all of them.
    char small[12];
    ck(out_capture_begin(small, sizeof(small)), "a small capture starts");
    out_multi("0123456789abcdefghij");
    ck(out_capture_overflowed(), "overflow is flagged");
    out_capture_end();
    ck(strlen(small) == sizeof(small) - 1, "a truncated capture is still terminated");

    // --- a capture inside a capture, on one task ---
    // fw_shell_run("ls | grep x") captures, and the pipeline inside it captures
    // each stage. With one capture for the whole device the inner one could not
    // start, the stage printed into the OUTER buffer, and ending it ended the
    // outer capture and handed back its length for a buffer that held nothing.
    char outer[64], inner[64];
    ck(out_capture_begin_all(outer, sizeof(outer)), "an outer capture starts");
    out_multi("before");
    ck(out_capture_begin(inner, sizeof(inner)), "a capture inside it starts too");
    out_multi("inside");
    uint32_t in_n = out_capture_end();
    eq(inner, "inside\n", "the inner capture took only what was printed inside it");
    ck(in_n == strlen("inside\n"), "and its length is its own");
    out_multi("after");
    out_capture_end();
    eq(outer, "before\nafter\n", "the outer capture is back, and never saw the inner output");

    // --- the error flag across a nested run ---
    // A line run with fw_shell_run from inside a command clears the flag to judge
    // itself; the outer command's error has to come back afterwards.
    char sink[64];
    out_capture_begin(sink, sizeof(sink));      // keep the error text off the test's output
    out_clear_error();
    out_err("the outer command failed");
    bool outer_err = out_had_error();
    out_clear_error();                          // the inner run starts clean
    ck(!out_had_error(), "an inner run starts without the outer error");
    out_restore_error(outer_err);
    ck(out_had_error(), "and the outer error is back after it");
    out_clear_error();
    out_restore_error(false);
    ck(!out_had_error(), "restoring 'no error' never invents one");
    out_capture_end();

    // --- ending with nothing to end ---
    ck(out_capture_end() == 0, "ending when nothing is captured returns nothing");
    ck(!out_capturing(), "and leaves nothing captured");

    // --- more captures than there is room for ---
    static char many[16][8];
    int got = 0;
    while (got < 16 && out_capture_begin(many[got], sizeof(many[got]))) got++;
    ck(got > 0 && got < 16, "captures run out at some bound rather than forever");
    ck(!out_capture_begin(small, sizeof(small)), "and a begin past it says no");
    for (int i = 0; i < got; i++) out_capture_end();
    ck(!out_capturing(), "and every one that started can be ended");
    ck(out_capture_begin(small, sizeof(small)), "after which a capture starts again");
    out_capture_end();

    if (argc > 1 && !strcmp(argv[1], "-v")) {
        printf("\n  --- rendered sample ---\n");
        out_ok("Package 'greet' loaded.");
        out_info("Scanning...");
        out_warn("Largest run is under 16 KB — reboot to defragment.");
        out_err("'frobnicate' is not a command or executable file.");
        out_okp("pkg", "Installed greet 1.0");
        out_multi("  Total : 480 KB");
        printf("\n");
    }

    // A warning goes into the log ring; a usage line does not. The ring holds
    // 48 lines and a handful of usage messages used to push out boot entries.
    {
        uint32_t before = log_count();
        capture(emit_warn, buf, sizeof(buf));
        ck(log_count() == before + 1 || log_count() == LOG_LINES, "a warning is logged");
        before = log_count();
        capture(emit_usage, buf, sizeof(buf));
        ck(log_count() == before, "a usage line is not logged");
        ck(strstr(buf, "Usage: thing <file>") != nullptr, "but it is still printed");
    }

    printf("  out: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
