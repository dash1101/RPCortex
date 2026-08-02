// Console output — a faithful port of v1's RPCortex.py output layer.
//
// The look of RPCortex is these five tagged lines and their colours. v1 built
// each as "<colour>[<white><symbol><colour>] <reset><message>", optionally with
// a "[prefix]" between, and that exact shape is reproduced here so v2 reads
// identically on the same terminal:
//
//   out_ok    [@]  cyan     something succeeded
//   out_info  [:]  magenta  progress / neutral statement
//   out_warn  [?]  yellow   worth noticing, not fatal
//   out_err   [!]  red      failed          (also sets the error flag)
//   out_fatal [!!!] red     failed badly    (also sets the error flag)
//   out_multi       none    the data channel: ls/cat/grep output, untagged
//
// The multi/status split is the same one v1 drew: multi() is stdout-like (data
// you might pipe), the tagged helpers are stderr-like (status about the data).
// Keeping it now means pipes and && / || can be added later without revisiting
// every call site.
#ifndef RPC_OUT_H
#define RPC_OUT_H

#include <stdint.h>

// v1's ANSI constants, same values.
#define C_HEADER   "\033[95m"
#define C_BLUE     "\033[94m"
#define C_CYAN     "\033[96m"
#define C_WARN     "\033[93m"
#define C_GRAY     "\033[90m"
#define C_GREEN    "\033[32m"
#define C_RESET    "\033[0m"
#define C_FAIL     "\033[91m"
#define C_BOLD     "\033[1m"
#define C_UNDER    "\033[4m"
#define C_WHITE    "\033[97m"

// Every one of these is printf-shaped, so it is declared printf-shaped. A
// mismatched argument then fails the build instead of printing a garbage line
// on a serial console nobody is watching — which is how format bugs normally
// survive on an embedded device.
#define RPC_PRINTF(fmt_idx) __attribute__((format(printf, fmt_idx, fmt_idx + 1)))

// Tagged status lines. `p` is an optional prefix shown as a second bracket,
// e.g. out_okp("WiFi", "Connected") -> "[@] [WiFi] Connected".
void out_ok   (const char *fmt, ...) RPC_PRINTF(1);
void out_info (const char *fmt, ...) RPC_PRINTF(1);
void out_warn (const char *fmt, ...) RPC_PRINTF(1);
void out_err  (const char *fmt, ...) RPC_PRINTF(1);
void out_fatal(const char *fmt, ...) RPC_PRINTF(1);
void out_okp  (const char *p, const char *fmt, ...) RPC_PRINTF(2);
void out_infop(const char *p, const char *fmt, ...) RPC_PRINTF(2);
void out_warnp(const char *p, const char *fmt, ...) RPC_PRINTF(2);
void out_errp (const char *p, const char *fmt, ...) RPC_PRINTF(2);

// The data channel — untagged, uncoloured, exactly what v1's multi() emitted.
void out_multi(const char *fmt, ...) RPC_PRINTF(1);

// A blank separator line. Its own call because out_multi("") is a zero-length
// format string, which the compiler flags — correctly, since it reads as a
// mistake everywhere else.
void out_blank(void);

// Pad `s` to `width` VISIBLE characters, ignoring any ANSI escapes it carries.
// printf's own "%-16s" counts escape bytes, so a coloured string in a padded
// field silently loses its column. Anything building a table uses this.
void out_pad(const char *s, int width, char *dst, int cap);

// Per-command error status, as v1's had_error()/clear_error(). Set by
// out_err/out_fatal; the shell clears it before each command. This is what
// && / || will read.
void out_clear_error(void);
bool out_had_error(void);

// v1's input prompt: "<message> ••>  ". Shared by the shell and the login flow
// so every prompt on the device looks the same.
void out_prompt(const char *msg);

#endif  // RPC_OUT_H
