// Turning terminal bytes into events, including mouse.
//
// A terminal sends keys as bytes and everything else as escape sequences, which
// arrive one byte at a time down a serial line and can be split anywhere. That
// makes this the same shape of problem as the HTTP parser: incremental, with the
// interesting bugs at the boundaries, and therefore worth keeping pure so those
// boundaries can be provoked directly instead of hoped for.
//
// **Mouse support is real and costs a decoder, not a transport.** A terminal
// told `\x1b[?1000h` `\x1b[?1006h` reports clicks, drags and the wheel as
// ordinary escape sequences on the same line as the keys. PuTTY does it. So a
// scroll wheel in a file list, or clicking a settings row, works over the same
// serial cable already in use — which is the difference between a demonstration
// and something people reach for.
//
// SGR encoding (1006) rather than the original: the 1981 encoding packs
// coordinates into single bytes with a +32 offset, so column 224 and beyond are
// unrepresentable and anything past 95 collides with printable characters. SGR
// sends decimal numbers and has neither problem.
#ifndef RPC_TUIKEY_H
#define RPC_TUIKEY_H

#include <stdint.h>
#include <stdbool.h>

enum TuiEventKind {
    TUI_EV_NONE = 0,     // nothing complete yet; feed more bytes
    TUI_EV_KEY,          // a key, in `key` below
    TUI_EV_MOUSE,
};

// Keys above 255 have no character, so the space above ASCII names them.
enum TuiKey {
    TUI_KEY_UP = 256, TUI_KEY_DOWN, TUI_KEY_LEFT, TUI_KEY_RIGHT,
    TUI_KEY_HOME, TUI_KEY_END, TUI_KEY_PGUP, TUI_KEY_PGDN,
    TUI_KEY_INSERT, TUI_KEY_DELETE,
    TUI_KEY_F1, TUI_KEY_F2, TUI_KEY_F3, TUI_KEY_F4,
    TUI_KEY_F5, TUI_KEY_F6, TUI_KEY_F7, TUI_KEY_F8,
    TUI_KEY_F9, TUI_KEY_F10, TUI_KEY_F11, TUI_KEY_F12,
    // Ctrl+arrow, which terminals send as a modified CSI rather than a
    // distinct key. Word-wise movement depends on these.
    TUI_KEY_CTRL_LEFT, TUI_KEY_CTRL_RIGHT, TUI_KEY_CTRL_UP, TUI_KEY_CTRL_DOWN,
    TUI_KEY_ESCAPE,
};

enum TuiMouseKind {
    TUI_MOUSE_DOWN = 0,
    TUI_MOUSE_UP,
    TUI_MOUSE_DRAG,
    TUI_MOUSE_WHEEL_UP,
    TUI_MOUSE_WHEEL_DOWN,
};

struct TuiEvent {
    uint8_t  kind;         // TuiEventKind
    int      key;          // a character, or a TuiKey
    uint8_t  mouse;        // TuiMouseKind
    uint16_t x, y;         // ZERO-based cell coordinates; the wire is 1-based
    bool     ctrl, shift, alt;
};

#define TUIKEY_BUF 24

struct TuiKeyParser {
    uint8_t  buf[TUIKEY_BUF];
    uint8_t  len;
    uint32_t esc_started_ms;   // for telling a lone ESC from a sequence
};

void tuikey_init(TuiKeyParser *p);

// Feed one byte. Returns an event, or one with kind TUI_EV_NONE when more bytes
// are needed. `now_ms` only matters for the ESC timeout below.
TuiEvent tuikey_feed(TuiKeyParser *p, uint8_t byte, uint32_t now_ms);

// A lone ESC is indistinguishable from the start of a sequence until either the
// next byte arrives or enough time passes without one. Callers poll this so
// pressing Escape does something rather than waiting for an unrelated keypress.
TuiEvent tuikey_timeout(TuiKeyParser *p, uint32_t now_ms);

#define TUIKEY_ESC_MS 40

// What to send a terminal to turn mouse reporting on and off. Off matters: a
// terminal left in reporting mode after an app exits spews escape sequences at
// the shell for every click.
extern const char *TUI_MOUSE_ON;
extern const char *TUI_MOUSE_OFF;

#endif  // RPC_TUIKEY_H
