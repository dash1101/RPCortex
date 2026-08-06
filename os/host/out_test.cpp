// The output layer's exact bytes.
//
// A terminal looking right is not a check — the escape sequences are what
// make it look right, and a wrong one shows up as visible garbage on someone
// else's. So this captures stdout and compares against the literal strings v1
// emitted, character for character.
//
// Run with -v to also print a rendered sample, for eyeballing colour choices.

#include "out.h"

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
static void emit_multi(void) { out_multi("plain %d", 7); }
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

    printf("  out: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
