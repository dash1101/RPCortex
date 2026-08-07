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

// Rows a list can show on this canvas.
inline int rows_for(const Canvas &c) { return (c.height() - TOP) / ROWH; }

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
    // text somebody reads once. Holding HOME shows these instead, and the status
    // bar marks a screen that has them with a '?'. Lists went from five rows to
    // six the day that changed.
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
    void set(const char *title, const MenuItem *items, int count);

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

// --- helpers ------------------------------------------------------------------

// Break text into lines of at most `cols` characters, at spaces where it can.
// Returns how many lines were produced. `store` is written to and the returned
// pointers point into it, so it has to outlive the use.
int wrap(const char *s, int cols, char *store, unsigned store_cap,
         const char **lines, int max_lines);

// Draw a title bar row for a screen that wants one inside its own body — a
// heading with a rule under it, at the top of the body area.
void heading(Canvas &c, const char *text);

}  // namespace ui
}  // namespace nova

#endif  // NOVA_UI_H
