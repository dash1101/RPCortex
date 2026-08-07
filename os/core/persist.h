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

#endif  // RPC_PERSIST_H
