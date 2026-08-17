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

// Run package code the way this board can: sandboxed where the protection
// hardware allows it, privileged where it does not. The one place that decides,
// so `run`, a registered command and a boot-time load all get the same answer.
int app_run(const LoadedApp *app, int (*fn)(int), int arg);

// The same, for a command a package registered — found by the owner token the
// command carries. `fn` takes argc/argv rather than an int, so it is passed
// through as-is; false means the owner is not a resident package, and the
// caller should just call it.
bool app_run_owner(const void *owner, int (*fn)(int, char **), int argc,
                   char **argv, int *ret);

// Copy a loaded app into the resident table. Returns the stored record (whose
// image/veneer pointers are unchanged and stay valid) or nullptr if full or a
// package of that name is already resident.
LoadedApp *apps_store(const LoadedApp *app);

// Unload a resident package by name: remove its commands, free its image, free
// the slot. Returns false if no such package is loaded, and ALSO if a task is
// currently executing it — ask apps_busy_pid to tell the two apart.
bool apps_unload(const char *name);

// Is a package by this name already loaded? pkg_install_file loads what it
// installs, so the boot walk over the index would otherwise load it a second
// time — and the second copy cannot register the commands the first already
// owns, which reads as four packages failing when nothing is wrong with any of
// them.
bool apps_resident(const char *name);

// The pid of a task sitting inside this package right now, or -1.
//
// A package's command yields on every ABI call it makes, so the shell can be
// parked in the middle of one indefinitely while something else removes the
// package. Freeing it there hands the heap back memory a task is about to
// return into.
int apps_busy_pid(const char *name);

// The image a resident package was loaded at, or null if it is not loaded.
//
// This is the token a Command carries in its `owner` field, so it is what
// answers "does this command belong to that package" — which is how `pkg
// install` works out that the service keeping a package busy is one of its own,
// rather than something it has no business stopping.
const void *apps_owner_of(const char *name);

// Load an app file, run its app_main, and either keep it resident (if it
// registered commands) or unload it. The one place the load-run-resident flow
// lives, shared by `run`, `pkg install`, and boot-time package loading. Returns
// app_main's value, or -1 if the file could not be loaded. `quiet` suppresses
// the per-run chatter (used for boot loading).
int apps_launch(const char *file, int arg, bool quiet);

// The same, for a package whose read-only half is already in a flash slot.
//
// `blob` is where that half is mapped and `m` the manifest read out of the slot
// beside it — pkg.cpp does the opening, because it is the half that knows about
// slots and filesystems. Nothing is copied: this allocates the writable half and
// nothing else, which for a Nova D1 is 62 KB against 184. Returns app_main's
// value, or -1.
int apps_launch_pic(const void *blob, const PicManifest *m, int arg, bool quiet);

// Register the `apps` (list) and `unload` commands.
void apps_register(void);

#endif  // RPC_APPS_H
