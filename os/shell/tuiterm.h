// The terminal side of the TUI: painting a grid, and reading events back.
#ifndef RPC_TUITERM_H
#define RPC_TUITERM_H

#include "tui.h"
#include "tuikey.h"

// Enter full-screen mode: alternate screen, cursor hidden, mouse reporting on.
// Always pair with tuiterm_end — a terminal left reporting sends escape
// sequences to the shell for every click afterwards.
void tuiterm_begin(void);
void tuiterm_end(void);

// Send the difference between `s` and what is already showing.
void tuiterm_present(const TuiScreen *s);

void tuiterm_cursor(int x, int y, bool visible);

// One event, or false when nothing is waiting. Non-blocking, so a caller
// controls its own frame rate and stays responsive to Ctrl+C.
bool tuiterm_poll(TuiEvent *out);

// The terminal's real size, asked for at begin. 80x24 when it does not answer.
void tuiterm_size(uint16_t *w, uint16_t *h);

// Re-ask the terminal how big it is and force a full repaint. Returns true when
// the size actually changed, so a caller can re-lay-out. A serial line has no
// resize notification, so this is bound to Ctrl+L rather than happening by
// itself — asking on a timer would race with whatever is being typed.
bool tuiterm_refresh(void);

bool tuiterm_active(void);

#endif  // RPC_TUITERM_H
