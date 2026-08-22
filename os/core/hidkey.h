// A US keyboard's ASCII-to-keycode map, as a pure function.
//
// The device could call TinyUSB's HID_ASCII_TO_KEYCODE table directly, but then
// the mapping would only exist where TinyUSB does — off the host, untestable,
// and a typed payload that comes out as garbage is a silent failure with no way
// to catch it before it is on a real machine. So the map lives here, in plain
// C both the firmware and the host tests compile, and mirrors the same US
// layout TinyUSB uses (verified against sdk/.../class/hid/hid.h). The keycodes
// are the HID Usage IDs; the shift flag says whether Left-Shift is held.
//
// The letters and digits are laid out by rule rather than a 62-entry table, so
// only the punctuation — where a US keyboard's shift pairings actually live —
// is written out and has to be right.
#ifndef RPC_HIDKEY_H
#define RPC_HIDKEY_H

#include <stdint.h>

// HID keyboard modifier bits (report byte 0). Left variants; a payload that
// needs the right-hand keys is not a thing DuckyScript expresses.
#define HID_MOD_CTRL   0x01
#define HID_MOD_SHIFT  0x02
#define HID_MOD_ALT    0x04
#define HID_MOD_GUI    0x08

// The Usage IDs used below and by the DuckyScript key names. Named rather than
// bare hex so a wrong one reads wrong.
#define HK_A          0x04   // A..Z run 0x04..0x1D
#define HK_1          0x1E   // 1..9 run 0x1E..0x26
#define HK_0          0x27
#define HK_ENTER      0x28
#define HK_ESC        0x29
#define HK_BACKSPACE  0x2A
#define HK_TAB        0x2B
#define HK_SPACE      0x2C
#define HK_MINUS      0x2D
#define HK_EQUAL      0x2E
#define HK_LBRACKET   0x2F
#define HK_RBRACKET   0x30
#define HK_BACKSLASH  0x31
#define HK_SEMICOLON  0x33
#define HK_APOSTROPHE 0x34
#define HK_GRAVE      0x35
#define HK_COMMA      0x36
#define HK_PERIOD     0x37
#define HK_SLASH      0x38
#define HK_CAPSLOCK   0x39
#define HK_F1         0x3A   // F1..F12 run 0x3A..0x45
#define HK_PRINTSCR   0x46
#define HK_INSERT     0x49
#define HK_HOME       0x4A
#define HK_PAGEUP     0x4B
#define HK_DELETE     0x4C   // forward delete (DuckyScript DELETE)
#define HK_END        0x4D
#define HK_PAGEDOWN   0x4E
#define HK_RIGHT      0x4F
#define HK_LEFT       0x50
#define HK_DOWN       0x51
#define HK_UP         0x52
#define HK_MENU       0x65   // the "application"/context-menu key

// Map one printable ASCII character to a keycode + shift. Returns false for
// anything outside 0x20..0x7E (control characters and the top bit set), which
// the caller turns into "nothing to type" rather than a wrong key.
static inline int hid_ascii_to_keycode(char ch, uint8_t *kc, uint8_t *shift) {
    unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c > 0x7e) return 0;

    if (c >= 'a' && c <= 'z') { *kc = (uint8_t)(HK_A + (c - 'a')); *shift = 0; return 1; }
    if (c >= 'A' && c <= 'Z') { *kc = (uint8_t)(HK_A + (c - 'A')); *shift = 1; return 1; }
    if (c >= '1' && c <= '9') { *kc = (uint8_t)(HK_1 + (c - '1')); *shift = 0; return 1; }
    if (c == '0')             { *kc = HK_0; *shift = 0; return 1; }

    uint8_t s = 0, k = 0;
    switch (c) {
        case ' ':  k = HK_SPACE;      s = 0; break;
        case '!':  k = HK_1;          s = 1; break;
        case '"':  k = HK_APOSTROPHE; s = 1; break;
        case '#':  k = HK_1 + 2;      s = 1; break;   // shift-3
        case '$':  k = HK_1 + 3;      s = 1; break;   // shift-4
        case '%':  k = HK_1 + 4;      s = 1; break;   // shift-5
        case '&':  k = HK_1 + 6;      s = 1; break;   // shift-7
        case '\'': k = HK_APOSTROPHE; s = 0; break;
        case '(':  k = HK_1 + 8;      s = 1; break;   // shift-9
        case ')':  k = HK_0;          s = 1; break;   // shift-0
        case '*':  k = HK_1 + 7;      s = 1; break;   // shift-8
        case '+':  k = HK_EQUAL;      s = 1; break;
        case ',':  k = HK_COMMA;      s = 0; break;
        case '-':  k = HK_MINUS;      s = 0; break;
        case '.':  k = HK_PERIOD;     s = 0; break;
        case '/':  k = HK_SLASH;      s = 0; break;
        case ':':  k = HK_SEMICOLON;  s = 1; break;
        case ';':  k = HK_SEMICOLON;  s = 0; break;
        case '<':  k = HK_COMMA;      s = 1; break;
        case '=':  k = HK_EQUAL;      s = 0; break;
        case '>':  k = HK_PERIOD;     s = 1; break;
        case '?':  k = HK_SLASH;      s = 1; break;
        case '@':  k = HK_1 + 1;      s = 1; break;   // shift-2
        case '[':  k = HK_LBRACKET;   s = 0; break;
        case '\\': k = HK_BACKSLASH;  s = 0; break;
        case ']':  k = HK_RBRACKET;   s = 0; break;
        case '^':  k = HK_1 + 5;      s = 1; break;   // shift-6
        case '_':  k = HK_MINUS;      s = 1; break;
        case '`':  k = HK_GRAVE;      s = 0; break;
        case '{':  k = HK_LBRACKET;   s = 1; break;
        case '|':  k = HK_BACKSLASH;  s = 1; break;
        case '}':  k = HK_RBRACKET;   s = 1; break;
        case '~':  k = HK_GRAVE;      s = 1; break;
        default:   return 0;
    }
    *kc = k; *shift = s;
    return 1;
}

#endif  // RPC_HIDKEY_H
