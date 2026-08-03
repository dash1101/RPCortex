// The .rps interpreter, against a fake shell.
//
// v1's language is the specification: scripts people already have should run.
// Every construct is checked, and the fake host records the exact sequence of
// commands so a loop that runs the wrong number of times is visible rather than
// merely suspected.
#include "rps.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { printf("    FAIL %s\n", what); fails++; }
}

// Records everything it is asked to do, so a test can assert on the trace.
struct Fake {
    char trace[64][64];
    int  n;
    bool next_fails;
    const char *capture_with;
    const char *prompt_with;
};

static bool fk_run(void *ctx, const char *line, char *cap, uint32_t cn) {
    Fake *f = (Fake *)ctx;
    if (f->n < 64) snprintf(f->trace[f->n++], 64, "%s", line);
    if (cap && cn) snprintf(cap, cn, "%s", f->capture_with ? f->capture_with : "");
    if (f->next_fails) { f->next_fails = false; return false; }
    return true;
}
static bool fk_prompt(void *ctx, const char *, char *out, uint32_t cap) {
    Fake *f = (Fake *)ctx;
    snprintf(out, cap, "%s", f->prompt_with ? f->prompt_with : "");
    return true;
}
static bool fk_exists(void *, const char *p) { return strcmp(p, "/there") == 0; }
static void fk_print(void *, const char *) {}

static RpsHost make(Fake *f) {
    RpsHost h{};
    h.run = fk_run; h.prompt = fk_prompt; h.exists = fk_exists; h.print = fk_print;
    h.ctx = f;
    return h;
}

// Run a script; return the trace joined by '|'.
static void run(const char *src, Fake *f, char *joined, uint32_t cap, RpsResult *r) {
    memset(f, 0, sizeof(*f));
    // keep caller-set fields
    RpsHost h = make(f);
    rps_run(src, &h, r);
    joined[0] = 0;
    for (int i = 0; i < f->n; i++) {
        if (i) strncat(joined, "|", cap - strlen(joined) - 1);
        strncat(joined, f->trace[i], cap - strlen(joined) - 1);
    }
}

int main(void) {
    printf("  rps\n");
    Fake f{}; RpsResult r; char t[512];

    // --- variables and substitution ----------------------------------------
    run("set name world\necho hello $name", &f, t, sizeof(t), &r);
    ok(r.ok && !strcmp(t, "echo hello world"), "set and $substitution");

    run("set a 1\nset b 2\necho $a$b", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo 12"), "two variables adjacent");

    run("echo $missing done", &f, t, sizeof(t), &r);
    // An unset variable expands to nothing, which is what makes `empty $x` work
    // before x is ever set.
    ok(!strcmp(t, "echo  done"), "an unset variable expands to nothing");

    run("echo $$5", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo $5"), "$$ is a literal dollar");

    run("set greeting \"hello there\"\necho $greeting", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo hello there"), "a quoted value keeps its spaces");

    // --- arithmetic ---------------------------------------------------------
    run("set n 5\ninc n\necho $n", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo 6"), "inc");
    run("set n 5\ndec n 3\necho $n", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo 2"), "dec by an amount");
    run("inc fresh\necho $fresh", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo 1"), "inc on an unset variable starts from zero");

    // --- conditions ---------------------------------------------------------
    run("if eq 1 1\necho yes\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo yes"), "if eq, true");
    run("if eq 1 2\necho yes\nend\necho after", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo after"), "if eq, false, and execution continues after end");
    run("if eq 1 2\necho yes\nelse\necho no\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo no"), "else runs when the condition is false");
    run("if eq 1 1\necho yes\nelse\necho no\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo yes"), "and is skipped when it is true");

    // v1's rule: numeric when BOTH look numeric, lexical otherwise.
    run("if gt 10 9\necho num\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo num"), "10 > 9 numerically, not as text");
    run("if gt b a\necho lex\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo lex"), "b > a lexically");
    run("if lt 9 10\necho ok\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo ok"), "9 < 10 numerically");

    run("if contains hello ell\necho in\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo in"), "contains");
    run("if exists /there\necho found\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo found"), "exists, true");
    run("if exists /nowhere\necho found\nend\necho done", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo done"), "exists, false");
    run("if empty $unset\necho blank\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo blank"), "empty on an unset variable");
    run("if not eq 1 2\necho differs\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo differs"), "not inverts");

    // A bare command as a condition is what makes `if ping ...` work without
    // the language knowing anything about ping.
    {
        memset(&f, 0, sizeof(f));
        RpsHost h = make(&f);
        rps_run("if somecmd\necho ran\nend", &h, &r);
        ok(f.n == 2 && !strcmp(f.trace[0], "somecmd") && !strcmp(f.trace[1], "echo ran"),
           "a command's success is a condition");
    }

    // --- loops --------------------------------------------------------------
    run("set i 0\nwhile lt $i 3\necho $i\ninc i\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo 0|echo 1|echo 2"), "while counts and stops");

    run("set i 0\nwhile lt $i 5\ninc i\nif eq $i 3\nbreak\nend\necho $i\nend\necho out", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo 1|echo 2|echo out"), "break leaves the loop from inside an if");

    run("set i 0\nwhile lt $i 4\ninc i\nif eq $i 2\ncontinue\nend\necho $i\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo 1|echo 3|echo 4"), "continue skips the rest of the body");

    run("while eq 1 2\necho never\nend\necho after", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo after"), "a while that is false from the start runs nothing");

    // --- nesting ------------------------------------------------------------
    run("if eq 1 1\nif eq 2 2\necho deep\nend\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo deep"), "nested ifs");
    run("if eq 1 1\nif eq 1 2\necho no\nend\necho yes\nend", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo yes"), "an inner end does not close the outer block");
    run("set i 0\nwhile lt $i 2\nset j 0\nwhile lt $j 2\necho $i$j\ninc j\nend\ninc i\nend",
        &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo 00|echo 01|echo 10|echo 11"), "nested whiles");

    // --- capture and prompt -------------------------------------------------
    {
        memset(&f, 0, sizeof(f));
        f.capture_with = "42\n";
        RpsHost h = make(&f);
        rps_run("capture v somecmd\nif eq $v 42\necho matched\nend", &h, &r);
        // The trailing newline must go, or every comparison against a captured
        // value fails for a reason nobody can see.
        ok(f.n == 2 && !strcmp(f.trace[1], "echo matched"), "capture strips its trailing newline");
    }
    {
        memset(&f, 0, sizeof(f));
        f.prompt_with = "dash";
        RpsHost h = make(&f);
        rps_run("prompt who Your name\necho hi $who", &h, &r);
        ok(f.n == 1 && !strcmp(f.trace[0], "echo hi dash"), "prompt stores what was typed");
    }

    // --- stop, comments, blanks --------------------------------------------
    run("echo one\nstop\necho two", &f, t, sizeof(t), &r);
    ok(r.ok && !strcmp(t, "echo one"), "stop ends the script successfully");
    run("# a comment\n\n   \necho only", &f, t, sizeof(t), &r);
    ok(!strcmp(t, "echo only"), "comments and blank lines are skipped");

    // --- errors -------------------------------------------------------------
    run("if eq 1 1\necho x", &f, t, sizeof(t), &r);
    ok(!r.ok, "an if with no end is an error");
    run("end", &f, t, sizeof(t), &r);
    ok(!r.ok && strstr(r.error, "without"), "a stray end is an error");
    run("break", &f, t, sizeof(t), &r);
    ok(!r.ok && strstr(r.error, "outside"), "break outside a loop is an error");
    run("else", &f, t, sizeof(t), &r);
    ok(!r.ok, "a stray else is an error");
    {
        run("echo a\necho b\nend", &f, t, sizeof(t), &r);
        ok(!r.ok && r.line == 3, "the error names the line it happened on");
    }

    // --- a realistic script -------------------------------------------------
    {
        memset(&f, 0, sizeof(f));
        RpsHost h = make(&f);
        const char *src =
            "# back up, then report\n"
            "set count 0\n"
            "while lt $count 2\n"
            "  cp /etc/passwd /tmp/backup$count\n"
            "  inc count\n"
            "end\n"
            "if exists /there\n"
            "  echo backup verified\n"
            "else\n"
            "  echo backup missing\n"
            "end\n";
        rps_run(src, &h, &r);
        ok(r.ok, "a realistic script runs clean");
        ok(f.n == 3, "and runs exactly the commands it should");
        ok(!strcmp(f.trace[0], "cp /etc/passwd /tmp/backup0"), "loop body, first pass");
        ok(!strcmp(f.trace[1], "cp /etc/passwd /tmp/backup1"), "loop body, second pass");
        ok(!strcmp(f.trace[2], "echo backup verified"), "the true branch after it");
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
