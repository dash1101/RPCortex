// A scrolling, selectable list — the widget every full-screen app here needs.
//
// The editor needs it for a file picker, `settings` for its rows, `ps` for
// tasks, a package browser for the repo. Written once, the arithmetic that
// everyone otherwise gets subtly wrong lives in one place: keeping the cursor
// on screen when it moves past an edge, scrolling by a page without losing the
// selection, and turning a mouse click at row 7 into "the fourth visible item,
// which is item 19".
//
// It holds no items. The caller owns them and answers "what is item N called",
// which is what lets a 4000-line file and a five-row settings panel use the
// same widget without either allocating.
#ifndef RPC_TUILIST_H
#define RPC_TUILIST_H

#include <stdint.h>
#include <stdbool.h>
#include "tui.h"

struct TuiList {
    int count;        // how many items exist
    int sel;          // which is selected
    int top;          // the first one drawn
    int rows;         // how many fit on screen
    bool wrap;        // moving off the end returns to the start
};

void tuilist_init(TuiList *l, int count, int rows);

// Keep sel and top consistent after anything changes them. Called by everything
// below; exposed because a caller that edits the fields directly — after
// deleting an item, say — needs it too.
void tuilist_clamp(TuiList *l);

void tuilist_move(TuiList *l, int delta);   // +1 down, -1 up
void tuilist_page(TuiList *l, int dir);     // +1 a page down, -1 up
void tuilist_home(TuiList *l);
void tuilist_end(TuiList *l);

// Scroll without moving the selection, which is what a wheel does. The
// selection stays where it is even when it scrolls out of view — matching every
// other list a person has used.
void tuilist_scroll(TuiList *l, int delta);

// Which item is drawn on visible row `row`, or -1 for an empty row past the
// end. This is the mouse's whole job: a click at y becomes an item.
int  tuilist_item_at_row(const TuiList *l, int row);

// Where item `index` is drawn, or -1 when it is scrolled out of view.
int  tuilist_row_of_item(const TuiList *l, int index);

// Is a scrollbar needed, and where does its thumb sit? Returned rather than
// drawn so a caller can put it on either side or leave it out.
bool tuilist_scrollbar(const TuiList *l, int *thumb_row, int *thumb_len);

// Draw the rows. `label` fills `out` with the text for item `index`; a null
// `label` draws nothing but still paints the selection bar, which is what a
// caller drawing its own columns wants.
typedef void (*TuiListLabel)(void *ctx, int index, char *out, uint32_t cap);
void tuilist_draw(const TuiList *l, TuiScreen *s, int x, int y, int w,
                  TuiListLabel label, void *ctx, uint8_t fg);

#endif  // RPC_TUILIST_H
