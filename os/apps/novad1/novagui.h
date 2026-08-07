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

// Placement construction, declared rather than included.
//
// <new> is a freestanding header that is not guaranteed to exist for this
// target, and the only thing wanted from it is this one line. Nothing in this
// package ever calls the ALLOCATING new — there is no operator new and no
// operator delete, deliberately, so a screen that tried would not link.
inline void *operator new(size_t, void *p) noexcept { return p; }

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

// The two halves of a push, so the pool itself stays private to novagui.cpp
// while the template that needs it can live here. A template defined in the .cpp
// cannot be instantiated by the screen files, and moving the pool into the
// header would put a kilobyte of screen storage in every translation unit that
// wanted to push anything.
void *push_slot(void);              // the next free slot, or null when full
void  push_commit(ui::Screen *s);   // adopt what was constructed in it

// Push a screen of type T, constructed in place. The static_assert names the
// type when one grows too large, which is a far better error than a pool
// overrun that shows up as the screen underneath being corrupted.
template <typename T>
T *push(void) {
    static_assert(sizeof(T) <= SLOT_BYTES, "screen too large for a stack slot");
    void *slot = push_slot();
    if (!slot) return nullptr;
    T *s = new (slot) T();
    push_commit(s);
    return s;
}

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
