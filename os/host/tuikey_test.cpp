// Terminal input decoding, fed the way a serial line actually delivers it.
//
// Every sequence here is also fed one byte at a time with unrelated work in
// between, because that is what a 115200 baud line does and a decoder that only
// works on whole sequences works only in tests.
#include "tuikey.h"
#include "rpc_app.h"    // the ABI's FW_KEY_* must agree with the TuiKey values here

#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
static void ok(bool c, const char *what) {
    checks++;
    if (!c) { printf("    FAIL %s\n", what); fails++; }
}

// Feed a whole string; return the last non-NONE event.
static TuiEvent feed(const char *s, uint32_t now = 100) {
    TuiKeyParser p; tuikey_init(&p);
    TuiEvent last{}; last.kind = TUI_EV_NONE;
    for (const char *c = s; *c; c++) {
        TuiEvent e = tuikey_feed(&p, (uint8_t)*c, now);
        if (e.kind != TUI_EV_NONE) last = e;
    }
    return last;
}

int main(void) {
    printf("  tuikey\n");

    // --- plain keys ---------------------------------------------------------
    { TuiEvent e = feed("a"); ok(e.kind == TUI_EV_KEY && e.key == 'a', "a plain letter"); }
    { TuiEvent e = feed("\r"); ok(e.key == '\r', "carriage return"); }
    { TuiEvent e = feed("\x03"); ok(e.key == 3, "Ctrl+C arrives as a byte"); }
    // Backspace: terminals disagree, and both must survive to the caller so it
    // can decide. Deciding here would bake in one terminal's convention.
    { TuiEvent e = feed("\x7f"); ok(e.key == 0x7f, "DEL passes through"); }
    { TuiEvent e = feed("\x08"); ok(e.key == 0x08, "BS passes through"); }

    // --- arrows and navigation ---------------------------------------------
    { TuiEvent e = feed("\033[A"); ok(e.key == TUI_KEY_UP,    "up arrow"); }
    { TuiEvent e = feed("\033[B"); ok(e.key == TUI_KEY_DOWN,  "down arrow"); }
    { TuiEvent e = feed("\033[C"); ok(e.key == TUI_KEY_RIGHT, "right arrow"); }
    { TuiEvent e = feed("\033[D"); ok(e.key == TUI_KEY_LEFT,  "left arrow"); }
    { TuiEvent e = feed("\033OA"); ok(e.key == TUI_KEY_UP,    "up arrow, SS3 form"); }
    { TuiEvent e = feed("\033[H"); ok(e.key == TUI_KEY_HOME,  "home"); }
    { TuiEvent e = feed("\033[5~"); ok(e.key == TUI_KEY_PGUP, "page up"); }
    { TuiEvent e = feed("\033[3~"); ok(e.key == TUI_KEY_DELETE, "delete"); }
    { TuiEvent e = feed("\033[15~"); ok(e.key == TUI_KEY_F5,  "F5, two-digit parameter"); }
    { TuiEvent e = feed("\033[24~"); ok(e.key == TUI_KEY_F12, "F12"); }

    // --- modifiers ----------------------------------------------------------
    {
        // Word-wise movement depends on these being distinguishable.
        TuiEvent e = feed("\033[1;5C");
        ok(e.key == TUI_KEY_CTRL_RIGHT && e.ctrl, "Ctrl+Right");
        e = feed("\033[1;5D");
        ok(e.key == TUI_KEY_CTRL_LEFT && e.ctrl, "Ctrl+Left");
        e = feed("\033[1;2C");
        ok(e.key == TUI_KEY_RIGHT && e.shift && !e.ctrl, "Shift+Right stays Right");
        e = feed("\033[1;6C");
        ok(e.key == TUI_KEY_CTRL_RIGHT && e.ctrl && e.shift, "Ctrl+Shift+Right");
        e = feed("\033[3;5~");
        ok(e.key == TUI_KEY_DELETE && e.ctrl, "Ctrl+Delete");
    }
    { TuiEvent e = feed("\033x"); ok(e.key == 'x' && e.alt, "ESC then a letter is Alt"); }

    // --- mouse --------------------------------------------------------------
    {
        // SGR: "\033[<button;col;rowM". Coordinates are 1-based on the wire and
        // zero-based everywhere above it.
        TuiEvent e = feed("\033[<0;10;5M");
        ok(e.kind == TUI_EV_MOUSE, "a click is a mouse event");
        ok(e.mouse == TUI_MOUSE_DOWN, "button press");
        ok(e.x == 9 && e.y == 4, "coordinates convert from 1-based to 0-based");

        e = feed("\033[<0;10;5m");
        ok(e.mouse == TUI_MOUSE_UP, "lowercase m is a release");

        e = feed("\033[<32;3;7M");
        ok(e.mouse == TUI_MOUSE_DRAG, "bit 5 marks a drag");

        e = feed("\033[<64;1;1M");
        ok(e.mouse == TUI_MOUSE_WHEEL_UP, "wheel up");
        e = feed("\033[<65;1;1M");
        ok(e.mouse == TUI_MOUSE_WHEEL_DOWN, "wheel down");

        // Coordinates past 95, which the original 1981 encoding cannot express
        // at all — the reason SGR is used.
        e = feed("\033[<0;200;150M");
        ok(e.x == 199 && e.y == 149, "large coordinates survive");

        e = feed("\033[<16;5;5M");
        ok(e.ctrl, "ctrl+click");
        e = feed("\033[<4;5;5M");
        ok(e.shift, "shift+click");
    }

    // --- split across reads -------------------------------------------------
    {
        // The property that matters: a sequence arriving one byte at a time,
        // which is exactly what a serial line does.
        const char *seqs[] = {"\033[A", "\033[1;5D", "\033[<0;10;5M", "\033[15~", "\033OP"};
        int expect[] = {TUI_KEY_UP, TUI_KEY_CTRL_LEFT, -1, TUI_KEY_F5, TUI_KEY_F1};
        bool all = true;
        for (int i = 0; i < 5; i++) {
            TuiKeyParser p; tuikey_init(&p);
            TuiEvent last{}; last.kind = TUI_EV_NONE;
            int events = 0;
            for (const char *c = seqs[i]; *c; c++) {
                TuiEvent e = tuikey_feed(&p, (uint8_t)*c, 100);
                if (e.kind != TUI_EV_NONE) { last = e; events++; }
            }
            // Exactly one event, at the end — never a partial one part-way.
            if (events != 1) all = false;
            if (expect[i] >= 0 && last.key != expect[i]) all = false;
            if (expect[i] < 0 && last.kind != TUI_EV_MOUSE) all = false;
        }
        ok(all, "every sequence yields exactly one event, byte by byte");
    }

    // --- escape -------------------------------------------------------------
    {
        // A lone ESC is indistinguishable from the start of a sequence until
        // time passes. Without the timeout, Escape appears to do nothing until
        // some unrelated key is pressed.
        TuiKeyParser p; tuikey_init(&p);
        TuiEvent e = tuikey_feed(&p, 0x1b, 1000);
        ok(e.kind == TUI_EV_NONE, "a lone ESC waits");
        e = tuikey_timeout(&p, 1000 + TUIKEY_ESC_MS - 1);
        ok(e.kind == TUI_EV_NONE, "and keeps waiting inside the window");
        e = tuikey_timeout(&p, 1000 + TUIKEY_ESC_MS);
        ok(e.kind == TUI_EV_KEY && e.key == TUI_KEY_ESCAPE, "then becomes Escape");
    }

    // --- the ABI names these same numbers -----------------------------------
    //
    // fw_tui_poll hands a package e.key with no translation, so the ABI's
    // FW_KEY_* have to BE the TuiKey values a package will actually receive. The
    // test above just proved a lone Escape arrives as TUI_KEY_ESCAPE; this is
    // where the door's name for it is checked against that, rather than against
    // itself. FW_KEY_ESC read 279 while a real Escape came in as 282 for exactly
    // as long as nothing compared the two — so this is the comparison, and the
    // static_asserts in api.cpp are the same check on the board build.
    ok(FW_KEY_UP     == TUI_KEY_UP,     "FW_KEY_UP matches the terminal layer");
    ok(FW_KEY_DOWN   == TUI_KEY_DOWN,   "FW_KEY_DOWN matches");
    ok(FW_KEY_LEFT   == TUI_KEY_LEFT,   "FW_KEY_LEFT matches");
    ok(FW_KEY_RIGHT  == TUI_KEY_RIGHT,  "FW_KEY_RIGHT matches");
    ok(FW_KEY_HOME   == TUI_KEY_HOME,   "FW_KEY_HOME matches");
    ok(FW_KEY_END    == TUI_KEY_END,    "FW_KEY_END matches");
    ok(FW_KEY_PGUP   == TUI_KEY_PGUP,   "FW_KEY_PGUP matches");
    ok(FW_KEY_PGDN   == TUI_KEY_PGDN,   "FW_KEY_PGDN matches");
    ok(FW_KEY_INSERT == TUI_KEY_INSERT, "FW_KEY_INSERT matches");
    ok(FW_KEY_DELETE == TUI_KEY_DELETE, "FW_KEY_DELETE matches");
    ok(FW_KEY_ESC    == TUI_KEY_ESCAPE, "FW_KEY_ESC matches a real Escape (was 279, is 282)");
    {
        // The timeout must NOT fire for a real sequence still arriving.
        TuiKeyParser p; tuikey_init(&p);
        tuikey_feed(&p, 0x1b, 1000);
        tuikey_feed(&p, '[', 1005);
        TuiEvent e = tuikey_timeout(&p, 2000);
        ok(e.kind == TUI_EV_NONE, "a sequence in progress never times out as Escape");
    }

    // --- malformed input ----------------------------------------------------
    {
        // Garbage must not wedge the parser: the next real key still works.
        TuiKeyParser p; tuikey_init(&p);
        for (int i = 0; i < 40; i++) tuikey_feed(&p, (uint8_t)'0', 100);
        TuiEvent e = tuikey_feed(&p, 'z', 100);
        ok(e.kind == TUI_EV_KEY && e.key == 'z', "an over-long sequence is dropped, not sticky");
    }
    { TuiEvent e = feed("\033[999Z"); ok(e.kind == TUI_EV_NONE, "an unknown final byte yields nothing"); }
    {
        // The parameters must be malformed WITHIN the parameter bytes
        // (0x30-0x3f). A letter is a valid CSI final byte, so "\033[<bad..."
        // legitimately ends the sequence at 'b' and the rest are ordinary
        // keypresses — correct behaviour, and worth learning from a test that
        // asserted otherwise.
        TuiEvent e = feed("\033[<1M");
        ok(e.kind == TUI_EV_NONE, "a mouse report missing its coordinates is dropped");
        e = feed("\033[<0;5M");
        ok(e.kind == TUI_EV_NONE, "a mouse report missing its row is dropped");
    }

    // --- the enable strings -------------------------------------------------
    ok(strstr(TUI_MOUSE_ON, "1006") != nullptr, "SGR encoding is requested");
    ok(strstr(TUI_MOUSE_ON, "1002") != nullptr, "drag reporting is requested");
    // Leaving a terminal reporting means it sends escape sequences to the shell
    // for every click after the app exits.
    ok(strstr(TUI_MOUSE_OFF, "1000l") != nullptr, "and there is a way to turn it off");

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
