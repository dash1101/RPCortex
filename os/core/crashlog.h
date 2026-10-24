// The crash report: what a previous run left behind, as text for a file.
//
// The log ring and the black box both survive a WARM reset, which covers the
// watchdog. They do not survive power being pulled, and a device that crashed
// at a bench and was unplugged before anyone read the console has lost the only
// record of why. Writing it down at the next boot, before anything can
// overwrite it, is what turns "it crashed once" into something to investigate.
//
// This file only builds the text. Writing it is the shell's job (logcmd.cpp),
// which keeps this part free of hardware headers and testable on the host.
#ifndef RPC_CRASHLOG_H
#define RPC_CRASHLOG_H

#include <stddef.h>
#include <stdint.h>
#include "blackbox.h"
#include "logring.h"

// Where the report goes. One previous generation is kept beside it, so a second
// crash does not erase the evidence of the first.
#define CRASHLOG_PATH "/os/crash.log"
#define CRASHLOG_OLD  "/os/crash.old"

// A comfortable upper bound for the text crashlog_format produces: the header,
// plus every line the ring can hold at its widest.
#define CRASHLOG_MAX  (1280 + LOG_LINES * (LOG_LINE_MAX + 24))

// Build the report into `out`. `bb` is the previous run's black box, or null.
// Always NUL-terminated; output that would not fit is cut at a line boundary
// rather than mid-line. Returns the length, excluding the NUL.
size_t crashlog_format(char *out, size_t cap, const BlackBox *bb);

// The one-character tag a log line kind prints with. Shared with `logdump` so a
// saved log and a crash report mark errors and warnings the same way.
const char *log_kind_tag(uint8_t kind);

#endif
