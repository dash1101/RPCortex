// The DuckyScript reader: each command turns into the right events, a comment
// into none, an unknown command into an error rather than keystrokes, and a
// runaway file stops when asked. The emitter is a recorder, so the test sees
// exactly what would have been typed.
#include "../core/ducky.h"
#include "../core/hidkey.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", what); fails++; }
}

// The recording emitter. Every callback appends one line to a log so a whole
// script's output can be compared as text.
struct Rec {
    char log[4096];
    int  len;
    int  stop_after;   // stop() returns true once this many events have logged
    int  events;
};
static void rec_append(Rec *r, const char *s) {
    int n = (int)strlen(s);
    if (r->len + n + 1 < (int)sizeof(r->log)) { memcpy(r->log + r->len, s, n); r->len += n; r->log[r->len] = 0; }
    r->events++;
}
static void e_key(void *c, uint8_t mod, uint8_t kc) {
    char b[48]; snprintf(b, sizeof(b), "KEY %02x %02x\n", mod, kc); rec_append((Rec *)c, b);
}
static void e_text(void *c, const char *s) {
    char b[300]; snprintf(b, sizeof(b), "TEXT %s\n", s); rec_append((Rec *)c, b);
}
static void e_delay(void *c, uint32_t ms) {
    char b[48]; snprintf(b, sizeof(b), "DELAY %u\n", ms); rec_append((Rec *)c, b);
}
static int e_stop(void *c) {
    Rec *r = (Rec *)c;
    return r->stop_after && r->events >= r->stop_after;
}

static DuckyEmit emit(Rec *r) { return DuckyEmit{ e_key, e_text, e_delay, e_stop, r }; }

// Run a script and return its event log.
static const char *run(Rec *r, const char *script, int stop_after = 0) {
    memset(r, 0, sizeof(*r));
    r->stop_after = stop_after;
    DuckyState st; DuckyEmit em = emit(r);
    ducky_run(script, &st, &em);
    return r->log;
}

int main(void) {
    printf("ducky_test - DuckyScript reader\n");
    Rec r; DuckyState st;

    // STRING types its text verbatim; STRINGLN adds an Enter.
    ck(!strcmp(run(&r, "STRING hello world"), "TEXT hello world\n"), "STRING is verbatim");
    ck(!strcmp(run(&r, "STRINGLN hi"), "TEXT hi\nKEY 00 28\n"), "STRINGLN adds Enter (0x28)");

    // REM and blank lines produce nothing.
    ck(run(&r, "REM this is a comment")[0] == 0, "REM emits nothing");
    ck(run(&r, "\n\n  \n")[0] == 0, "blank lines emit nothing");

    // ENTER and the named keys.
    { DuckyEmit em = emit(&r); memset(&r,0,sizeof r);
      ducky_run_line("ENTER", &st, &em); ck(!strcmp(r.log, "KEY 00 28\n"), "ENTER -> 0x28"); }

    // GUI r -> Windows key + r, no shift (the case is preserved).
    ck(!strcmp(run(&r, "GUI r"), "KEY 08 15\n"), "GUI r -> mod 0x08, key 0x15");
    // The classic three-finger salute.
    ck(!strcmp(run(&r, "CTRL ALT DELETE"), "KEY 05 4c\n"), "CTRL ALT DELETE -> mod ctrl|alt, key 0x4c");
    // A lone modifier is a valid tap.
    ck(!strcmp(run(&r, "GUI"), "KEY 08 00\n"), "lone GUI -> mod only");
    // SHIFT TAB.
    ck(!strcmp(run(&r, "SHIFT TAB"), "KEY 02 2b\n"), "SHIFT TAB -> mod shift, key 0x2b");
    // A shifted character in a chord ORs its shift in.
    ck(!strcmp(run(&r, "GUI R"), "KEY 0a 15\n"), "GUI R -> mod gui|shift (R is shifted)");
    // Function key.
    ck(!strcmp(run(&r, "ALT F4"), "KEY 04 3d\n"), "ALT F4 -> mod alt, key 0x3d");

    // DELAY.
    ck(!strcmp(run(&r, "DELAY 500"), "DELAY 500\n"), "DELAY 500");

    // DEFAULTDELAY: a delay after every LATER command, and the DEFAULTDELAY line
    // itself is not paced.
    ck(!strcmp(run(&r, "DEFAULTDELAY 100\nSTRING a\nENTER"),
               "TEXT a\nDELAY 100\nKEY 00 28\nDELAY 100\n"),
       "DEFAULTDELAY paces subsequent commands, not itself");

    // An unknown command is refused, not typed, and named as the error line.
    { memset(&r,0,sizeof r); DuckyEmit em = emit(&r);
      ducky_run("STRING ok\nFLOOB the cat\nSTRING more", &st, &em);
      ck(st.error_line == 2, "unknown command flags its line");
      ck(strstr(r.log, "FLOOB") == nullptr, "unknown command is not typed"); }

    // The stop callback aborts between lines — a runaway payload is interruptible.
    { const char *big = "STRING 1\nSTRING 2\nSTRING 3\nSTRING 4\nSTRING 5\n";
      run(&r, big, /*stop_after=*/2);
      ck(r.events <= 3, "stop() halts the run early"); }

    // An over-long line truncates rather than overrunning; a huge file is capped.
    { char huge[600]; memcpy(huge, "STRING ", 7);
      for (int i = 7; i < 590; i++) huge[i] = 'x';
      huge[590] = 0;
      memset(&r,0,sizeof r); DuckyEmit em = emit(&r);
      ducky_run(huge, &st, &em);
      ck(r.events >= 1, "an over-long line still runs, bounded"); }

    printf(fails ? "  %d checks, %d FAILED\n" : "  %d checks\n", checks, fails);
    return fails ? 1 : 0;
}
