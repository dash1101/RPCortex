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

// The apps BUILT IN to this package, and how many. The home-screen picker
// edits these and only these, deliberately: a Hidden list written before
// somebody installed an app must not be able to hide it.
const App *apps(void);
unsigned   app_count(void);

// The built-ins AND whatever is in /nova/apps, as one sequence. Everything that
// asks "what can this device open" goes through these two rather than indexing
// apps() to app_count(), because a third-party app is an app.
unsigned   app_total(void);
const App *app_at(unsigned i);

// Bring the catalogue back in step with what is in /nova/apps, if anything has
// said it changed.
//
// THE RUNNER'S TASK ONLY, or with the runner down. It rebuilds the arrays a
// Gallery is drawing out of and rewrites the labels it is holding pointers
// into. A shell command marks napps dirty and lets the home screen do this on
// its own task; when there is no home, it can do it itself.
void refresh_apps(void);

// Which row's open() is running.
//
// An OpenFn takes nothing and returns nothing, so an opener shared between
// several rows — the folders, and now every installed app — has no other way to
// know which one was chosen. The Gallery sets this immediately before the call;
// anything else that opens an app the way a person would has to set it too, or
// the opener reads whatever was chosen last.
void chose(const App *a);
const App *chosen(void);

// Is this app usable right now? An app whose module is absent is still LISTED —
// a device somebody has not finished wiring should say what is missing rather
// than hide the feature — but it is drawn greyed and does not open.
bool app_available(const App &a);

// --- running -------------------------------------------------------------------

// Bring up the panel, the input and the home screen.
//
// FALSE means one of two things and the caller has to tell them apart with
// started(): either there is no panel — the common half-built case, which has to
// be said out loud — or a runner is already up, in which case starting a second
// is not a smaller version of starting one, it is two tasks writing one screen
// stack.
bool begin(void);

// Has begin() been called and not yet finished tearing down? Distinct from
// running(), which is only true once the loop is actually turning.
bool started(void);

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

// How long to hand tick(), given the wall clock, where the last frame was, and
// whether a gesture was just handled. Advances *last.
//
// A FUNCTION rather than three lines inside run(), because the harness drives
// the runner with its own copy of the loop — and a rule that exists in two
// places is a rule the test cannot check. This one went wrong in a way that
// survived two attempts at fixing it: dt is the gap since the last frame, the
// frame before a gesture is the idle one, and NAP_IDLE happens to equal
// SLIDE_MS, so the first tick of a slide consumed the whole animation and one
// detent always arrived as a jump.
uint32_t frame_dt(uint32_t now, uint32_t *last, bool had_input);

// Is the screen lock due, `idle_s` seconds after the last gesture?
//
// A FUNCTION for the same reason frame_dt is one: the rule belongs to the runner
// and a harness that reimplements it is a harness testing its own copy. Three
// things have to be true and only one of them is a clock — the timeout is set,
// it has elapsed, and there is actually a code to unlock with. Arming without
// the last of those leaves a panel nothing can get back into.
bool lock_due(uint32_t idle_s);

// May holding HOME open the power menu right now?
//
// It is meant to work from ANY screen — that is the guarantee, and it is why
// lock and shutdown are one gesture from anywhere. The lock is the one
// exception, and it has to be one: Screen off, Reboot and Incognito are all on
// that menu, so a power menu over the lock is a way round it.
//
// A function, not an `if` inside the loop, so the exception can be checked. The
// harness drives the runner with its own copy of the loop and a rule that only
// exists there is a rule nothing tests.
bool power_gesture_ok(void);

}  // namespace gui
}  // namespace nova

#endif  // NOVA_GUI_H
