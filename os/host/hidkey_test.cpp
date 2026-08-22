// The ASCII-to-keycode map: every printable character resolves, and the ones a
// US layout shifts carry the shift. A wrong entry here types the wrong letter
// on a real machine with no error, so the whole table is swept and the shift
// pairings that are easy to get wrong are pinned by name.
#include "../core/hidkey.h"
#include <stdio.h>

static int checks, fails;
static void ck(bool c, const char *what) {
    checks++;
    if (!c) { printf("  FAIL: %s\n", what); fails++; }
}

static bool maps(char ch, uint8_t kc, uint8_t shift) {
    uint8_t k = 0, s = 0;
    if (!hid_ascii_to_keycode(ch, &k, &s)) return false;
    return k == kc && s == shift;
}

int main(void) {
    printf("hidkey_test - ASCII to HID keycode\n");

    // The checks the design calls out by hand.
    ck(maps('a', 0x04, 0), "a -> 0x04, no shift");
    ck(maps('A', 0x04, 1), "A -> 0x04 with shift");
    ck(maps('z', 0x1d, 0), "z -> 0x1d");
    ck(maps('Z', 0x1d, 1), "Z -> 0x1d with shift");
    ck(maps('1', 0x1e, 0), "1 -> 0x1e");
    ck(maps('9', 0x26, 0), "9 -> 0x26");
    ck(maps('0', 0x27, 0), "0 -> 0x27, not 0x1e");
    ck(maps('!', 0x1e, 1), "! -> shift-1");
    ck(maps('@', 0x1f, 1), "@ -> shift-2");
    ck(maps('#', 0x20, 1), "# -> shift-3");
    ck(maps(' ', 0x2c, 0), "space -> 0x2c");
    ck(maps('-', 0x2d, 0), "- -> 0x2d");
    ck(maps('_', 0x2d, 1), "_ -> shift-minus");
    ck(maps('=', 0x2e, 0), "= -> 0x2e");
    ck(maps('+', 0x2e, 1), "+ -> shift-equal");
    ck(maps(';', 0x33, 0), "; -> 0x33");
    ck(maps(':', 0x33, 1), ": -> shift-semicolon");
    ck(maps('/', 0x38, 0), "/ -> 0x38");
    ck(maps('?', 0x38, 1), "? -> shift-slash");
    ck(maps('.', 0x37, 0), ". -> 0x37");
    ck(maps(',', 0x36, 0), ", -> 0x36");
    ck(maps('\'', 0x34, 0), "apostrophe -> 0x34");
    ck(maps('"', 0x34, 1), "quote -> shift-apostrophe");
    ck(maps('\\', 0x31, 0), "backslash -> 0x31");
    ck(maps('|', 0x31, 1), "pipe -> shift-backslash");
    ck(maps('`', 0x35, 0), "grave -> 0x35");
    ck(maps('~', 0x35, 1), "tilde -> shift-grave");

    // Every letter and digit lands in its run, with the right shift.
    for (char c = 'a'; c <= 'z'; c++) ck(maps(c, (uint8_t)(0x04 + c - 'a'), 0), "lowercase run");
    for (char c = 'A'; c <= 'Z'; c++) ck(maps(c, (uint8_t)(0x04 + c - 'A'), 1), "uppercase run");

    // Every printable character resolves; nothing outside does.
    int printable = 0;
    for (int c = 0; c < 256; c++) {
        uint8_t k = 0, s = 0;
        int ok = hid_ascii_to_keycode((char)c, &k, &s);
        if (c >= 0x20 && c <= 0x7e) { if (ok) printable++; }
        else ck(!ok, "control / high byte does not map");
    }
    ck(printable == 95, "all 95 printable ASCII map");

    // A resolved keycode is never zero — a zero would be "no key held", which
    // an emitter would send as an empty tap.
    for (int c = 0x20; c <= 0x7e; c++) {
        uint8_t k = 0, s = 0;
        hid_ascii_to_keycode((char)c, &k, &s);
        ck(k != 0, "no printable maps to keycode 0");
    }

    printf(fails ? "  %d checks, %d FAILED\n" : "  %d checks\n", checks, fails);
    return fails ? 1 : 0;
}
