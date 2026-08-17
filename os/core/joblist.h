// Line lists — the shared shape behind startup, task and service.
//
// All three are the same thing on disk: a file of lines, each one a command,
// managed by index. v1 had three near-identical copies of the add/remove/list
// code; this is that logic once, pure, so the off-by-ones in "remove line 3"
// are settled by a test rather than by three separate bug reports.
//
// Comment and blank lines are preserved in the file but are not counted or
// indexed, so the numbers a user sees in `list` match what `remove` takes.
#ifndef RPC_JOBLIST_H
#define RPC_JOBLIST_H

#include <stdint.h>

// Called per entry, 1-based to match what the user is shown.
typedef void (*JobWalkFn)(void *ctx, uint32_t index, const char *line);

// Entries, ignoring comments and blanks.
uint32_t joblist_count(const char *buf, uint32_t len);

void joblist_walk(const char *buf, uint32_t len, JobWalkFn cb, void *ctx);

// Append a line. Returns the new length; unchanged if it would not fit or the
// line is empty. A duplicate is allowed — running the same command twice at boot
// is unusual but not wrong, and refusing it would be surprising.
uint32_t joblist_add(char *buf, uint32_t len, uint32_t cap, const char *line);

// Remove the 1-based `index`-th entry. Returns the new length, unchanged if the
// index is out of range.
uint32_t joblist_remove(char *buf, uint32_t len, uint32_t index);

// Copy the `index`-th entry into out. Returns false if there is no such entry.
bool joblist_get(const char *buf, uint32_t len, uint32_t index, char *out, uint32_t cap);

// Split "<seconds> <command>" as `task add` stores it. Returns false when the
// leading field is not a number, which is how a malformed line is skipped rather
// than run at the wrong interval.
bool joblist_split_interval(const char *line, uint32_t *secs, const char **cmd);

// The first word of an entry — the command the line runs.
//
// For `pkg install`, which has to decide whether a service belongs to the
// package it is replacing, and stops the service if it does. So the answer
// selects a task to end, and a wrong one ends the wrong thing.
//
// It REFUSES rather than truncates when the word does not fit, and that is the
// whole reason this is a function rather than three lines at the caller: a
// truncated name resolves to a DIFFERENT command. "novad1-legacy" cut to
// "novad1" is a service belonging to somebody else, reported as belonging to
// this package and stopped on its behalf.
//
// False for a blank or whitespace-only entry too. `out` is always terminated.
bool joblist_first_word(const char *line, char *out, uint32_t cap);

#endif  // RPC_JOBLIST_H
