// A DuckyScript reader.
//
// DuckyScript is the little language a USB "rubber ducky" payload is written in
// — one command a line: REM for a comment, STRING to type text, ENTER, a
// chord like CTRL ALT DELETE, DELAY to wait. This parses it and calls back with
// what to do, so the same reader drives the real keyboard on the device and a
// recording mock in the host tests. It never touches USB itself.
//
// Grounded in the Hak5 DuckyScript 1.0 command set, not recall — a wrong key
// name types silence, which is the failure that is invisible until it is on a
// real machine.
#ifndef RPC_DUCKY_H
#define RPC_DUCKY_H

#include <stdint.h>

// What a line turns into. The emitter owns press-and-release; `key` is one
// chord (modifiers held while the keycode is tapped). `stop` lets a long
// payload be interrupted — the runner asks it between lines and it returns
// nonzero to abort, which is how Ctrl+C on the console stops a runaway script.
typedef struct {
    void (*key)(void *ctx, uint8_t mod, uint8_t keycode);
    void (*text)(void *ctx, const char *s);
    void (*delay)(void *ctx, uint32_t ms);
    int  (*stop)(void *ctx);
    void *ctx;
} DuckyEmit;

typedef struct {
    uint32_t default_delay;   // DEFAULTDELAY: inserted after each later command
    int      error_line;      // first line that failed, or 0
    int      lines;           // lines executed
} DuckyState;

// ducky_run_line's return, which also tells the runner whether to pace the line
// with the default delay: a keystroke is paced, a comment or DEFAULTDELAY line
// is not, an unknown command is an error and is NOT typed (a typo must never
// become keystrokes).
#define DUCKY_OK      0    // a keystroke ran; the runner paces it
#define DUCKY_NOPACE  1    // a comment/blank/DEFAULTDELAY line; do not pace
#define DUCKY_ERR   (-1)   // an unknown command; nothing was typed

// One line. Returns one of DUCKY_OK / DUCKY_NOPACE / DUCKY_ERR (above). Reads
// and updates st.
int ducky_run_line(const char *line, DuckyState *st, const DuckyEmit *e);

// A whole script (newline-separated). Runs each line, honours DEFAULTDELAY,
// and stops early if the emitter's stop() fires or a hard cap is hit. Returns
// the number of lines run; st->error_line names the first bad line if any.
int ducky_run(const char *script, DuckyState *st, const DuckyEmit *e);

#endif  // RPC_DUCKY_H
