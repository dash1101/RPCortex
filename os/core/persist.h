// Persistence for the registry and the accounts table.
//
// The registry and users cores are pure (text in, text out). This is the thin
// device layer that reads their files off littlefs at boot and writes them back
// when they change. Kept apart so the cores host-test with no filesystem.
#ifndef RPC_PERSIST_H
#define RPC_PERSIST_H

// What the last load had to repair, if anything.
//
// Read after persist_load_all so the boot can SAY that a shadow copy was used.
// A device that healed itself silently has taught nobody that its flash is
// going, and the second time it happens there may be no shadow left.
struct PersistRepair {
    bool registry_restored;   // the primary was unreadable; the backup was good
    bool registry_lost;       // both were unreadable
    bool users_restored;
    bool users_lost;
};
const PersistRepair *persist_repair_report(void);

void persist_load_all(void);     // registry + users, at boot
void persist_save_registry(void);
void persist_save_users(void);
void persist_save_dirty(void);   // save whichever changed; called after commands

// --- the signed-in user's own settings ----------------------------------------
//
// The "User." half of the registry lives in that person's home directory and is
// swapped in at login. See the note in registry.h for which keys those are and
// why it is not simply everything an app owns.

// Load <user>'s settings and make them the active scope. Called once a login
// has succeeded, and safe on a user who has never had any.
void persist_scope_enter(const char *user);

// Write them back if they changed, then forget them. Called at logout, and
// before a login replaces the scope with somebody else's.
void persist_scope_leave(void);

// Write them out now, without leaving the scope. persist_save_dirty calls this.
void persist_save_scope(void);

#endif  // RPC_PERSIST_H
