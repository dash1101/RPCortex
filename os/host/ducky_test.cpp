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
static void e_hold(void *c, uint8_t mod, uint8_t kc) {
    char b[48]; snprintf(b, sizeof(b), "HOLD %02x %02x\n", mod, kc); rec_append((Rec *)c, b);
}
static void e_release(void *c, uint8_t mod, uint8_t kc) {
    char b[48]; snprintf(b, sizeof(b), "REL %02x %02x\n", mod, kc); rec_append((Rec *)c, b);
}
static void e_altstring(void *c, const char *s) {
    char b[300]; snprintf(b, sizeof(b), "ALT %s\n", s); rec_append((Rec *)c, b);
}

static DuckyEmit emit(Rec *r) {
    return DuckyEmit{ e_key, e_text, e_delay, e_stop, e_hold, e_release, e_altstring, r };
}

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

    // --- Flipper extensions ---------------------------------------------------

    // HOLD / RELEASE: a modifier held, then let go. Bare RELEASE lets go of all.
    ck(!strcmp(run(&r, "HOLD SHIFT"), "HOLD 02 00\n"), "HOLD SHIFT holds the modifier");
    ck(!strcmp(run(&r, "HOLD a"), "HOLD 00 04\n"), "HOLD a holds a regular key");
    ck(!strcmp(run(&r, "RELEASE"), "REL 00 00\n"), "bare RELEASE lets go of everything");
    ck(!strcmp(run(&r, "RELEASE SHIFT"), "REL 02 00\n"), "RELEASE SHIFT lets go of just it");
    // A held modifier while other keys are typed is the point of HOLD.
    ck(!strcmp(run(&r, "HOLD CTRL\nSTRING ab\nRELEASE"),
               "HOLD 01 00\nTEXT ab\nREL 00 00\n"),
       "HOLD spans the keystrokes between it and RELEASE");

    // ALTSTRING / ALTCHAR type via the alt-code path, not as normal keys.
    ck(!strcmp(run(&r, "ALTSTRING hi"), "ALT hi\n"), "ALTSTRING routes to the alt-code emitter");
    ck(!strcmp(run(&r, "ALTCHAR A"), "ALT A\n"), "ALTCHAR is the same path");

    // SYSRQ b: Alt + SysRq(PrintScreen) held, the key tapped, then all released.
    ck(!strcmp(run(&r, "SYSRQ b"), "HOLD 04 00\nHOLD 00 46\nKEY 00 05\nREL 00 00\n"),
       "SYSRQ builds the magic-SysRq chord");
    // SYSRQ with no key is refused, not half-sent.
    { memset(&r,0,sizeof r); DuckyEmit em = emit(&r);
      ducky_run_line("SYSRQ", &st, &em);
      ck(r.log[0] == 0, "SYSRQ with no key emits nothing"); }

    // WAIT_FOR_BUTTON_PRESS is recognised and skipped — NOT flagged as an error,
    // and it types nothing, so a Flipper payload that pauses runs on.
    { memset(&r,0,sizeof r); DuckyEmit em = emit(&r);
      ducky_run("STRING a\nWAIT_FOR_BUTTON_PRESS\nSTRING b", &st, &em);
      ck(st.error_line == 0, "WAIT_FOR_BUTTON_PRESS is not an error");
      ck(!strcmp(r.log, "TEXT a\nTEXT b\n"), "and emits nothing itself"); }

    // REM_BLOCK / END_REM: a multi-line comment types nothing between the markers.
    ck(!strcmp(run(&r, "STRING a\nREM_BLOCK\nSTRING secret\nENTER\nEND_REM\nSTRING b"),
               "TEXT a\nTEXT b\n"),
       "REM_BLOCK swallows everything up to END_REM");

    // REPEAT n: the previous command runs n MORE times (n+1 total).
    ck(!strcmp(run(&r, "STRING x\nREPEAT 2"), "TEXT x\nTEXT x\nTEXT x\n"),
       "REPEAT 2 runs the previous command twice more");
    // REPEAT with nothing before it does nothing (no crash, no phantom key).
    ck(run(&r, "REPEAT 3")[0] == 0, "REPEAT with no history does nothing");
    // A huge REPEAT is capped rather than wedging — it still runs, bounded.
    { const char *big = "STRING y\nREPEAT 99999999";
      run(&r, big);
      ck(r.events > 1 && r.events <= 10002, "a runaway REPEAT is capped"); }

    printf(fails ? "  %d checks, %d FAILED\n" : "  %d checks\n", checks, fails);
    return fails ? 1 : 0;
}
