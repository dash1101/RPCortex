#include "tuikey.h"

#include <string.h>
#include <stdlib.h>

// 1000 enables button reporting, 1002 adds drags, 1006 selects the SGR
// encoding. Turning them OFF on exit is not optional: a terminal left
// reporting sends escape sequences to the shell for every click afterwards.
const char *TUI_MOUSE_ON  = "\033[?1000h\033[?1002h\033[?1006h";
const char *TUI_MOUSE_OFF = "\033[?1006l\033[?1002l\033[?1000l";

static TuiEvent none(void) { TuiEvent e{}; e.kind = TUI_EV_NONE; return e; }

static TuiEvent key(int k, bool ctrl = false, bool shift = false, bool alt = false) {
    TuiEvent e{};
    e.kind = TUI_EV_KEY; e.key = k;
    e.ctrl = ctrl; e.shift = shift; e.alt = alt;
    return e;
}

void tuikey_init(TuiKeyParser *p) { memset(p, 0, sizeof(*p)); }

static void reset(TuiKeyParser *p) { p->len = 0; p->esc_started_ms = 0; }

// CSI modifier parameter: 1 + bit0 shift, bit1 alt, bit2 ctrl. So 5 is ctrl,
// 2 is shift, 6 is ctrl+shift.
static void mods_from(int m, TuiEvent *e) {
    if (m <= 1) return;
    m -= 1;
    e->shift = (m & 1) != 0;
    e->alt   = (m & 2) != 0;
    e->ctrl  = (m & 4) != 0;
}

// "\033[<b;x;yM" (press) or "...m" (release) — the SGR mouse report.
static TuiEvent parse_mouse_sgr(const char *body, char final_ch) {
    TuiEvent e{};
    e.kind = TUI_EV_MOUSE;

    char *end = nullptr;
    long b = strtol(body, &end, 10);
    if (!end || *end != ';') return none();
    long x = strtol(end + 1, &end, 10);
    if (!end || *end != ';') return none();
    long y = strtol(end + 1, &end, 10);

    // Bit 4 ctrl, bit 3 alt, bit 2 shift; bit 5 marks a drag; bit 6 the wheel.
    e.ctrl  = (b & 16) != 0;
    e.alt   = (b & 8)  != 0;
    e.shift = (b & 4)  != 0;

    if (b & 64) {
        e.mouse = (b & 1) ? TUI_MOUSE_WHEEL_DOWN : TUI_MOUSE_WHEEL_UP;
    } else if (final_ch == 'm') {
        e.mouse = TUI_MOUSE_UP;
    } else if (b & 32) {
        e.mouse = TUI_MOUSE_DRAG;
    } else {
        e.mouse = TUI_MOUSE_DOWN;
    }

    // The wire is 1-based; everything above this line counts from zero, and
    // mixing the two puts a click one row off — which looks like a selection
    // bug rather than an off-by-one.
    e.x = (uint16_t)(x > 0 ? x - 1 : 0);
    e.y = (uint16_t)(y > 0 ? y - 1 : 0);
    return e;
}

// Decode a completed CSI sequence: the bytes between "\033[" and its final.
static TuiEvent parse_csi(const char *body, char final_ch) {
    if (body[0] == '<') return parse_mouse_sgr(body + 1, final_ch);

    // "1;5A" style: an optional first parameter, then an optional modifier.
    int p1 = 0, p2 = 0;
    const char *semi = strchr(body, ';');
    p1 = atoi(body);
    if (semi) p2 = atoi(semi + 1);

    TuiEvent e{};
    e.kind = TUI_EV_KEY;

    switch (final_ch) {
        case 'A': e.key = TUI_KEY_UP;    break;
        case 'B': e.key = TUI_KEY_DOWN;  break;
        case 'C': e.key = TUI_KEY_RIGHT; break;
        case 'D': e.key = TUI_KEY_LEFT;  break;
        case 'H': e.key = TUI_KEY_HOME;  break;
        case 'F': e.key = TUI_KEY_END;   break;
        case 'P': e.key = TUI_KEY_F1;    break;
        case 'Q': e.key = TUI_KEY_F2;    break;
        case 'R': e.key = TUI_KEY_F3;    break;
        case 'S': e.key = TUI_KEY_F4;    break;
        case '~':
            switch (p1) {
                case 1: case 7:  e.key = TUI_KEY_HOME;   break;
                case 2:          e.key = TUI_KEY_INSERT; break;
                case 3:          e.key = TUI_KEY_DELETE; break;
                case 4: case 8:  e.key = TUI_KEY_END;    break;
                case 5:          e.key = TUI_KEY_PGUP;   break;
                case 6:          e.key = TUI_KEY_PGDN;   break;
                case 15:         e.key = TUI_KEY_F5;     break;
                case 17:         e.key = TUI_KEY_F6;     break;
                case 18:         e.key = TUI_KEY_F7;     break;
                case 19:         e.key = TUI_KEY_F8;     break;
                case 20:         e.key = TUI_KEY_F9;     break;
                case 21:         e.key = TUI_KEY_F10;    break;
                case 23:         e.key = TUI_KEY_F11;    break;
                case 24:         e.key = TUI_KEY_F12;    break;
                default: return none();
            }
            break;
        default: return none();
    }

    mods_from(p2, &e);

    // Ctrl+arrow gets its own identity, because word-wise movement is common
    // enough that every caller would otherwise repeat the same check.
    if (e.ctrl) {
        switch (e.key) {
            case TUI_KEY_LEFT:  e.key = TUI_KEY_CTRL_LEFT;  break;
            case TUI_KEY_RIGHT: e.key = TUI_KEY_CTRL_RIGHT; break;
            case TUI_KEY_UP:    e.key = TUI_KEY_CTRL_UP;    break;
            case TUI_KEY_DOWN:  e.key = TUI_KEY_CTRL_DOWN;  break;
            default: break;
        }
    }
    return e;
}

TuiEvent tuikey_feed(TuiKeyParser *p, uint8_t b, uint32_t now_ms) {
    // Not in a sequence: an ordinary byte, or the start of one.
    if (p->len == 0) {
        if (b == 0x1b) {
            p->buf[p->len++] = b;
            p->esc_started_ms = now_ms ? now_ms : 1;   // 0 would read as "unset"
            return none();
        }
        return key((int)b);
    }

    if (p->len >= TUIKEY_BUF - 1) {
        // Longer than any real sequence. Drop it rather than grow: a runaway
        // buffer here would swallow every key after it.
        reset(p);
        return none();
    }
    p->buf[p->len++] = b;

    // Second byte decides the shape.
    if (p->len == 2) {
        if (b == '[' || b == 'O') return none();       // CSI or SS3, keep going
        // ESC followed by anything else is Alt+that key.
        int k = (int)b;
        reset(p);
        return key(k, false, false, /*alt*/true);
    }

    // SS3: "\033O" then one final byte. F1-F4 on many terminals.
    if (p->buf[1] == 'O') {
        char f = (char)b;
        reset(p);
        switch (f) {
            case 'P': return key(TUI_KEY_F1);
            case 'Q': return key(TUI_KEY_F2);
            case 'R': return key(TUI_KEY_F3);
            case 'S': return key(TUI_KEY_F4);
            case 'H': return key(TUI_KEY_HOME);
            case 'F': return key(TUI_KEY_END);
            case 'A': return key(TUI_KEY_UP);
            case 'B': return key(TUI_KEY_DOWN);
            case 'C': return key(TUI_KEY_RIGHT);
            case 'D': return key(TUI_KEY_LEFT);
            default:  return none();
        }
    }

    // CSI: parameters until a final byte in 0x40..0x7e.
    if (b >= 0x40 && b <= 0x7e) {
        char body[TUIKEY_BUF];
        uint8_t n = (uint8_t)(p->len - 3);          // between "\033[" and the final
        memcpy(body, p->buf + 2, n);
        body[n] = 0;
        char final_ch = (char)b;
        reset(p);
        return parse_csi(body, final_ch);
    }
    return none();
}

TuiEvent tuikey_timeout(TuiKeyParser *p, uint32_t now_ms) {
    // A lone ESC looks exactly like the start of a sequence until either the
    // next byte arrives or enough time passes. Without this, pressing Escape
    // appears to do nothing until some unrelated key is pressed.
    if (p->len == 1 && p->buf[0] == 0x1b && p->esc_started_ms &&
        now_ms - p->esc_started_ms >= TUIKEY_ESC_MS) {
        reset(p);
        return key(TUI_KEY_ESCAPE);
    }
    return none();
}
