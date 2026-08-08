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
void open_power(void);       // screen off, incognito, sleep, reboot, shut down
void open_check(void);       // the startup check, on demand and standing still
void open_repair(void);      // the recovery actions, one press each
void open_commands(void);    // a curated list of commands, and their output

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_OPS_H
