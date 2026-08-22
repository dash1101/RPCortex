// The DuckyScript reader. See ducky.h.
#include "ducky.h"
#include "hidkey.h"

#include <string.h>

// A bounded copy of one token (a whitespace-delimited word), RAW — case is
// preserved, because a single-character key in a chord (the r in GUI r) is
// taken literally and must not be folded to R and pick up a shift. Command and
// key NAMES are matched case-insensitively by upper-casing a copy where needed.
// Returns the length and advances *p past the token.
static int next_token(const char **p, char *out, int cap) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    int n = 0;
    while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') {
        if (n < cap - 1) out[n++] = *s;
        s++;
    }
    out[n] = 0;
    *p = s;
    return n;
}

// An upper-cased copy, for matching a keyword regardless of how it was typed.
static void upcopy(char *dst, const char *src, int cap) {
    int n = 0;
    for (; src[n] && n < cap - 1; n++) {
        char c = src[n];
        dst[n] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    dst[n] = 0;
}

// Parse a non-negative decimal. Returns -1 for anything that is not all digits.
static long parse_uint(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return -1;
    long v = 0;
    for (; *s && *s != '\r' && *s != '\n'; s++) {
        if (*s == ' ' || *s == '\t') break;
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 3600000) return 3600000;   // an hour is already absurd; clamp
    }
    return v;
}

// A modifier name. Returns its bit, or 0 if the token is not a modifier.
static uint8_t modifier_bit(const char *t) {
    if (!strcmp(t, "CTRL") || !strcmp(t, "CONTROL")) return HID_MOD_CTRL;
    if (!strcmp(t, "ALT"))                            return HID_MOD_ALT;
    if (!strcmp(t, "SHIFT"))                          return HID_MOD_SHIFT;
    if (!strcmp(t, "GUI") || !strcmp(t, "WINDOWS") || !strcmp(t, "WIN"))
        return HID_MOD_GUI;
    return 0;
}

// A named non-character key. Returns its keycode, or 0 if the token is not one.
static uint8_t named_key(const char *t) {
    if (!strcmp(t, "ENTER") || !strcmp(t, "RETURN")) return HK_ENTER;
    if (!strcmp(t, "ESC") || !strcmp(t, "ESCAPE"))   return HK_ESC;
    if (!strcmp(t, "TAB"))                            return HK_TAB;
    if (!strcmp(t, "SPACE"))                          return HK_SPACE;
    if (!strcmp(t, "BACKSPACE"))                      return HK_BACKSPACE;
    if (!strcmp(t, "DELETE") || !strcmp(t, "DEL"))    return HK_DELETE;
    if (!strcmp(t, "INSERT"))                         return HK_INSERT;
    if (!strcmp(t, "HOME"))                           return HK_HOME;
    if (!strcmp(t, "END"))                            return HK_END;
    if (!strcmp(t, "PAGEUP"))                         return HK_PAGEUP;
    if (!strcmp(t, "PAGEDOWN"))                       return HK_PAGEDOWN;
    if (!strcmp(t, "CAPSLOCK"))                       return HK_CAPSLOCK;
    if (!strcmp(t, "PRINTSCREEN"))                    return HK_PRINTSCR;
    if (!strcmp(t, "MENU") || !strcmp(t, "APP"))      return HK_MENU;
    if (!strcmp(t, "UP") || !strcmp(t, "UPARROW"))    return HK_UP;
    if (!strcmp(t, "DOWN") || !strcmp(t, "DOWNARROW")) return HK_DOWN;
    if (!strcmp(t, "LEFT") || !strcmp(t, "LEFTARROW")) return HK_LEFT;
    if (!strcmp(t, "RIGHT") || !strcmp(t, "RIGHTARROW")) return HK_RIGHT;
    // F1..F12
    if (t[0] == 'F' && t[1] >= '1' && t[1] <= '9') {
        int n = t[1] - '0';
        if (t[2] >= '0' && t[2] <= '9') n = n * 10 + (t[2] - '0');
        if (n >= 1 && n <= 12) return (uint8_t)(HK_F1 + n - 1);
    }
    return 0;
}

// A chord line: zero or more modifiers and at most one key, in any order
// (GUI r, CTRL ALT DELETE, SHIFT TAB, or a lone GUI). Emits one key event.
static int run_chord(const char *rest, const DuckyEmit *e) {
    uint8_t mod = 0, kc = 0;
    char tok[24], up[24];
    const char *p = rest;
    while (next_token(&p, tok, sizeof(tok)) > 0) {
        upcopy(up, tok, sizeof(up));
        uint8_t mb = modifier_bit(up);
        if (mb) { mod |= mb; continue; }
        uint8_t nk = named_key(up);
        if (nk) { kc = nk; continue; }
        // A single printable character, e.g. the 'r' in GUI r — taken with its
        // literal case (tok, not the upper-cased copy). Its own shift, if it is
        // a symbol, rides along with any modifiers named on the line.
        if (tok[0] && !tok[1]) {
            uint8_t ck = 0, sh = 0;
            if (hid_ascii_to_keycode(tok[0], &ck, &sh)) {
                kc = ck;
                if (sh) mod |= HID_MOD_SHIFT;
                continue;
            }
        }
        return -1;   // an unrecognised token: refuse rather than type garbage
    }
    e->key(e->ctx, mod, kc);   // a lone modifier (kc==0) is a valid tap too
    return 0;
}

int ducky_run_line(const char *line, DuckyState *st, const DuckyEmit *e) {
    char raw[24], cmd[24];
    const char *p = line;
    if (next_token(&p, raw, sizeof(raw)) == 0) return DUCKY_NOPACE;   // blank
    upcopy(cmd, raw, sizeof(cmd));

    // The rest of the line, verbatim, for STRING — case and spaces preserved.
    const char *rest = p;
    while (*rest == ' ' || *rest == '\t') rest++;

    // Return DUCKY_NOPACE for a line that is not a keystroke — a comment, a
    // blank, or DEFAULTDELAY configuration — so the runner does not insert a
    // default delay after it. Real commands return DUCKY_OK (0), which the
    // runner paces.
    if (!strcmp(cmd, "REM") || raw[0] == '#') return DUCKY_NOPACE;

    if (!strcmp(cmd, "STRING")) { e->text(e->ctx, rest); return DUCKY_OK; }
    if (!strcmp(cmd, "STRINGLN")) {
        e->text(e->ctx, rest);
        e->key(e->ctx, 0, HK_ENTER);
        return DUCKY_OK;
    }
    if (!strcmp(cmd, "DELAY")) {
        long ms = parse_uint(rest);
        if (ms < 0) return DUCKY_ERR;
        e->delay(e->ctx, (uint32_t)ms);
        return DUCKY_OK;
    }
    if (!strcmp(cmd, "DEFAULTDELAY") || !strcmp(cmd, "DEFAULT_DELAY")) {
        long ms = parse_uint(rest);
        if (ms < 0) return DUCKY_ERR;
        st->default_delay = (uint32_t)ms;
        return DUCKY_NOPACE;
    }

    // Anything else is a chord: put the command token back at the front so a
    // lone key (ENTER) and a modifier line (CTRL ALT DELETE) run one code path.
    return run_chord(line, e);
}

int ducky_run(const char *script, DuckyState *st, const DuckyEmit *e) {
    st->default_delay = 0;
    st->error_line = 0;
    st->lines = 0;
    if (!script) return 0;

    char line[256];
    const char *s = script;
    int lineno = 0;
    // A backstop against a pathological file; a real payload is far under this.
    const int MAX_LINES = 4096;

    while (*s && lineno < MAX_LINES) {
        lineno++;
        // Copy one line (up to the newline), truncating an over-long one rather
        // than overrunning — a 300-character STRING is unusual but must not
        // smash the stack.
        int n = 0;
        while (*s && *s != '\n') {
            if (n < (int)sizeof(line) - 1) line[n++] = *s;
            s++;
        }
        line[n] = 0;
        if (*s == '\n') s++;

        if (e->stop && e->stop(e->ctx)) break;

        // The line itself decides whether it is paced: DUCKY_OK is a keystroke
        // and gets the default delay after it; a comment, blank, or DEFAULTDELAY
        // line reports DUCKY_NOPACE and is not paced; DUCKY_ERR flags the line.
        int rc = ducky_run_line(line, st, e);
        if (rc == DUCKY_ERR && st->error_line == 0)
            st->error_line = lineno;
        st->lines++;

        if (rc == DUCKY_OK && st->default_delay)
            e->delay(e->ctx, st->default_delay);
    }
    return st->lines;
}
