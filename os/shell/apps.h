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

// Copy a loaded app into the resident table. Returns the stored record (whose
// image/veneer pointers are unchanged and stay valid) or nullptr if full or a
// package of that name is already resident.
LoadedApp *apps_store(const LoadedApp *app);

// Unload a resident package by name: remove its commands, free its image, free
// the slot. Returns false if no such package is loaded.
bool apps_unload(const char *name);

// Load an app file, run its app_main, and either keep it resident (if it
// registered commands) or unload it. The one place the load-run-resident flow
// lives, shared by `run`, `pkg install`, and boot-time package loading. Returns
// app_main's value, or -1 if the file could not be loaded. `quiet` suppresses
// the per-run chatter (used for boot loading).
int apps_launch(const char *file, int arg, bool quiet);

// Register the `apps` (list) and `unload` commands.
void apps_register(void);

#endif  // RPC_APPS_H
