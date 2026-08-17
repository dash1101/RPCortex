// Desc: Getting text and a yes out of one knob and one button.
// File: novakeys.h
//
// Three screens that every other screen needs and none of them should own: the
// on-screen keyboard, a six-digit PIN pad, and a confirmation.
//
// There is one encoder and one switch. Everything a person can say to this
// device has to be reachable from turn, press, and hold — so the keyboard puts
// SHIFT, SPACE, DEL and OK on their own row rather than assuming a modifier
// key that does not exist.
//
// The callbacks take a context pointer rather than being lambdas. A capturing
// lambda needs storage, and a screen has 384 bytes of pool and no allocator
// worth the name; a plain function pointer and a pointer to something the
// caller already owns costs eight bytes and cannot dangle by surprise.
//
// THE TEXT HANDED TO A CALLBACK DOES NOT OUTLIVE THE CALL. It points into the
// keyboard, and the keyboard pops as soon as the callback returns. Copy it.
#ifndef NOVA_KEYS_H
#define NOVA_KEYS_H

#include "novaui.h"

namespace nova {
namespace ui {

typedef void (*TextFn)(void *ctx, const char *text);
typedef void (*VoidFn)(void *ctx);

// Push the keyboard. `done` is called with the finished string when OK is
// chosen or SELECT is held; `cancel` when it is abandoned. Either may be null,
// and so may `initial`.
//
// `secret` masks the buffer with asterisks — for a WiFi password, which is the
// one thing on this device somebody might type with a person behind them.
void keyboard(const char *title, const char *initial, bool secret,
              TextFn done, VoidFn cancel, void *ctx);

// Push the PIN pad: six digits, turned to the value and pressed to advance.
// `done` gets the six characters and a terminator. HOME submits, because SELECT
// is spent moving between digits and there is nothing else left.
void pinpad(const char *title, TextFn done, VoidFn cancel, void *ctx);

// Push a question. `yes` runs when it is answered yes; backing out does
// nothing at all, which is the right default for everything that asks.
//
// The default selection is NO. Every question worth asking is one where the
// wrong answer costs something, and a dialogue that starts on the destructive
// option turns a mis-click into that cost.
void confirm(const char *question, const char *yes_label, VoidFn yes, void *ctx);

// Push a message that goes away on any button. For the answer to something
// that just happened — "Connected", "No route to host" — where a status line
// on the screen underneath would be missed.
void notice(const char *title, const char *body);

// What the last notice said. The text lives outside the screen — a pool slot is
// 384 bytes and a message read once is not worth a third of one — and being
// outside it makes it readable, which is how a test asks whether the device
// reported what actually happened.
const char *last_notice(void);

}  // namespace ui
}  // namespace nova

#endif  // NOVA_KEYS_H
