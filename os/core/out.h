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

// Raw bytes on the data channel — what `cat` and `hex` emit. Goes wherever
// out_multi goes, so a redirect or a pipe catches it too. Anything writing file
// content must use this rather than putchar, or `cat x > y` silently produces
// an empty y while the content goes to the console.
void out_write(const char *data, uint32_t len);

// --- capture ----------------------------------------------------------------
//
// Redirect the DATA channel into a buffer, leaving the status channel on the
// console. This is what makes `a | b` and `a > f` work, and it is why out_multi
// and the tagged helpers were separated in the first place: during `ls > f` the
// listing belongs in the file, but an error about it still belongs on screen
// where someone will see it.
//
// Not nestable — one shell, one pipeline at a time. Beginning a capture while
// one is active is a bug in the caller, so it is refused rather than tracked.
//
// ANSI escapes are stripped on the way in, whichever form is used. What comes
// out is text a parser can read, which is the only thing a buffer is ever for.
bool     out_capture_begin(char *buf, uint32_t cap);

// The same, plus the tagged status lines. For fw_shell_run, where a package
// asked for a command's output and means all of it — a listing whose header is
// an out_info and whose rows are out_multi arrives half-captured otherwise, and
// half a listing reads exactly like a complete one.
bool     out_capture_begin_all(char *buf, uint32_t cap);
uint32_t out_capture_end(void);       // bytes captured; ends the capture
bool     out_capturing(void);
// True if the capture ran out of room. A truncated pipe should say so rather
// than quietly hand the next stage half its input.
bool     out_capture_overflowed(void);

// v1's input prompt: "<message> ••>  ". Shared by the shell and the login flow
// so every prompt on the device looks the same.
void out_prompt(const char *msg);

#endif  // RPC_OUT_H

// --- progress ---------------------------------------------------------------
//
// Anything that takes long enough to wonder about should say so, and the two
// cases need different answers: a download knows how far it has to go, a WiFi
// join does not.
//
// Both redraw in place with a carriage return and no newline — which is exactly
// why out_flush exists. stdout is LINE buffered, so a line with no newline sits
// in the C library's buffer until something later fills it. A 696 KB download
// appeared to update twice, at 49% and 100%, because that is how often 1 KB of
// buffered progress lines happened to fill.

// A bar with a percentage and the figures behind it:
//   Downloading  [##########----------]  49%   342/696 KB
void out_progress(const char *label, uint64_t done, uint64_t total);

// For work of unknown length: a turning bar and how long it has been going.
//   Connecting  /  4s
void out_spinner(const char *label, uint32_t elapsed_ms);

// Finish a progress line: clears it and moves on, so the next output starts
// clean rather than after a half-drawn bar.
void out_progress_done(void);

// Push whatever is buffered to the terminal now.
void out_flush(void);

// Stop synchronising output, permanently. For the crash and stack-overflow
// paths only: past this point a report matters more than clean interleaving,
// and waiting on a lock held by a core that has already stopped would hang
// instead of saying what went wrong.
void out_panic_mode(void);
