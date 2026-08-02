// Command-line parsing — argv splitting, pipelines, chaining, redirection.
//
// Pure string work, no filesystem and no command registry, so the whole of it
// host-tests. That matters more here than anywhere else in the shell: an
// off-by-one in a quote scan or a connector scan does not crash, it silently
// runs the wrong command, which is the kind of bug that survives a long time on
// a device with no debugger attached.
//
// Every function operates IN PLACE on a mutable buffer and returns pointers into
// it. Nothing allocates.
#ifndef RPC_CMDLINE_H
#define RPC_CMDLINE_H

#include <stdint.h>

// How one segment is joined to the previous one.
enum Connector {
    CON_FIRST = 0,   // the first segment on the line
    CON_SEQ,         // ;   run regardless
    CON_AND,         // &&  run only if the previous succeeded
    CON_OR           // ||  run only if the previous failed
};

// Split into argv, honouring double quotes so `echo "a b"` is one argument.
// Quotes are removed. There is no escape character — v1's behaviour, and enough
// for a device shell. Returns argc.
int cmdline_split_args(char *line, char **argv, int max);

// The next unquoted connector, or nullptr. *kind is what it is, *skip its length
// in characters.
char *cmdline_next_connector(char *s, Connector *kind, int *skip);

// The next unquoted single '|' (a pipe). '||' is a connector, not a pipe, and is
// skipped over.
char *cmdline_next_pipe(char *s);

// Strip leading and trailing spaces in place. Returns a pointer into `s`.
char *cmdline_trim(char *s);

// Split a trailing "> file" / ">> file" off a segment. Truncates the segment at
// the '>' and returns the filename, or nullptr when there is no redirect.
// *append is set for '>>'.
char *cmdline_split_redirect(char *seg, bool *append);

#endif  // RPC_CMDLINE_H
