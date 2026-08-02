// Command history ring for the shell's line editor. Pure (a static ring, no I/O)
// so the circular-index math host-tests. read_line drives it from arrow keys.
#ifndef RPC_HISTORY_H
#define RPC_HISTORY_H

#define HIST_N        16
#define HIST_LINE_MAX 128

// Record a command. Ignores empty lines and a line identical to the most recent,
// so holding enter or repeating a command does not fill the ring with dupes.
void hist_add(const char *line);

// The depth-th most recent command (0 = most recent), or nullptr past the end.
const char *hist_get(int depth);

int  hist_count(void);
void hist_reset(void);   // for tests

#endif  // RPC_HISTORY_H
