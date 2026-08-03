// The command registry — the C++ replacement for launchpad's command table and
// its `.lp` mapping files.
//
// In v1 a command was a line in system.lp pointing at a Python function, loaded
// by name through the interpreter. Here a command is a struct with a function
// pointer, and the registry holds them in a fixed array. Two kinds register
// into the SAME table, which is the whole point:
//
//   * built-ins, registered at boot (ls, run, mem, ...)
//   * commands a loaded APP registers when it starts, via the app ABI
//
// So an installed app extends the shell exactly the way a v1 package did — that
// is the package/program system, rebuilt on the loader rather than on an
// interpreter.
#ifndef RPC_COMMAND_H
#define RPC_COMMAND_H

#include <stdint.h>

// argc/argv, the shape every shell command in existence expects. argv[0] is the
// command name. Return 0 for success; non-zero is an error status the shell can
// use for && / || later, the same convention v1's had_error() served.
typedef int (*CommandFn)(int argc, char **argv);

struct Command {
    const char *name;
    const char *help;      // one line, shown by `help`
    CommandFn   fn;
    // Set when the command came from a loaded app rather than a built-in, so it
    // can be removed cleanly when that app unloads. nullptr for built-ins.
    void       *owner;
};

// A fixed table: no allocation, and a hard ceiling is the right shape for a
// device where runaway registration should fail loudly rather than eat the heap.
#define CMD_MAX 96

// Register a command. Returns false if the table is full or the name collides
// with an existing one — a silent overwrite would let an app shadow `reboot`.
bool cmd_register(const Command *cmd);

// Remove every command owned by `owner` (an unloading app). Built-ins, whose
// owner is nullptr, are never touched.
void cmd_remove_owner(void *owner);

// Aliases live in their own table, not the command one. v1's system.lp mapped
// ~40 extra names onto the same functions (ll/la/dir -> ls, del/rmdir -> rm,
// more/less/view -> cat); registering each as a full Command would spend a
// registry slot and a help line on what is really just a second spelling. Here
// an alias is two pointers, resolved before dispatch, and `help` stays readable
// because it lists commands rather than every name that reaches them.
struct Alias { const char *name; const char *target; };

#define ALIAS_MAX 64

bool         cmd_alias(const char *name, const char *target);
// The command an alias points at, or nullptr if `name` is not an alias.
const char  *cmd_alias_target(const char *name);
uint32_t     cmd_alias_count(void);
const Alias *cmd_alias_at(uint32_t i);

// --- user aliases -----------------------------------------------------------
//
// Separate from the built-in ones above, and a different thing: a built-in alias
// is a second NAME for a command, while a user alias maps a name to a whole
// COMMAND LINE ("alias ll=ls -l"). That is what v1's alias did, and it is why
// these are expanded into the line before it is parsed rather than resolved at
// dispatch. They persist to the registry as Alias.<name>.
#define UALIAS_MAX     16
#define UALIAS_NAME    16
#define UALIAS_VALUE   64

// Define or replace. Returns false if the name collides with a real command
// (shadowing `rm` would be a trap), the table is full, or a field is too long.
bool        cmd_ualias_set(const char *name, const char *value);
bool        cmd_ualias_remove(const char *name);
const char *cmd_ualias_get(const char *name);      // the line, or nullptr
uint32_t    cmd_ualias_count(void);
const char *cmd_ualias_name_at(uint32_t i);

// Looks up a real command only. Use cmd_resolve for the dispatch path.
const Command *cmd_find(const char *name);
// cmd_find, then one alias hop. One hop, not a chain: an alias to an alias is a
// configuration mistake, and following it would need loop detection to be safe.
const Command *cmd_resolve(const char *name);
uint32_t       cmd_count(void);
// How many registrations were refused (table full, or a duplicate name). Checked
// once after boot: a command that silently failed to register is a bug that
// otherwise only surfaces when someone types it.
uint32_t       cmd_refused(void);
const Command *cmd_at(uint32_t i);

#endif  // RPC_COMMAND_H
