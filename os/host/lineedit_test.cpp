// The line editor: cursor arithmetic, word boundaries, escape decoding, and a
// full drive of the interactive loop through a scripted keyboard.
//
// None of this is observable on a board you are not holding — a cursor that
// lands one character off looks like "the arrows are a bit weird" and survives
// for months. So the editor's terminal I/O is behind function pointers and the
// whole loop runs here against a fake terminal.

#include "lineedit.h"

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
    fails++;
    printf("  FAIL: %s  (got '%s', want '%s')\n", what, got ? got : "(null)", want);
}

static void eqn(uint32_t got, uint32_t want, const char *what) {
    checks++;
    if (got == want) return;
    fails++;
    printf("  FAIL: %s  (got %u, want %u)\n", what, got, want);
}

// --- word boundaries --------------------------------------------------------

static void t_words(void) {
    const char *s = "cat /pkg/greet.app";
    uint32_t len = (uint32_t)strlen(s);

    eqn(line_word_start(s, len, len), 4,  "word start from the end");
    eqn(line_word_start(s, len, 3),   0,  "word start from inside the first word");
    eqn(line_word_start(s, len, 0),   0,  "word start at 0 stays at 0");

    // A cursor sitting in the gap belongs to the word BEFORE it, which is what
    // makes Ctrl+Backspace delete the word you just typed rather than nothing.
    const char *g = "one   two";
    eqn(line_word_start(g, 9, 5), 0, "from inside a gap, skip back over the gap");

    eqn(line_word_end(s, len, 0),   3,   "word end from the start");
    eqn(line_word_end(s, len, len), len, "word end at the end stays");
    eqn(line_word_end(g, 9, 3),     9,   "word end skips the gap then the word");

    const char *trail = "abc   ";
    eqn(line_word_start(trail, 6, 6), 0, "trailing spaces then a word");
    eqn(line_word_end(trail, 6, 3),   6, "word end past trailing spaces is the end");
}

// --- insert and delete ------------------------------------------------------

static void t_edit(void) {
    char b[16] = "abd";
    uint32_t len = 3;

    len = line_insert(b, len, sizeof(b), 2, 'c');
    eqn(len, 4, "insert grows the length");
    eq(b, "abcd", "insert in the middle");

    len = line_insert(b, len, sizeof(b), 0, 'X');
    eq(b, "Xabcd", "insert at the start");

    len = line_insert(b, len, sizeof(b), len, 'Z');
    eq(b, "XabcdZ", "insert at the end");

    // A full buffer must refuse rather than run past the terminator.
    char full[5] = "abcd";
    uint32_t fl = 4;
    uint32_t got = line_insert(full, fl, sizeof(full), 2, 'x');
    eqn(got, 4, "insert into a full buffer is refused");
    eq(full, "abcd", "a refused insert leaves the buffer alone");

    uint32_t pos = 0;
    char d[16] = "hello";
    len = line_delete_back(d, 5, 5, 1, &pos);
    eqn(len, 4, "backspace shortens");
    eqn(pos, 4, "backspace moves the cursor back");
    eq(d, "hell", "backspace at the end");

    strcpy(d, "hello"); pos = 0;
    len = line_delete_back(d, 5, 2, 2, &pos);
    eq(d, "llo", "delete two from the middle");
    eqn(pos, 0, "cursor lands at the deletion point");

    // Asking to delete more than exists must clamp, not underflow.
    strcpy(d, "hi"); pos = 99;
    len = line_delete_back(d, 2, 1, 50, &pos);
    eqn(len, 1, "over-long delete clamps to the cursor");
    eqn(pos, 0, "clamped delete leaves the cursor at 0");
    eq(d, "i", "clamped delete keeps the tail");

    strcpy(d, "abc");
    len = line_delete_at(d, 3, 1);
    eq(d, "ac", "delete key removes the character under the cursor");
    eqn(line_delete_at(d, 2, 2), 2, "delete at the end does nothing");
}

// --- common prefix ----------------------------------------------------------

static void t_prefix(void) {
    char out[32];
    const char *a[] = {"reboot", "rebuild", "recover"};
    eqn(line_common_prefix(a, 3, out, sizeof(out)), 2, "common prefix length");
    eq(out, "re", "common prefix of three");

    const char *b[] = {"exactly"};
    line_common_prefix(b, 1, out, sizeof(out));
    eq(out, "exactly", "a single candidate completes fully");

    const char *c[] = {"abc", "xyz"};
    line_common_prefix(c, 2, out, sizeof(out));
    eq(out, "", "no shared prefix gives an empty string");

    line_common_prefix(a, 0, out, sizeof(out));
    eq(out, "", "no candidates gives an empty string");

    // One candidate being a prefix of the other stops at the shorter.
    const char *d[] = {"ls", "lsblk"};
    line_common_prefix(d, 2, out, sizeof(out));
    eq(out, "ls", "prefix stops at the shorter candidate");
}

// --- escape decoding --------------------------------------------------------

static void t_escapes(void) {
    ck(line_decode_escape("[A", 2) == KEY_UP,    "CSI up");
    ck(line_decode_escape("[B", 2) == KEY_DOWN,  "CSI down");
    ck(line_decode_escape("[C", 2) == KEY_RIGHT, "CSI right");
    ck(line_decode_escape("[D", 2) == KEY_LEFT,  "CSI left");

    // SS3 — what a terminal in application-cursor mode sends. The old two-byte
    // lookahead could not see this at all, which is one way arrows "do nothing".
    ck(line_decode_escape("OA", 2) == KEY_UP,    "SS3 up");
    ck(line_decode_escape("OD", 2) == KEY_LEFT,  "SS3 left");
    ck(line_decode_escape("OH", 2) == KEY_HOME,  "SS3 home");

    // Ctrl+Arrow is six bytes with a modifier parameter.
    ck(line_decode_escape("[1;5C", 5) == KEY_WORD_RIGHT, "ctrl+right is a word jump");
    ck(line_decode_escape("[1;5D", 5) == KEY_WORD_LEFT,  "ctrl+left is a word jump");
    ck(line_decode_escape("[1;6C", 5) == KEY_WORD_RIGHT, "ctrl+shift+right too");
    ck(line_decode_escape("[1;2C", 5) == KEY_RIGHT,      "shift alone is a plain arrow");

    ck(line_decode_escape("[H", 2)  == KEY_HOME,   "CSI home");
    ck(line_decode_escape("[F", 2)  == KEY_END,    "CSI end");
    ck(line_decode_escape("[3~", 3) == KEY_DELETE, "delete key");
    ck(line_decode_escape("[1~", 3) == KEY_HOME,   "home as a tilde sequence");
    ck(line_decode_escape("[4~", 3) == KEY_END,    "end as a tilde sequence");

    ck(line_decode_escape("[Z", 2) == KEY_UNKNOWN, "an unhandled final byte");
    ck(line_decode_escape("", 0)   == KEY_UNKNOWN, "an empty sequence");
    ck(line_decode_escape("x", 1)  == KEY_UNKNOWN, "not an escape sequence at all");
}

// --- the interactive loop, driven by a scripted keyboard --------------------

struct Fake {
    const char *keys;
    uint32_t    at;
    char        screen[512];
    uint32_t    sn;
};

static int fake_getch(void *ctx, uint32_t) {
    Fake *f = (Fake *)ctx;
    if (f->keys[f->at] == 0) return '\r';       // never block the test
    return (unsigned char)f->keys[f->at++];
}
static void fake_putch(void *ctx, char c) {
    Fake *f = (Fake *)ctx;
    if (f->sn + 1 < sizeof(f->screen)) { f->screen[f->sn++] = c; f->screen[f->sn] = 0; }
}

static const char *kCmds[] = {"reboot", "reg", "rename", "ls", "sysinfo"};

static bool fake_complete(void *, const char *prefix, uint32_t word_start,
                          uint32_t index, char *out, uint32_t cap) {
    if (word_start != 0) return false;
    uint32_t seen = 0;
    for (const char *c : kCmds) {
        if (strncmp(c, prefix, strlen(prefix)) != 0) continue;
        if (seen++ != index) continue;
        snprintf(out, cap, "%s", c);
        return true;
    }
    return false;
}

static const char *kHist[] = {"sysinfo", "ls /pkg"};
static const char *fake_history(void *, int depth) {
    if (depth < 0 || depth >= 2) return nullptr;
    return kHist[depth];
}

// Run the editor over a scripted key sequence and return the resulting line.
static void drive(const char *keys, char *out, uint32_t cap) {
    Fake f{keys, 0, "", 0};
    LineEdit le{};
    le.io.getch = fake_getch;
    le.io.putch = fake_putch;
    le.io.ctx   = &f;
    le.complete = fake_complete;
    le.history  = fake_history;
    le.prompt   = "$ ";
    line_edit(&le, out, cap);
}

static void t_loop(void) {
    char b[64];

    drive("hello\r", b, sizeof(b));
    eq(b, "hello", "plain typing");

    // Left arrow then a character inserts at the cursor, not at the end. This is
    // the whole point of tracking a cursor separately from the length.
    drive("abd\033[Dc\r", b, sizeof(b));
    eq(b, "abcd", "left arrow then insert");

    drive("abc\033[D\033[D\033[DX\r", b, sizeof(b));
    eq(b, "Xabc", "three lefts reach the start");

    // Left past the start must clamp rather than wrap.
    drive("ab\033[D\033[D\033[D\033[DX\r", b, sizeof(b));
    eq(b, "Xab", "left clamps at the start");

    drive("abc\033[D\033[CX\r", b, sizeof(b));
    eq(b, "abcX", "right arrow returns to the end");

    drive("abc\033[H" "X\r", b, sizeof(b));
    eq(b, "Xabc", "home goes to the start");

    drive("abc\033[H\033[F" "X\r", b, sizeof(b));
    eq(b, "abcX", "end goes back to the end");

    // 0x7F is Backspace on PuTTY: one character.
    drive("abcd\x7f\r", b, sizeof(b));
    eq(b, "abc", "backspace deletes one character");

    // 0x08 is Ctrl+Backspace on PuTTY: a whole word.
    drive("one two\x08\r", b, sizeof(b));
    eq(b, "one ", "ctrl+backspace deletes a word");

    drive("one two three\x08\x08\r", b, sizeof(b));
    eq(b, "one ", "two word-deletes remove two words");

    // Delete key removes forward.
    drive("abc\033[D\033[3~\r", b, sizeof(b));
    eq(b, "ab", "delete key removes the character under the cursor");

    // Ctrl+Left is a word jump, then insert lands at the word start.
    drive("one two\033[1;5DX\r", b, sizeof(b));
    eq(b, "one Xtwo", "ctrl+left jumps to the word start");

    drive("one two\033[1;5D\033[1;5CY\r", b, sizeof(b));
    eq(b, "one twoY", "ctrl+right jumps back over the word");

    drive("abc\x15X\r", b, sizeof(b));         // Ctrl+U
    eq(b, "X", "ctrl+u clears to the start of the line");

    drive("abcdef\033[D\033[D\x0b\r", b, sizeof(b));   // Ctrl+K
    eq(b, "abcd", "ctrl+k clears to the end of the line");

    drive("junk\x03" "ok\r", b, sizeof(b));    // Ctrl+C
    eq(b, "ok", "ctrl+c abandons the line");

    // History: one Up recalls the most recent.
    drive("\033[A\r", b, sizeof(b));
    eq(b, "sysinfo", "up recalls the most recent command");
    drive("\033[A\033[A\r", b, sizeof(b));
    eq(b, "ls /pkg", "two ups reach the one before");
    drive("\033[A\033[A\033[B\r", b, sizeof(b));
    eq(b, "sysinfo", "down comes back");
    // Past the oldest must not move or crash.
    drive("\033[A\033[A\033[A\033[A\r", b, sizeof(b));
    eq(b, "ls /pkg", "up past the oldest entry stays put");
    // Down past the live line returns to an empty line.
    drive("\033[A\033[B\r", b, sizeof(b));
    eq(b, "", "down from the newest returns to an empty line");

    // SS3 arrows must work too — same behaviour, different bytes.
    drive("\033OA\r", b, sizeof(b));
    eq(b, "sysinfo", "SS3 up recalls history");
    drive("abd\033ODc\r", b, sizeof(b));
    eq(b, "abcd", "SS3 left moves the cursor");

    // Completion: "reb" is unique, so it completes and adds a space.
    drive("reb\t\r", b, sizeof(b));
    eq(b, "reboot ", "a unique completion finishes the word and adds a space");

    // "re" matches reboot/reg/rename — common prefix is already "re", so the
    // line must not change.
    drive("re\t\r", b, sizeof(b));
    eq(b, "re", "an ambiguous completion leaves the line alone");

    // "ren" is unique among the candidates.
    drive("ren\t\r", b, sizeof(b));
    eq(b, "rename ", "completion of a longer unique prefix");

    // No candidate: the line is untouched and the tab is not inserted.
    drive("zzz\t\r", b, sizeof(b));
    eq(b, "zzz", "a tab with no candidates changes nothing");

    // A tab mid-word, then more typing.
    drive("sys\tx\r", b, sizeof(b));
    eq(b, "sysinfo x", "typing continues after a completion");
}

int main(void) {
    t_words();
    t_edit();
    t_prefix();
    t_escapes();
    t_loop();
    printf("  lineedit: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
