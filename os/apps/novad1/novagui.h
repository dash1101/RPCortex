// Desc: The runner — the screen stack, the status bar, and the home screen.
// File: novagui.h
//
// Orchestration for the UI. It owns the panel, the canvas, the screen stack and
// the frame timing; screens own everything inside their own body and nothing
// outside it.
#ifndef NOVA_GUI_H
#define NOVA_GUI_H

#include "novaui.h"
#include "novamodtab.h"

namespace nova {
namespace gui {

// --- the screen stack ---------------------------------------------------------
//
// Screens are placement-constructed into a fixed pool rather than allocated.
// A package's whole heap is 12 KB and it is reset between calls into the
// package, so an allocated screen would be both scarce and short-lived; a pool
// in bss is neither, and the depth is bounded by construction rather than by
// hoping.
constexpr unsigned STACK_MAX  = 8;
constexpr unsigned SLOT_BYTES = 384;

// Push a screen of type T, constructed in place. The static_assert names the
// type when one grows too large, which is a far better error than a pool
// overrun that shows up as the screen underneath being corrupted.
template <typename T>
T *push(void);

void pop(void);
void go_home(void);
ui::Screen *top(void);
unsigned depth(void);

// Mark the current frame dirty, from outside a draw. A background task that
// changes something the screen is showing calls this rather than waiting for
// the screen's own tick to notice.
void invalidate(void);

// --- the app catalogue ---------------------------------------------------------

typedef void (*OpenFn)(void);

struct App {
    const char *key;        // "cc1101" — the registry name, and the icon key
    const char *label;      // "Sub-GHz"
    Category    cat;
    OpenFn      open;       // null while the screen is not written yet
    const char *module;     // the module it needs, or null for a built-in
};

const App *apps(void);
unsigned   app_count(void);

// Is this app usable right now? An app whose module is absent is still LISTED —
// a device somebody has not finished wiring should say what is missing rather
// than hide the feature — but it is drawn greyed and does not open.
bool app_available(const App &a);

// --- running -------------------------------------------------------------------

// Bring up the panel, the input and the home screen. False when there is no
// panel, which is the common half-built case and has to be said out loud.
bool begin(void);

// The loop. Returns when stop() is called or the task is asked to end.
void run(void);
void stop(void);
bool running(void);

// What the last second of frames cost, for the Resources screen and for `d1
// perf`. A frame rate nobody can read on the device is not a claim.
struct Perf {
    uint32_t frames;        // in the last second
    uint32_t draw_us;       // the last frame's compose time
    uint32_t draw_us_max;
    uint32_t push_us;       // and its push
    unsigned pages;         // pages the last push actually sent, of 8
};
const Perf &perf(void);

Canvas &canvas(void);

}  // namespace gui
}  // namespace nova

#endif  // NOVA_GUI_H
