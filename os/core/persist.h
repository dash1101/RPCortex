// Persistence for the registry and the accounts table.
//
// The registry and users cores are pure (text in, text out). This is the thin
// device layer that reads their files off littlefs at boot and writes them back
// when they change. Kept apart so the cores host-test with no filesystem.
#ifndef RPC_PERSIST_H
#define RPC_PERSIST_H

void persist_load_all(void);     // registry + users, at boot
void persist_save_registry(void);
void persist_save_users(void);
void persist_save_dirty(void);   // save whichever changed; called after commands

#endif  // RPC_PERSIST_H
