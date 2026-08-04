// The table of resident loaded apps (packages).
//
// A one-shot app is unloaded the moment app_main returns. A package — one that
// registered shell commands — must stay resident, and therefore must be tracked
// so it can later be unloaded cleanly: its commands removed (cmd_remove_owner)
// and its image freed (app_unload). This table is that record. It is the piece
// that turns "run keeps it until reboot" into a real install/remove lifecycle.
#ifndef RPC_APPS_H
#define RPC_APPS_H

#include "loader.h"
#include "task.h"

// Entering and leaving package code.
//
// Everything a package runs — app_main, and later every command it registered —
// goes through these, and they are the only reason the protection regions ever
// point at anything. Entering describes the package's two halves to the
// hardware: code read-only, data non-executable. Leaving puts back whatever was
// there before, which is usually nothing.
//
// `mem` is filled in by app_enter and handed back to app_leave. Passing it back
// rather than clearing means a package entered from inside another package
// leaves the outer one still protected. Nothing can nest today; this costs one
// stack slot and means it never has to be revisited.
void app_enter(const LoadedApp *app, TaskAppMem *saved, bool *had_saved);
void app_leave(const TaskAppMem *saved, bool had_saved);

// The same, for a command that a package registered — found by the owner token
// the command carries, which is the package's image. False if the owner is not
// a resident package, in which case it is a built-in and nothing is protected.
bool app_enter_owner(const void *owner, TaskAppMem *saved, bool *had_saved);

// Copy a loaded app into the resident table. Returns the stored record (whose
// image/veneer pointers are unchanged and stay valid) or nullptr if full or a
// package of that name is already resident.
LoadedApp *apps_store(const LoadedApp *app);

// Unload a resident package by name: remove its commands, free its image, free
// the slot. Returns false if no such package is loaded, and ALSO if a task is
// currently executing it — ask apps_busy_pid to tell the two apart.
bool apps_unload(const char *name);

// The pid of a task sitting inside this package right now, or -1.
//
// A package's command yields on every ABI call it makes, so the shell can be
// parked in the middle of one indefinitely while something else removes the
// package. Freeing it there hands the heap back memory a task is about to
// return into.
int apps_busy_pid(const char *name);

// Load an app file, run its app_main, and either keep it resident (if it
// registered commands) or unload it. The one place the load-run-resident flow
// lives, shared by `run`, `pkg install`, and boot-time package loading. Returns
// app_main's value, or -1 if the file could not be loaded. `quiet` suppresses
// the per-run chatter (used for boot loading).
int apps_launch(const char *file, int arg, bool quiet);

// Register the `apps` (list) and `unload` commands.
void apps_register(void);

#endif  // RPC_APPS_H
