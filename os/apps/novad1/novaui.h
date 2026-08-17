// Desc: The UI leaf — the Screen protocol, the layout tokens, and the shared widgets.
// File: novaui.h
//
// The stable surface everything above builds on. It knows about the canvas and
// the input vocabulary and nothing else — no services, no drivers, no runner —
// which is what lets screens move between files freely as the UI grows.
//
// The layout tokens are DERIVED FROM THE FONT rather than written down, so
// changing the face reflows every screen instead of leaving fifty of them with
// hard-coded pixel positions that were right for the old one.
#ifndef NOVA_UI_H
#define NOVA_UI_H

#include "novacanvas.h"
#include "novainput.h"

namespace nova {
namespace ui {

// --- layout ------------------------------------------------------------------

constexpr int ADV  = FONT_ADV;      // 6 — one character cell
constexpr int FH   = FONT_H;        // 7 — glyph height
constexpr int BARH = FH + 2;        // 9 — the status bar, with a pixel to clear the rule
constexpr int TOP  = BARH + 1;      // 10 — where a screen's body starts
// Row pitch: the glyph plus two pixels of air. On a 64-pixel panel that gives
// six rows, where a tighter pitch gives seven that crowd each other and a looser
// one gives five. Six is the number every list in the suite is designed around.
constexpr int ROWH = FH + 2;        // 9
constexpr int SB_W = 3;             // the scrollbar lane

// The slot the power indicator takes at the right of the status bar: the cell,
// its terminal, and two pixels of air before the clock. One width for all three
// of the states it can be in, so the WiFi bars and the title beside it do not
// shuffle sideways when the power state changes.
constexpr int POWER_W = 16;

// Rows a list can show on this canvas.
inline int rows_for(const Canvas &c) { return (c.height() - TOP) / ROWH; }

// --- the gallery -------------------------------------------------------------
//
// The home screen and the folders inside it are laid out from the BOTTOM up: the
// position pips on the last usable row, the label above them, and the ring of
// icons in what is left.
//
// Here with the rest of the layout rather than inside the gallery because the
// host renderer composes the same frame from the same numbers. Two copies of
// "twenty pixels down from TOP" is how the renderer and the panel drifted apart
// before, and a renderer that disagrees with the device is worse than none.

constexpr int ICON_BIG     = 12;    // the icon under the cursor
constexpr int ICON_SMALL   = 5;     // its neighbours
constexpr int ICON_SPACING = 38;    // between icon centres, for three across
constexpr int PIP_H        = 2;     // a lit pip is two pixels square
constexpr int PIP_PITCH    = 3;     // ...and the air after it

// Blank rows between the top of the body and the top of the big icon. The ring
// used to start eight rows down because the pips took the first two; with the
// pips moved to the foot of the panel it comes up to six, which keeps the air
// above the ring looking the way it did while the clutter under the status bar
// goes away.
constexpr int RING_GAP = 6;
constexpr int RING_Y   = TOP + RING_GAP + ICON_BIG;

// One row of margin is left under the pips deliberately. Nothing on the host can
// see whether the bezel eats the last row of the glass, and a position indicator
// that is half hidden is worse than one a pixel higher.
inline int pip_y(const Canvas &c)   { return c.height() - PIP_H - 1; }
// Two rows of air between the label and the pips: enough that they read as a
// label with an indicator under it rather than as one block.
inline int label_y(const Canvas &c) { return pip_y(c) - FH - 2; }

// --- the Screen protocol ------------------------------------------------------

// What a screen wants to happen after handling a gesture.
enum Action {
    ACT_STAY = 0,   // still here
    ACT_BACK,       // pop one level
    ACT_HOME,       // all the way out to the home screen
};

class Screen {
public:
    // No virtual destructor, deliberately.
    //
    // A virtual destructor puts the DELETING variant in the vtable, and that one
    // calls operator delete — a symbol the firmware does not export, so the
    // package would not build. Screens are placement-constructed into a fixed
    // pool and torn down by an explicit destructor call, so the deleting variant
    // would never be right anyway. Any screen needing cleanup does it in an
    // explicit leave().
    //
    // Nothing here is pure virtual, for the same family of reason: a pure
    // virtual generates a reference to __cxa_pure_virtual, which is also not
    // exported. A default that does nothing is honest and costs one branch.

    // Paint the body, from TOP down. The status bar belongs to the runner and a
    // screen must not draw over it.
    virtual void draw(Canvas &c) { (void)c; }

    // Advance time. Return true ONLY when something changed and the screen needs
    // repainting — a still screen returning false is what lets the device idle
    // at three frames a second instead of sixty, and it is the difference
    // between a day of battery and an afternoon.
    virtual bool tick(uint32_t dt_ms) { (void)dt_ms; return false; }

    // Handle a gesture. The default is the one every screen should have: BACK
    // goes back, HOME goes home. A screen that traps somebody is a bug.
    virtual Action on_event(Event e) {
        if (e == EV_BACK) return ACT_BACK;
        if (e == EV_HOME) return ACT_HOME;
        return ACT_STAY;
    }

    // The name in the status bar.
    virtual const char *title(void) const { return ""; }

    // Currently moving, so the runner should keep the frame rate up. Distinct
    // from tick() returning true: this says "expect more", that says "one
    // changed thing happened".
    virtual bool animating(void) const { return false; }

    // Take the whole panel, status bar included. The splash and the lock screen.
    virtual bool fullscreen(void) const { return false; }

    // HOME does not escape this screen. For a confirmation that has to be
    // answered and for the lock.
    virtual bool modal(void) const { return false; }

    // What the controls do here, one line each, shown on demand.
    //
    // The MicroPython suite spent the bottom row of every screen on a permanent
    // hint like "SEL rec  HOME edit" — a sixth of a six-row panel, given over to
    // text somebody reads once. These are shown on demand instead, and the
    // status bar marks a screen that has them with a '?'. Lists went from five
    // rows to six the day that changed.
    //
    // ON DEMAND means: hold HOME, then Controls, which is the first row of the
    // power menu. It used to say "holding HOME shows these", and holding HOME
    // has only ever opened the power menu — so for a long time the '?' marked a
    // screen whose help nothing on the device could reach. There is no spare
    // gesture to give it (turn, press, back, home and the two holds are all
    // spoken for), so it lives behind the one gesture that already works from
    // anywhere.
    //
    // A dynamic STATUS message is a different thing and still belongs on the
    // screen: "Connected", "Wrong password" is feedback about what just
    // happened, not a manual.
    virtual int help(const char **out, int max) const { (void)out; (void)max; return 0; }

    // Called when this screen becomes the top one, and when it stops being.
    // Anything that has to be released — a radio, a file — goes in leave().
    virtual void enter(void) {}
    virtual void leave(void) {}
};

// --- the list widget ----------------------------------------------------------
//
// Every choice in the suite is one of these. Re-implementing a list rather than
// using it is the single most reliable way to make a screen feel like it came
// from somewhere else.

// An item's action, when it is chosen. Returning ACT_STAY leaves the menu up,
// which is what a toggle does; a submenu pushes itself and also returns ACT_STAY.
typedef Action (*MenuFn)(void *ctx, int index);

struct MenuItem {
    const char *label;
    MenuFn      fn;        // null means the row is inert, and it is drawn as such
    void       *ctx;
};

class Menu : public Screen {
public:
    // The item array belongs to the caller and must outlive the menu. Screens
    // hold theirs as a static, which is the only lifetime that is obviously
    // right on a device with no allocator worth the name.
    //
    // set() PUTS THE CURSOR BACK AT THE TOP, so it belongs in a screen's begin()
    // — the once-only setup — and not in enter(), which runs again every time a
    // child screen pops. A menu that calls set() from enter() sends you back to
    // row zero every time you look at a row's output and come back, which on the
    // twelve-row Commands list means finding your place again after every single
    // command. refresh() is the same call for the screen that has to rebuild its
    // rows on the way in: it keeps the cursor, clamped to the new count.
    void set(const char *title, const MenuItem *items, int count);
    void refresh(const char *title, const MenuItem *items, int count);

    void draw(Canvas &c) override;
    Action on_event(Event e) override;
    const char *title(void) const override { return title_; }

    int selected(void) const { return sel_; }
    void select(int i);

protected:
    const char     *title_;
    const MenuItem *items_;
    int             count_;
    int             sel_;
    int             top_;
};

// --- the slider widget --------------------------------------------------------
//
// A value you scrub with the encoder, drawn as a fill bar with the reading over
// it. One widget for every "how much" setting — brightness, the dim and off
// timeouts, the auto-lock — so they all move and look the same.
//
// TWO modes, because two kinds of "how much" want different treatment:
//   * a RANGE (lo..hi by step) for a true level like brightness, where any point
//     is meaningful;
//   * a set of STOPS for a timeout, where 47 seconds is not a thing anybody
//     wants — the curated list "never, 15s, 30s, 1m, 2m, 5m" is, and the encoder
//     steps between those.
//
// The change is applied LIVE: on_change fires on every detent, so brightness
// dims under your thumb and the owner can save the final value on its own leave.

enum SliderFmt {
    SL_PLAIN,       // the number itself
    SL_PERCENT255,  // a 0..255 level shown 0..100%
    SL_SECONDS,     // seconds shown "never" / "45s" / "5m"
};

typedef void (*SliderFn)(void *ctx, int value);

class Slider : public Screen {
public:
    // A continuous range. `value` is clamped into [lo, hi]. on_change fires on
    // every detent (the owner persists to the registry in RAM there); on_commit
    // fires ONCE on leave, if anything moved (the owner flushes to flash there).
    // Split this way so scrubbing costs no flash and the widget itself stays free
    // of the registry — the flush is the owner's, called back through on_commit.
    void set(const char *title, int value, int lo, int hi, int step,
             SliderFmt fmt, SliderFn on_change, void *ctx, SliderFn on_commit = nullptr);

    // A curated set of stops; the encoder moves between them. `stops` belongs to
    // the caller and must outlive the slider (a static, like a MenuItem array).
    void set_stops(const char *title, int value, const int *stops, int n,
                   SliderFmt fmt, SliderFn on_change, void *ctx, SliderFn on_commit = nullptr);

    void draw(Canvas &c) override;
    Action on_event(Event e) override;
    // Flush the value to flash — ONCE, on the way out. on_change persists each
    // detent to the registry in RAM (cheap); this is the single flash write that
    // makes it survive a reboot, so scrubbing costs no flash and does not lag.
    void leave(void) override;
    const char *title(void) const override { return title_; }

    int help(const char **out, int max) const override {
        if (max < 2) return 0;
        out[0] = "Turn to change it.";
        out[1] = "BACK keeps it.";
        return 2;
    }

private:
    void move(int dir);
    void fmt_value(char *out, unsigned cap) const;

    const char *title_;
    SliderFn    cb_;
    SliderFn    commit_;    // owner's flush-to-flash, called once on leave
    void       *ctx_;
    const int  *stops_;     // null for a continuous range
    int         n_;         // stop count, or 0
    int         idx_;       // current stop, in STOPS mode
    int         val_, lo_, hi_, step_;
    SliderFmt   fmt_;
    bool        moved_;     // did anything change, so leave() need flush
};

// --- helpers ------------------------------------------------------------------

// Break text into lines of at most `cols` characters, at spaces where it can.
// Returns how many lines were produced. `store` is written to and the returned
// pointers point into it, so it has to outlive the use.
int wrap(const char *s, int cols, char *store, unsigned store_cap,
         const char **lines, int max_lines);

// Draw a title bar row for a screen that wants one inside its own body — a
// heading with a rule under it, at the top of the body area.
void heading(Canvas &c, const char *text);

// The power indicator, drawn from `x` and occupying POWER_W. Three states, and
// which one shows is decided by what is actually KNOWN rather than by a guess:
//
//   usb              the USB trident, and no level — on external power a cell
//                    reads as a charging voltage, and drawing that as a level
//                    would be a number that moves for the wrong reason
//   pct < 0          a plain cell outline: powered, and no way to say how much.
//                    The ordinary state of a board whose battery sense pin is
//                    not wired, and it must not show a level it does not have
//   pct >= 0         the cell filled in proportion, with the number inside it
//
// Plain arguments rather than a power::Source, so this file goes on knowing
// about the canvas and nothing else and the host renderer can call it without
// dragging a driver in behind it.
void power_badge(Canvas &c, int x, bool usb, int pct);

}  // namespace ui
}  // namespace nova

#endif  // NOVA_UI_H
