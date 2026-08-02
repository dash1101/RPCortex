#include "lineedit.h"

#include <string.h>
#include <stdio.h>

// --- pure buffer operations -------------------------------------------------

uint32_t line_word_start(const char *s, uint32_t len, uint32_t pos) {
    if (pos > len) pos = len;
    while (pos > 0 && s[pos - 1] == ' ') pos--;          // skip the gap
    while (pos > 0 && s[pos - 1] != ' ') pos--;          // then the word
    return pos;
}

uint32_t line_word_end(const char *s, uint32_t len, uint32_t pos) {
    if (pos > len) return len;
    while (pos < len && s[pos] == ' ') pos++;
    while (pos < len && s[pos] != ' ') pos++;
    return pos;
}

uint32_t line_insert(char *s, uint32_t len, uint32_t cap, uint32_t pos, char c) {
    if (len + 1 >= cap) return len;                      // full; keep the NUL
    if (pos > len) pos = len;
    memmove(s + pos + 1, s + pos, len - pos);
    s[pos] = c;
    len++;
    s[len] = 0;
    return len;
}

uint32_t line_delete_back(char *s, uint32_t len, uint32_t pos, uint32_t count,
                          uint32_t *pos_out) {
    if (pos > len) pos = len;
    if (count > pos) count = pos;
    if (count) {
        memmove(s + pos - count, s + pos, len - pos);
        len -= count;
        pos -= count;
        s[len] = 0;
    }
    if (pos_out) *pos_out = pos;
    return len;
}

uint32_t line_delete_at(char *s, uint32_t len, uint32_t pos) {
    if (pos >= len) return len;
    memmove(s + pos, s + pos + 1, len - pos - 1);
    len--;
    s[len] = 0;
    return len;
}

uint32_t line_common_prefix(const char *const *cands, uint32_t n, char *out, uint32_t cap) {
    if (!n || !cap) { if (cap) out[0] = 0; return 0; }
    uint32_t i = 0;
    while (i + 1 < cap) {
        char c = cands[0][i];
        if (!c) break;
        bool all = true;
        for (uint32_t k = 1; k < n; k++)
            if (cands[k][i] != c) { all = false; break; }
        if (!all) break;
        out[i++] = c;
    }
    out[i] = 0;
    return i;
}

// --- escape decoding --------------------------------------------------------
//
// Two forms reach us:
//   CSI  ESC [ <params> <final>      arrows in normal mode, Home/End/Delete
//   SS3  ESC O <final>               arrows when the terminal is in application
//                                    cursor mode — which PuTTY enters on its own
//                                    in some configurations, and which the old
//                                    two-byte lookahead here could never see.
//
// Ctrl+Arrow is CSI with a modifier parameter: ESC [ 1 ; 5 C. The ";5" is what
// makes it six bytes rather than three, so anything that reads a fixed number of
// bytes after ESC misses it entirely.
int line_decode_escape(const char *seq, uint32_t len) {
    if (len == 0) return KEY_UNKNOWN;

    // SS3: ESC O A/B/C/D, plus Home/End as O H / O F.
    if (seq[0] == 'O' && len >= 2) {
        switch (seq[1]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            default:  return KEY_UNKNOWN;
        }
    }
    if (seq[0] != '[') return KEY_UNKNOWN;

    char final = seq[len - 1];
    // A modifier is present when the parameters contain ';'. Ctrl is 5, and
    // Ctrl+Shift is 6; both mean "by word" here — someone holding an extra
    // modifier meant the same thing.
    bool ctrl = false;
    for (uint32_t i = 1; i + 1 < len; i++)
        if (seq[i] == ';' && (seq[i + 1] == '5' || seq[i + 1] == '6')) ctrl = true;

    switch (final) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return ctrl ? KEY_WORD_RIGHT : KEY_RIGHT;
        case 'D': return ctrl ? KEY_WORD_LEFT  : KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '~':
            // ESC [ n ~ — 1/7 Home, 3 Delete, 4/8 End.
            if (len >= 2) {
                if (seq[1] == '3') return KEY_DELETE;
                if (seq[1] == '1' || seq[1] == '7') return KEY_HOME;
                if (seq[1] == '4' || seq[1] == '8') return KEY_END;
            }
            return KEY_UNKNOWN;
        default: return KEY_UNKNOWN;
    }
}

// --- the interactive loop ---------------------------------------------------

#define COMP_MAX     16      // candidates shown / considered at once
#define COMP_LEN     40
// An escape sequence longer than this is not one we handle; the cap stops a
// stream of junk from being mistaken for a key that never terminates.
#define ESC_MAX      12
// How long to wait for the rest of an escape sequence. The old code allowed
// 3 ms, which is under the gap a USB host can leave between packets when it
// splits a three-byte arrow — that is why arrows appeared to do nothing at all.
#define ESC_TIMEOUT_US 50000

namespace {

struct Term {
    const LineIO *io;
    void put(char c) const { io->putch(io->ctx, c); }
    void puts(const char *s) const { while (*s) put(*s++); }
    void num(uint32_t v) const {
        char b[12]; int n = 0;
        if (!v) { put('0'); return; }
        while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) put(b[--n]);
    }
};

// Redraw from scratch: carriage return, clear to end of line, prompt, text, then
// put the cursor back where it belongs. Simpler and more robust than tracking
// incremental terminal state, and at 115200 a 128-character line is under 12 ms.
void redraw(const Term &t, const char *prompt, const char *buf,
            uint32_t len, uint32_t pos) {
    t.put('\r');
    t.puts("\033[K");
    t.puts(prompt);
    for (uint32_t i = 0; i < len; i++) t.put(buf[i]);
    if (pos < len) {                     // walk the cursor back to its column
        t.puts("\033[");
        t.num(len - pos);
        t.put('D');
    }
}

}  // namespace

uint32_t line_edit(const LineEdit *le, char *buf, uint32_t cap) {
    Term t{&le->io};
    uint32_t len = 0, pos = 0;
    int  browse = -1;                    // -1 = the live line; 0.. = history depth
    bool last_was_tab = false;

    buf[0] = 0;
    t.puts(le->prompt);

    while (true) {
        int c = le->io.getch(le->io.ctx, 0);
        if (c == LE_NO_KEY) continue;

        bool this_is_tab = (c == '\t');

        if (c == '\r' || c == '\n') {
            t.put('\n');
            buf[len] = 0;
            return len;
        }

        // Ctrl+C: abandon the line, as every shell does.
        if (c == 0x03) {
            t.puts("^C\n");
            len = 0; pos = 0; buf[0] = 0;
            t.puts(le->prompt);
            last_was_tab = false;
            continue;
        }

        // PuTTY sends 0x7F for Backspace and 0x08 for Ctrl+Backspace, which is
        // the OPPOSITE of most Linux terminals. PuTTY is how this device is
        // actually used, so that mapping wins — on a terminal that sends 0x08
        // for plain Backspace, backspace will delete a word. Change the two
        // cases below if that is ever the common setup.
        if (c == 0x7F) {                             // Backspace: one character
            len = line_delete_back(buf, len, pos, 1, &pos);
            browse = -1; redraw(t, le->prompt, buf, len, pos);
            last_was_tab = false;
            continue;
        }
        if (c == 0x08 || c == 0x17) {                // Ctrl+Backspace / Ctrl+W
            uint32_t start = line_word_start(buf, len, pos);
            len = line_delete_back(buf, len, pos, pos - start, &pos);
            browse = -1; redraw(t, le->prompt, buf, len, pos);
            last_was_tab = false;
            continue;
        }
        if (c == 0x15) {                             // Ctrl+U: to start of line
            len = line_delete_back(buf, len, pos, pos, &pos);
            browse = -1; redraw(t, le->prompt, buf, len, pos);
            continue;
        }
        if (c == 0x0B) {                             // Ctrl+K: to end of line
            len = pos; buf[len] = 0;
            redraw(t, le->prompt, buf, len, pos);
            continue;
        }
        if (c == 0x01) { pos = 0;   redraw(t, le->prompt, buf, len, pos); continue; }  // Ctrl+A
        if (c == 0x05) { pos = len; redraw(t, le->prompt, buf, len, pos); continue; }  // Ctrl+E

        if (c == 0x1b) {
            // Collect the sequence until its final byte (@ to ~) or the cap.
            // A lone ESC (nothing follows) is ignored rather than swallowing the
            // next keystroke.
            char seq[ESC_MAX];
            uint32_t n = 0;
            while (n < ESC_MAX) {
                int k = le->io.getch(le->io.ctx, ESC_TIMEOUT_US);
                if (k == LE_NO_KEY) break;
                seq[n++] = (char)k;
                if (n == 1 && k == 'O') continue;             // SS3 needs one more
                if (k >= '@' && k <= '~' && !(n == 1 && k == '[')) break;
            }
            if (!n) continue;
            int key = line_decode_escape(seq, n);
            last_was_tab = false;

            switch (key) {
                case KEY_LEFT:  if (pos) pos--;      break;
                case KEY_RIGHT: if (pos < len) pos++; break;
                case KEY_HOME:  pos = 0;             break;
                case KEY_END:   pos = len;           break;
                case KEY_WORD_LEFT:  pos = line_word_start(buf, len, pos); break;
                case KEY_WORD_RIGHT: pos = line_word_end(buf, len, pos);   break;
                case KEY_DELETE: len = line_delete_at(buf, len, pos); browse = -1; break;
                case KEY_UP:
                case KEY_DOWN: {
                    if (!le->history) break;
                    int want = browse + (key == KEY_UP ? 1 : -1);
                    if (want < -1) want = -1;
                    const char *h = (want < 0) ? "" : le->history(le->history_ctx, want);
                    if (!h) break;                    // past the oldest: stay put
                    browse = want;
                    snprintf(buf, cap, "%s", h);
                    len = (uint32_t)strlen(buf);
                    pos = len;
                    break;
                }
                default: break;
            }
            redraw(t, le->prompt, buf, len, pos);
            continue;
        }

        // --- tab completion ---------------------------------------------------
        if (this_is_tab) {
            if (!le->complete) { last_was_tab = false; continue; }
            uint32_t ws = line_word_start(buf, len, pos);
            char prefix[COMP_LEN];
            uint32_t plen = pos - ws;
            if (plen >= sizeof(prefix)) plen = sizeof(prefix) - 1;
            memcpy(prefix, buf + ws, plen);
            prefix[plen] = 0;

            static char  store[COMP_MAX][COMP_LEN];
            const char  *cands[COMP_MAX];
            uint32_t n = 0;
            while (n < COMP_MAX &&
                   le->complete(le->complete_ctx, prefix, ws, n, store[n], COMP_LEN)) {
                cands[n] = store[n];
                n++;
            }

            if (n == 0) { last_was_tab = false; continue; }

            char common[COMP_LEN];
            uint32_t clen = line_common_prefix(cands, n, common, sizeof(common));

            if (clen > plen) {
                // Extend to the unambiguous part. A single match also gets a
                // trailing space, because the next thing typed is always another
                // word and adding it by hand every time is friction.
                for (uint32_t i = plen; i < clen && len + 1 < cap; i++)
                    len = line_insert(buf, len, cap, pos++, common[i]);
                if (n == 1 && len + 1 < cap && (pos == len || buf[pos] != ' '))
                    len = line_insert(buf, len, cap, pos++, ' ');
                redraw(t, le->prompt, buf, len, pos);
                last_was_tab = false;
            } else if (last_was_tab) {
                // Second tab with nothing more to add: show what the choices are,
                // then reprint the line so the user is where they left off.
                t.put('\n');
                for (uint32_t i = 0; i < n; i++) {
                    t.puts("  ");
                    t.puts(cands[i]);
                    if ((i % 4) == 3 || i + 1 == n) t.put('\n');
                }
                if (n == COMP_MAX) t.puts("  ...\n");
                redraw(t, le->prompt, buf, len, pos);
                last_was_tab = false;
            } else {
                last_was_tab = true;      // one more tab lists them
            }
            continue;
        }

        if (c >= 32 && c < 127) {
            uint32_t before = len;
            len = line_insert(buf, len, cap, pos, (char)c);
            if (len != before) {
                pos++;
                browse = -1;
                // Appending at the end is the common case and needs no redraw —
                // echoing the character is enough, and avoids a full line rewrite
                // on every keystroke.
                if (pos == len) t.put((char)c);
                else            redraw(t, le->prompt, buf, len, pos);
            }
        }
        last_was_tab = false;
    }
}
