// Ctrl+C — the universal stop.
//
// Any command that can run for more than a moment must be interruptible, and
// "more than a moment" includes every network command, every recursive
// filesystem walk, and anything a package does. A shell where Ctrl+C only works
// at the prompt is a shell where a mistyped `ping` means reaching for the USB
// cable.
//
// The mechanism is deliberately not signals: there is no scheduler to deliver
// them and no safe point to unwind from. Instead the byte is noticed wherever
// input is read, a flag is raised, and long-running work polls it. That means
// every loop that can spin has to ask — the alternative, forcibly unwinding a
// command mid-flash-write, corrupts the filesystem.
#ifndef RPC_INTERRUPT_H
#define RPC_INTERRUPT_H

#include <stdint.h>

// Raise the flag. Called from the input path when 0x03 arrives.
void intr_raise(void);

// Has Ctrl+C been pressed since the last clear? Cheap; safe to call in a loop.
bool intr_pending(void);

// Clear it. The shell does this before dispatching each command, so a stray
// Ctrl+C at the prompt cannot kill the next thing typed.
void intr_clear(void);

// Poll for a Ctrl+C on the input stream WITHOUT consuming anything else, then
// report whether one is pending. This is what a long-running command calls: it
// is the only way a byte gets noticed while a command, rather than the line
// editor, owns the input.
//
// Returns true if the command should stop. A caller that returns on true should
// leave things in a consistent state; that is the caller's job, not this one's.
bool intr_check(void);

// Bytes intr_check pulled off the input while scanning for Ctrl+C, in order.
// The line editor drains these BEFORE reading the real input, so type-ahead and
// pasted command blocks survive a command running in between. Returns -1 when
// there is nothing stashed.
int  intr_stashed(void);
void intr_stash_clear(void);

// Set by the platform so intr_check can read a byte without blocking. Kept as a
// seam so the pure core and the host tests do not need the SDK.
typedef int (*IntrPollFn)(void);      // returns a byte, or -1 if none waiting
void intr_set_poll(IntrPollFn fn);

// --- when the console is carrying data rather than typing --------------------
//
// intr_check reads up to 32 bytes off the console looking for a Ctrl+C, and
// EVERY package calls it: fw_task_should_stop is intr_check, and a service loop
// calls that tens of times a second. That is right while the bytes on the wire
// are somebody typing — they are stashed and the line editor gets them back.
//
// It is catastrophic while the bytes are a file. `put` reads the console
// directly, so anything another task scanned first never reaches it: the bytes
// go into the stash, the oldest are dropped when it fills, and the transfer is
// short by however many were taken. Measured on a Pico 2 W with the Nova D1
// screen service running, a 300 KB upload lost 735 bytes — and the survivors
// were replayed at the prompt afterwards, which is how it was found.
//
// It is also how a 0x03 in the middle of a binary file becomes an interrupt for
// an unrelated task.
//
// So a transfer CLAIMS the console. While it is claimed, intr_check answers
// from the flag and the stop request only — it does not read. One claim at a
// time; a second is refused rather than queued, because two readers of one
// stream is the bug being fixed.
bool intr_input_claim(void);       // false when somebody else already has it
void intr_input_release(void);

// Scoped, so no early return can leave the console claimed by a command that
// has finished.
struct InputClaim {
    bool ok;
    InputClaim()  { ok = intr_input_claim(); }
    ~InputClaim() { if (ok) intr_input_release(); }
    InputClaim(const InputClaim &) = delete;
    InputClaim &operator=(const InputClaim &) = delete;
};

#endif  // RPC_INTERRUPT_H
