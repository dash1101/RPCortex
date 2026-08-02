#ifndef RPC_SHELL_H
#define RPC_SHELL_H

// Register the built-in commands, then run the prompt forever. The built-ins
// register through the same cmd_register a loaded app uses, so there is exactly
// one command path in the system.
void shell_register_builtins(void);
void shell_run(void);

#endif  // RPC_SHELL_H
