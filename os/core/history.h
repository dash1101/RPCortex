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

// A ring of its own, for a terminal that is not the console: a remote login
// keeps its commands to itself, so one person's up-arrow never shows another's
// lines. The functions above are the console's ring.
struct HistRing {
    char line[HIST_N][HIST_LINE_MAX];
    int  count;
    int  next;
};
void        hist_ring_add(HistRing *r, const char *line);
const char *hist_ring_get(const HistRing *r, int depth);

#endif  // RPC_HISTORY_H
