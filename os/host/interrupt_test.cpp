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

int main(void) {
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
    static char flood[4096];
    memset(flood, 'x', sizeof(flood) - 1);
    flood[sizeof(flood) - 1] = 0;
    feed(flood);
    g_polls = 0;
    intr_check();
    ck(g_polls <= 33, "one check reads a bounded number of bytes");

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
