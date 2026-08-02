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

const Command *cmd_find(const char *name);
uint32_t       cmd_count(void);
const Command *cmd_at(uint32_t i);

#endif  // RPC_COMMAND_H
