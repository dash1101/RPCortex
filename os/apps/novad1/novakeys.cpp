// Desc: The keyboard, the PIN pad and the confirmation.
// File: novakeys.cpp
#include "novakeys.h"
#include "novagui.h"
#include "novacore.h"

#include "rpc_app.h"
#include <stdio.h>
#include <string.h>

namespace nova {
namespace ui {

// --- the keyboard -----------------------------------------------------------------
//
// Ported from the MicroPython KeyboardScreen, grid and all. Ten columns because
// the panel is 128 pixels and the font advance is 6: ten cells of 12 fill it
// with two to spare, and the digits are exactly ten so they line up one per
// column rather than wrapping into a ragged row.

static const char *const kRows[] = {
    "abcdefghij",
    "klmnopqrst",
    "uvwxyz.-_@",
    "0123456789",
};
constexpr int KB_ROWS = 4;
constexpr int KB_COLS = 10;

// The bottom row. Everything a keyboard needs that is not a character, on the
// only row that has space for words.
static const char *const kActions[] = { "SHF", "SPC", "DEL", "OK" };
constexpr int KB_ACTS = 4;

// The longest thing anybody types here is a WiFi password, and WPA2 caps those
// at 63 characters. One more for the terminator.
constexpr unsigned TEXT_MAX = 64;

class KeyboardScreen : public Screen {
public:
    // No constructor: this is placement-constructed into the pool and the pool
    // is bss. begin() is what a constructor would have been, and calling it is
    // the caller's job — see keyboard() at the bottom of this section.
    void begin(const char *title, const char *initial, bool secret,
               TextFn done, VoidFn cancel, void *ctx) {
        title_  = title ? title : "Enter text";
        len_    = initial ? copy(text_, TEXT_MAX, initial) : 0;
        text_[len_] = 0;
        sel_    = 0;
        shift_  = false;
        secret_ = secret;
        done_   = done;
        cancel_ = cancel;
        ctx_    = ctx;
        blink_  = 0;
        caret_  = true;
    }

    const char *title(void) const override { return title_; }

    int help(const char **out, int max) const override {
        if (max < 3) return 0;
        out[0] = "Turn to move, press to type.";
        out[1] = "Hold SELECT to accept.";
        out[2] = "BACK deletes; empty backs out.";
        return 3;
    }

    // The caret has to keep blinking, so this screen is never still. It is the
    // one place in the suite that spends frames on decoration, and it earns
    // them: a text field with no caret gives no sign it is waiting for input.
    bool animating(void) const override { return true; }

    bool tick(uint32_t dt) override {
        blink_ += dt;
        if (blink_ < 450) return false;
        blink_ = 0;
        caret_ = !caret_;
        return true;
    }

    void draw(Canvas &c) override {
        // The buffer, masked when it is a secret. Only the tail is shown: what
        // matters while typing is the end, and 19 characters is what fits.
        char shown[24];
        unsigned show_from = len_ > 19 ? len_ - 19 : 0;
        unsigned n = 0;
        for (unsigned i = show_from; i < len_ && n < sizeof(shown) - 1; i++)
            shown[n++] = secret_ ? '*' : text_[i];
        shown[n] = 0;

        c.text(2, TOP, shown, 1);
        if (caret_) c.vline(2 + c.text_width(shown), TOP, FH, 1);

        const int gw = c.width() / KB_COLS;
        const int aw = c.width() / KB_ACTS;
        const int y0 = TOP + FH + 2;
        const int gh = (c.height() - y0) / (KB_ROWS + 1);

        for (int i = 0; i < key_count(); i++) {
            int r, col;
            const char *label = key_at(i, &r, &col);
            bool action = (r == KB_ROWS);
            int cw = action ? aw : gw;
            int x  = col * cw;
            int y  = y0 + r * gh;
            bool on = (i == sel_);
            if (on) c.rounded_rect(x, y - 1, cw - 1, gh, 1, true);

            if (label[0] == 'S' && label[1] == 'P' && label[2] == 'C') {
                space_key(c, x, y, cw, gh, on);
                continue;
            }
            char up[4];
            if (!action && shift_) { up[0] = (char)(label[0] - 32); up[1] = 0; label = up; }
            c.text(x + 1, y, label, on ? 0 : 1);
        }
    }

    Action on_event(Event e) override {
        const int n = key_count();
        if (e == EV_ROT_CW)  { sel_ = (sel_ + 1) % n; return ACT_STAY; }
        if (e == EV_ROT_CCW) { sel_ = (sel_ + n - 1) % n; return ACT_STAY; }

        // Holding SELECT accepts from wherever the cursor is. Reaching OK means
        // turning past up to forty keys, and the shortcut is the difference
        // between a keyboard and a chore.
        if (e == EV_SELECT_HOLD) return finish();

        if (e == EV_SELECT) {
            int r, col;
            const char *k = key_at(sel_, &r, &col);
            if (r < KB_ROWS)              { type(k[0]); return ACT_STAY; }
            if (!strcmp(k, "SHF"))        { shift_ = !shift_; return ACT_STAY; }
            if (!strcmp(k, "SPC"))        { type(' '); return ACT_STAY; }
            if (!strcmp(k, "DEL"))        { backspace(); return ACT_STAY; }
            return finish();                                     // OK
        }

        // BACK deletes, and only leaves once there is nothing left to delete.
        // Backing straight out of a half-typed password is the mistake this
        // prevents, and it is one somebody makes exactly once before it annoys
        // them permanently.
        if (e == EV_BACK) {
            if (len_) { backspace(); return ACT_STAY; }
            if (cancel_) cancel_(ctx_);
            return ACT_BACK;
        }
        if (e == EV_HOME) {
            if (cancel_) cancel_(ctx_);
            return ACT_HOME;
        }
        return ACT_STAY;
    }

private:
    const char *title_;
    char        text_[TEXT_MAX];
    unsigned    len_;
    int         sel_;
    bool        shift_, secret_, caret_;
    uint32_t    blink_;
    TextFn      done_;
    VoidFn      cancel_;
    void       *ctx_;

    static int key_count(void) { return KB_ROWS * KB_COLS + KB_ACTS; }

    // Index to (row, column, label). The grid is regular apart from the action
    // row, so this is arithmetic rather than a built table — forty-four entries
    // of three fields each is 500 bytes of a package that is counting them.
    static const char *key_at(int i, int *row, int *col) {
        static char one[2];
        if (i < KB_ROWS * KB_COLS) {
            *row = i / KB_COLS;
            *col = i % KB_COLS;
            one[0] = kRows[*row][*col];
            one[1] = 0;
            return one;
        }
        *row = KB_ROWS;
        *col = i - KB_ROWS * KB_COLS;
        return kActions[*col];
    }

    // The space bar as the open box a real keyboard shows, rather than the word.
    // Three glyphs of "SPC" in a cell this size is unreadable; a mark is not.
    static void space_key(Canvas &c, int x, int y, int w, int h, bool on) {
        int colour = on ? 0 : 1;
        int mid = y + h / 2;
        int x0 = x + 3, x1 = x + w - 4;
        c.vline(x0, mid - 1, 3, colour);
        c.vline(x1, mid - 1, 3, colour);
        c.hline(x0, mid + 2, x1 - x0 + 1, colour);
    }

    void type(char ch) {
        if (len_ + 1 >= TEXT_MAX) return;
        if (shift_ && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        text_[len_++] = ch;
        text_[len_] = 0;
    }

    void backspace(void) {
        if (len_) text_[--len_] = 0;
    }

    Action finish(void) {
        if (done_) done_(ctx_, text_);
        return ACT_BACK;
    }
};

void keyboard(const char *title, const char *initial, bool secret,
              TextFn done, VoidFn cancel, void *ctx) {
    KeyboardScreen *s = gui::push<KeyboardScreen>();
    if (s) s->begin(title, initial, secret, done, cancel, ctx);
}

// --- the PIN pad ------------------------------------------------------------------
//
// Six digits, turned rather than typed. SELECT advances and wraps, so the whole
// code is reachable without a second button; HOME submits, because by then
// there is nothing else left for it to mean.

class PinScreen : public Screen {
public:
    void begin(const char *title, TextFn done, VoidFn cancel, void *ctx) {
        title_ = title ? title : "PIN";
        for (int i = 0; i < 6; i++) digit_[i] = 0;
        pos_   = 0;
        done_  = done;
        cancel_ = cancel;
        ctx_   = ctx;
    }

    const char *title(void) const override { return title_; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Turn to set, press to move.";
        out[1] = "HOME enters it.";
        return 2;
    }

    void draw(Canvas &c) override {
        // Big digits, because this is read at arm's length and a six-character
        // string in the body font is smaller than the thing it is guarding.
        const int scale = 2;
        const int cw = FONT_ADV * scale + 4;
        const int x0 = (c.width() - cw * 6) / 2;
        const int y  = TOP + 8;
        for (int i = 0; i < 6; i++) {
            char d[2] = { (char)('0' + digit_[i]), 0 };
            c.text(x0 + i * cw + 2, y, d, 1, scale);
            // The cursor is a rule under the digit rather than a box round it:
            // a box at this size crowds the glyph and reads as selection, which
            // is not what is happening — one digit is being edited.
            if (i == pos_) c.hline(x0 + i * cw, y + FH * scale + 2, cw - 2, 1);
        }
        if (msg_[0]) c.text_centred(c.height() - FH - 1, msg_, 1);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW)  { digit_[pos_] = (digit_[pos_] + 1) % 10; return ACT_STAY; }
        if (e == EV_ROT_CCW) { digit_[pos_] = (digit_[pos_] + 9) % 10; return ACT_STAY; }
        if (e == EV_SELECT)  { pos_ = (pos_ + 1) % 6; return ACT_STAY; }
        if (e == EV_HOME || e == EV_SELECT_HOLD) {
            char pin[7];
            for (int i = 0; i < 6; i++) pin[i] = (char)('0' + digit_[i]);
            pin[6] = 0;
            if (done_) done_(ctx_, pin);
            return ACT_BACK;
        }
        if (e == EV_BACK) {
            if (pos_ > 0) { pos_--; return ACT_STAY; }
            if (cancel_) cancel_(ctx_);
            return ACT_BACK;
        }
        return ACT_STAY;
    }

private:
    const char *title_;
    int         digit_[6];
    int         pos_;
    char        msg_[16];
    TextFn      done_;
    VoidFn      cancel_;
    void       *ctx_;
};

void pinpad(const char *title, TextFn done, VoidFn cancel, void *ctx) {
    PinScreen *s = gui::push<PinScreen>();
    if (s) s->begin(title, done, cancel, ctx);
}

// --- confirmation and notice ------------------------------------------------------

class ConfirmScreen : public Screen {
public:
    void begin(const char *question, const char *yes_label, VoidFn yes, void *ctx) {
        question_ = question;
        yes_label_ = yes_label ? yes_label : "Yes";
        yes_ = yes;
        ctx_ = ctx;
        on_yes_ = false;            // NO by default; see the header
    }

    const char *title(void) const override { return "Confirm"; }

    // HOME must not walk out of a question. Everything that asks one is in the
    // middle of something that needs an answer either way.
    bool modal(void) const override { return true; }

    // Turning to move between two buttons is this screen's only gesture and it
    // is nowhere else on the device — every other list moves a cursor down
    // rows. It is worth one line of help, and the '?' that comes with it.
    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Turn between No and Yes,";
        out[1] = "SELECT answers. BACK is No.";
        return 2;
    }

    void draw(Canvas &c) override {
        static char store[96];
        const char *lines[4];
        int n = wrap(question_, c.cols() - 1, store, sizeof(store), lines, 4);
        for (int i = 0; i < n; i++) c.text(2, TOP + i * ROWH, lines[i], 1);

        const int y = c.height() - ROWH;
        button(c, 4, y, "No", !on_yes_);
        button(c, c.width() / 2 + 2, y, yes_label_, on_yes_);
    }

    Action on_event(Event e) override {
        if (e == EV_ROT_CW || e == EV_ROT_CCW) { on_yes_ = !on_yes_; return ACT_STAY; }
        if (e == EV_SELECT) {
            if (on_yes_ && yes_) yes_(ctx_);
            return ACT_BACK;
        }
        if (e == EV_BACK) return ACT_BACK;
        return ACT_STAY;
    }

private:
    const char *question_;
    const char *yes_label_;
    bool        on_yes_;
    VoidFn      yes_;
    void       *ctx_;

    static void button(Canvas &c, int x, int y, const char *label, bool on) {
        int w = c.width() / 2 - 6;
        if (on) c.rounded_rect(x, y - 1, w, ROWH, 1, true);
        else    c.rounded_rect(x, y - 1, w, ROWH, 1, false);
        c.text_centred_in(x, w, y, label, on ? 0 : 1);
    }
};

void confirm(const char *question, const char *yes_label, VoidFn yes, void *ctx) {
    ConfirmScreen *s = gui::push<ConfirmScreen>();
    if (s) s->begin(question, yes_label, yes, ctx);
}

// The message, held OUTSIDE the screen.
//
// A pool slot is 384 bytes and this was a hundred and twenty-eight of them —
// a third of a screen, spent on text that is read once and dismissed. There is
// only ever one notice up, so a file static is the same bytes in a cheaper
// place, and it is the same reasoning the app picker's buffer and the device
// listing already sit outside their screens for.
//
// It also means what the device last said is readable, which is what lets a
// host test check that a confirmation reported the right thing — a screen that
// says "Clock set." for a clock it did not set is a bug no compiler can see.
static char g_notice_body[128];

const char *last_notice(void) { return g_notice_body; }

class NoticeScreen : public Screen {
public:
    void begin(const char *title, const char *body) {
        title_ = title;
        copy(g_notice_body, sizeof(g_notice_body), body ? body : "");
    }

    const char *title(void) const override { return title_; }

    void draw(Canvas &c) override {
        static char store[128];
        const char *lines[5];
        int n = wrap(g_notice_body, c.cols() - 1, store, sizeof(store), lines, 5);
        for (int i = 0; i < n; i++) c.text(2, TOP + i * ROWH, lines[i], 1);
    }

    // Any gesture dismisses it. A notice that needs the right button pressed is
    // a question wearing a notice's clothes.
    Action on_event(Event e) override {
        if (e == EV_NONE) return ACT_STAY;
        return ACT_BACK;
    }

private:
    const char *title_;
};

void notice(const char *title, const char *body) {
    NoticeScreen *s = gui::push<NoticeScreen>();
    if (s) s->begin(title, body);
}

}  // namespace ui
}  // namespace nova
