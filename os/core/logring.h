// The log ring.
//
// Everything that goes wrong is recorded here whether or not anyone was looking
// at the console — which is the point. A warning printed while you were in
// another window is gone; a warning in the ring is still there ten minutes
// later, and still there after the shell scrolled past it.
//
// A fixed ring in RAM rather than a file: the log has to work when the
// filesystem is the thing that is broken, and a logger that needs a healthy disk
// to record disk problems is not much of a logger. `logdump save` writes it out
// when there IS somewhere to write it.
//
// It survives a warm reboot. The buffer lives in a section the C startup does
// not clear, so the reason a device rebooted is readable after it comes back —
// which is the single most useful thing a log can do on a device with no screen.
#ifndef RPC_LOGRING_H
#define RPC_LOGRING_H

#include <stdint.h>

#define LOG_LINES     48
#define LOG_LINE_MAX  96

enum LogKind { LOG_K_INFO = 0, LOG_K_OK, LOG_K_WARN, LOG_K_ERR, LOG_K_BOOT };

struct LogLine {
    uint32_t at_ms;
    uint8_t  kind;
    char     text[LOG_LINE_MAX];
};

// Append. Oldest is dropped when full — a log that stops recording once it fills
// misses exactly the part you needed.
void log_add(LogKind kind, const char *text);
void log_addf(LogKind kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Oldest first. `i` from 0 to log_count()-1.
uint32_t       log_count(void);
const LogLine *log_at(uint32_t i);
void           log_clear(void);

// Lines dropped because the ring wrapped, so a dump can say what it is missing
// rather than quietly presenting a partial history as a complete one.
uint32_t log_dropped(void);

// How many boots this ring has seen. Timestamps are milliseconds since the
// CURRENT boot, so a dump spanning a restart needs the marker log_init writes to
// be readable at all.
uint32_t log_boot_count(void);

// Prepare the ring. Checks the magic first: if it survived a warm reboot the
// contents are kept, so the run-up to a crash is readable after the restart.
// Returns true when previous contents were found.
bool log_init(void);

#endif  // RPC_LOGRING_H
