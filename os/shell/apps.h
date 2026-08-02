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

// Register the `apps` (list) and `unload` commands.
void apps_register(void);

#endif  // RPC_APPS_H
