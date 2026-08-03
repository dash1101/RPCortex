// The interactive line editor.
//
// Cursor movement, word jumps, word delete, history recall and tab completion —
// all of it pure state machine over a character buffer, with terminal I/O behind
// two function pointers. That separation is the point: off-by-ones in cursor
// arithmetic are invisible on a board nobody is holding, and they are exactly
// the bugs a host test catches in a second.
//
// Key handling targets PuTTY at 115200, which is how this device is actually
// used. Where PuTTY differs from a Linux terminal the PuTTY behaviour wins and
// the difference is commented, because the person hitting the other case will
// need to know why.
#ifndef RPC_LINEEDIT_H
#define RPC_LINEEDIT_H

#include <stdint.h>

// Terminal seam. `getch` returns a byte, or LE_NO_KEY if none arrived within
// `timeout_us` (0 = poll, don't wait). `putch` writes one byte.
#define LE_NO_KEY (-1)

struct LineIO {
    int  (*getch)(void *ctx, uint32_t timeout_us);
    void (*putch)(void *ctx, char c);
    // Called once a redraw is complete. Optional — null is fine, and the host
    // tests leave it so. It exists because a line being edited never ends in a
    // newline, and on a buffered stdout that means nothing the user types
    // reaches the terminal until they press Enter.
    void (*flush)(void *ctx);
    void  *ctx;
};

// Completion source. Called with the word being completed; fills `out` with the
// i-th candidate that starts with `prefix` and returns false when there are no
// more. `word_start` is the byte offset of the word in the line, so a source can
// tell "completing the command" (0) from "completing an argument" (anything
// else) and offer commands or filenames accordingly.
typedef bool (*LineComplete)(void *ctx, const char *prefix, uint32_t word_start,
                             uint32_t index, char *out, uint32_t cap);

// History source, oldest-last: depth 0 is the most recent. Returns nullptr past
// the end.
typedef const char *(*LineHistory)(void *ctx, int depth);

struct LineEdit {
    LineIO       io;
    LineComplete complete;
    LineHistory  history;
    void        *complete_ctx;
    void        *history_ctx;
    const char  *prompt;
};

// Read one line. Returns the length. Blocks until Enter.
uint32_t line_edit(const LineEdit *le, char *buf, uint32_t cap);

// The inline suggestion shown ahead of the cursor in grey — the rest of the
// single candidate that matches what has been typed, so you can see what Tab
// would give you before pressing it. Right arrow or End at the end of the line
// accepts it.
//
// Only shown when there is exactly ONE candidate. With several, the shared part
// is not a prediction and showing it would be guessing at the user.
uint32_t line_ghost(const char *const *cands, uint32_t n, const char *prefix,
                    char *out, uint32_t cap);

// --- the pure pieces, exposed for testing -----------------------------------

// Start of the word at or before `pos` (a word is a run of non-space).
uint32_t line_word_start(const char *s, uint32_t len, uint32_t pos);
// Start of the NEXT word at or after `pos`, or `len` if there is none.
uint32_t line_word_end(const char *s, uint32_t len, uint32_t pos);

// Insert `c` at `pos`. Returns the new length (unchanged if the buffer is full).
uint32_t line_insert(char *s, uint32_t len, uint32_t cap, uint32_t pos, char c);
// Delete `count` characters ending at `pos` (i.e. backspace). Returns the new
// length; *pos_out is where the cursor lands.
uint32_t line_delete_back(char *s, uint32_t len, uint32_t pos, uint32_t count,
                          uint32_t *pos_out);
// Delete the character AT `pos` (i.e. the Delete key). Returns the new length.
uint32_t line_delete_at(char *s, uint32_t len, uint32_t pos);

// The longest prefix shared by every candidate, for the "complete as far as it
// is unambiguous" behaviour. Writes into `out`; returns its length.
uint32_t line_common_prefix(const char *const *cands, uint32_t n, char *out, uint32_t cap);

// Decoded escape sequences. Values above 0x7F so they cannot collide with a byte.
enum LineKey {
    KEY_NONE = 0x100,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_DELETE,
    KEY_WORD_LEFT, KEY_WORD_RIGHT,
    KEY_UNKNOWN
};

// Decode a collected escape sequence (everything AFTER the ESC byte).
// Handles CSI ("[...final") and SS3 ("Ofinal") forms.
int line_decode_escape(const char *seq, uint32_t len);

#endif  // RPC_LINEEDIT_H
