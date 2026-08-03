// Ctrl+C handling.
//
// The rule that matters: a command must see the interrupt, and the NEXT command
// must not. Getting that wrong either makes Ctrl+C useless (never seen) or makes
// the shell unusable (every command dies on a keypress from a minute ago).

#include "interrupt.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ck(bool c, const char *m) {
    checks++;
    if (!c) { fails++; printf("  FAIL: %s\n", m); }
}

// A scripted input stream for intr_check to scan.
static const char *g_stream;
static int         g_at;
static int         g_polls;

static int fake_poll(void) {
    g_polls++;
    if (!g_stream || !g_stream[g_at]) return -1;
    return (unsigned char)g_stream[g_at++];
}

static void feed(const char *s) { g_stream = s; g_at = 0; }

static char flood[4096];

int main(void) {
    memset(flood, 'x', sizeof(flood) - 1);
    flood[sizeof(flood) - 1] = 0;

    intr_set_poll(fake_poll);

    // --- the flag ----------------------------------------------------------
    intr_clear();
    ck(!intr_pending(), "clear leaves nothing pending");
    intr_raise();
    ck(intr_pending(), "raise sets it");
    ck(intr_pending(), "pending does not consume the flag");
    intr_clear();
    ck(!intr_pending(), "clear resets it");

    // --- scanning the stream ------------------------------------------------
    feed("");
    ck(!intr_check(), "an empty stream is not an interrupt");

    feed("hello");
    ck(!intr_check(), "ordinary bytes are not an interrupt");

    feed("\x03");
    ck(intr_check(), "a bare Ctrl+C is seen");
    ck(intr_pending(), "and it stays pending for the caller");
    intr_clear();

    // Ctrl+C behind other keystrokes still has to be found — someone mashing
    // keys before hitting Ctrl+C is the normal case, not the exception.
    feed("abc\x03");
    ck(intr_check(), "Ctrl+C after other bytes is found");
    intr_clear();

    // Once raised, a check must not need to find the byte again: the poll
    // stream is empty by then, and the flag is what carries the state forward.
    feed("\x03");
    intr_check();
    feed("");
    ck(intr_check(), "the flag survives once the byte is consumed");
    intr_clear();

    // --- the shell's contract ------------------------------------------------
    // A stray Ctrl+C at the prompt must not kill the command typed after it.
    // The shell clears before dispatch; this is that sequence.
    intr_raise();
    intr_clear();                      // shell: before dispatching
    feed("");
    ck(!intr_check(), "an earlier Ctrl+C does not leak into the next command");

    // --- the drain is bounded ------------------------------------------------
    // A command polling in a tight loop must not be able to spin forever inside
    // one intr_check on a flooded input.
    feed(flood);
    g_polls = 0;
    intr_check();
    ck(g_polls <= 33, "one check reads a bounded number of bytes");

    // --- type-ahead must survive ---------------------------------------------
    //
    // A command runs between Enter and the next prompt. Anything typed in that
    // window — or, far more often, the rest of a pasted block — arrives while a
    // COMMAND owns the input. intr_check pulls those bytes off looking for
    // Ctrl+C, and if it discards them the shell appears to drop characters.
    intr_set_poll(fake_poll);
    intr_clear();
    intr_stash_clear();

    feed("ping 1.1.1.1\r");
    intr_check();
    char got[32]; int gn = 0;
    for (int c = intr_stashed(); c >= 0 && gn < 31; c = intr_stashed()) got[gn++] = (char)c;
    got[gn] = 0;
    ck(strcmp(got, "ping 1.1.1.1\r") == 0, "typed-ahead bytes are kept, in order");
    ck(intr_stashed() < 0, "the stash empties");

    // Ctrl+C mid-paste: everything BEFORE it is still real input.
    intr_clear(); intr_stash_clear();
    feed("abc\x03");
    ck(intr_check(), "Ctrl+C after typed input is still seen");
    gn = 0;
    for (int c = intr_stashed(); c >= 0 && gn < 31; c = intr_stashed()) got[gn++] = (char)c;
    got[gn] = 0;
    ck(strcmp(got, "abc") == 0, "input before the Ctrl+C is preserved");
    intr_clear();

    // A flood must not run off the end of the ring.
    intr_stash_clear();
    feed(flood);
    for (int i = 0; i < 20; i++) intr_check();
    gn = 0;
    while (intr_stashed() >= 0 && gn < 4096) gn++;
    ck(gn > 0 && gn < 200, "the stash is bounded and still readable");

    intr_stash_clear();
    ck(intr_stashed() < 0, "stash_clear empties it");

    // --- no poll installed ---------------------------------------------------
    intr_set_poll(nullptr);
    intr_clear();
    ck(!intr_check(), "no poll function is safe, not a crash");
    intr_raise();
    ck(intr_check(), "the flag still works without a poll function");
    intr_clear();

    printf("  interrupt: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
