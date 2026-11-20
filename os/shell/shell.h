#ifndef RPC_SHELL_H
#define RPC_SHELL_H

// Register the built-in commands, then run the prompt forever. The built-ins
// register through the same cmd_register a loaded app uses, so there is exactly
// one command path in the system.
void shell_register_builtins(void);
void shell_run(void);

// Run one typed line from another terminal (a remote login) as the console's
// prompt would: ; && || | > all work. Returns the last status.
int shell_run_input(char *line);

// The prompt text the console shows, for the calling task's user and
// directory: user@device:cwd>
void shell_prompt_text(char *out, unsigned cap);

#endif  // RPC_SHELL_H
