// Desc: The operations screens — power, the health check, repair, and the command list.
// File: novagui_ops.h
//
// A flat sibling of novagui, the same as novagui_tools and novagui_system. What
// these four have in common is that they all ACT on the device rather than
// reporting on it: they stop it, restart it, put it right, or run something.
//
// That is why they share a file. Every one of them ends up needing the same two
// things — a question before anything drastic, and a way to run a command that
// takes seconds without the panel appearing to freeze while it does — and one
// copy of each is worth more than four screens each solving it slightly
// differently.
#ifndef NOVA_GUI_OPS_H
#define NOVA_GUI_OPS_H

namespace nova {
namespace screens {

// Every one of these pushes its screen and returns. They are the App table's
// open functions, so their shape is fixed by it.
void open_power(void);       // screen off, lock, incognito, sleep, reboot, shut down
void open_check(void);       // the startup check, on demand and standing still
void open_repair(void);      // the recovery actions, one press each
void open_commands(void);    // a curated list of commands, and their output

// --- the screen lock ------------------------------------------------------------
//
// Here rather than beside the settings that configure it, because the RUNNER
// arms it — the idle tiers own the clock it goes off on — and the Power menu is
// where somebody reaches for it by hand. The settings screen only writes the
// keys these three read.

// What the lock actually is right now: "PIN", "Password" or "None". Lock type
// keeps its last value after a code is cleared, so this is NOT the same question
// as reading Lock_Kind — and arming a lock with nothing to check against would
// leave a panel with no way in.
const char *lock_state(void);
bool lock_armed(void);       // ...in one word, for the callers that only need it

// Is the lock screen up? The runner asks so that holding HOME does not open the
// power menu over it, which would be a way round it.
bool lock_active(void);

// The lowest depth gui::go_home() may pop to — 1 normally, the lock's own depth
// while it is up. A screen pushed over the lock may ask to go home (the
// keyboard does, from EV_HOME) and must not be able to take the lock with it.
unsigned lock_floor(void);

// Forget it was ever up. gui::begin() calls this: it resets the screen stack to
// nothing, and a lock still marked active across that would leave the device
// with no power menu and a floor pointing at a slot that holds something else.
void lock_forget(void);

// Put it up. Refuses when nothing is set, when it is already up, and when the
// screen on top is modal — a staged update must not be interrupted by it.
void lock_engage(void);

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_OPS_H
