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

// --- whose Ctrl+C ---------------------------------------------------------------
//
// The flag used to be one global, because there was one keyboard. A remote
// login is a second keyboard: its Ctrl+C must stop the command running in
// THAT session, and the console's must not reach it — nor may a command run
// remotely clear the console's flag, which the shell does before every line.
//
// So the flag belongs to a DOMAIN. Domain 0 is the console's, and every task
// is in it unless it was started from a remote login, which allocates a domain
// of its own and passes it on (by value, through TaskShell) to whatever it
// starts. A domain carries a generation: once the login ends and its number is
// reused, a job it left running holds a stale handle, and a stale handle is
// never pending rather than somebody else's flag.
//
// Everything below acts on the CALLING task's domain; intr_raise_in is the one
// that names a domain, for the code that reads a remote session's input on a
// different task from the one running its commands.
uint16_t intr_domain_new(void);          // 0 when none is free
void     intr_domain_free(uint16_t dom);
void     intr_raise_in(uint16_t dom);
bool     intr_pending_in(uint16_t dom);

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
// there is nothing stashed. When the stash is full intr_check stops reading and
// leaves the rest on the wire; it never drops anything to make room.
int  intr_stashed(void);
void intr_stash_clear(void);

// One key, without blocking: a stashed byte first, then the wire. -1 if none.
//
// EVERY command that reads keys itself must read them through this, not
// getchar_timeout_us. A command that both polls for a key and calls
// intr_check (directly, or as fw_task_should_stop) loses any key that arrives
// between the two: intr_check takes it into the stash, the getchar never sees
// it, and the line editor replays it at the next prompt. `download` said
// "press any key" and ignored most of them; the 'q' turned up as "qls".
int  intr_getkey(void);

// A filter every console key passes through on its way to whoever reads it:
// intr_getkey here, and the shell's line editor through intr_filter_key. It
// returns the byte to pass on, or -1 to keep it. Installed by the network bridge
// (shell/bridge.cpp) in an image without Wi-Fi, to take a host's offer out of the
// keys and to drop a stray 0x1E line before anything can read it as typing; none
// is installed otherwise. Not applied to a remote terminal's keys.
typedef int (*IntrKeyFilter)(int c);
void intr_set_key_filter(IntrKeyFilter fn);
int  intr_filter_key(int c);

// Set by the platform so intr_check can read a byte without blocking. Kept as a
// seam so the pure core and the host tests do not need the SDK.
typedef int (*IntrPollFn)(void);      // returns a byte, or -1 if none waiting
void intr_set_poll(IntrPollFn fn);

// Several keys at once, for a reader that takes whole lines off the console
// (the network bridge): the same rules as intr_getkey (a remote terminal's own
// keys, only the shell's task, the stash first, every byte through the filter),
// with the wire read in bulk. Returns how many were put in `buf`, 0 for none.
//
// Why it exists: one byte per call costs a trip through the SDK's stdio and the
// USB stack for each one, and a bridged download spent more time there than
// writing the file. Reading up to `cap` at a time halved the time to receive
// a chunk (measured on a Pico 2 W).
int  intr_getkeys(char *buf, int cap);

// Bytes a bulk reader took and did not use, given back so the next reader sees
// them first and in order (they go in front of anything stashed since). What
// does not fit is dropped; with a 512-byte stash and a reader that takes at
// most a couple of hundred, it fits.
void intr_unget(const char *buf, int n);

// The bulk counterpart of IntrPollFn: up to `cap` bytes, without blocking.
// Without one, intr_getkeys reads a byte at a time through the IntrPollFn.
typedef int (*IntrPollBulkFn)(char *buf, int cap);
void intr_set_poll_bulk(IntrPollBulkFn fn);

// The millisecond clock a timed console claim ages against. Defaults to the
// real one; a host test overrides it to drive a deadline by hand. Passing null
// restores the default rather than disabling expiry — a claim that could never
// expire is the failure this exists to remove.
typedef uint32_t (*IntrClockFn)(void);
void intr_set_clock(IntrClockFn fn);

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

// The same claim, with a deadline. This is the door a package's line read goes
// through, and the deadline is what makes that safe: if the claim is never
// released — the reader faulted, was killed, or was contained mid-call — it
// stops being anyone's once the deadline passes, so the console recovers without
// a reboot. A long, legitimate read pushes the deadline out by calling renew (or
// simply re-claiming) as it makes progress. Plain intr_input_claim above has no
// deadline and never expires; only this door can time out, so nothing that uses
// the plain claim — `put` above all — is touched.
bool intr_input_claim_for(uint32_t ms);
void intr_input_renew(uint32_t ms);

// Release the claim if the task whose SLOT this is held it. Called from
// task_slot_recycled — the one hook that runs whenever a task's slot is reused,
// on every path a task can end by — so a console left claimed by a task that has
// gone is reclaimed the same way the sandbox pool and the network slots are,
// rather than waiting for a reboot. Keyed on the slot, not the pid, because by
// the time the hook runs the task is already dismantled; the slot was recorded
// when the claim was taken, while it still meant something.
void intr_input_task_ended(int slot);

// Would a poll by `pid` be allowed right now? This is the predicate intr_check
// asks, exposed so a host test can put a pid other than its own to it — there
// is one thread there, so the case that matters cannot be reached any other
// way. Answers yes for everyone once a timed claim has expired.
bool intr_input_may_read(int pid);

// Scoped, so no early return can leave the console claimed by a command that
// has finished. The timed variant is for a reader that must not keep the console
// past a deadline even if its own unwinding is skipped — a package line read.
struct InputClaim {
    bool ok;
    InputClaim()  { ok = intr_input_claim(); }
    ~InputClaim() { if (ok) intr_input_release(); }
    InputClaim(const InputClaim &) = delete;
    InputClaim &operator=(const InputClaim &) = delete;
};

struct InputClaimFor {
    bool ok;
    explicit InputClaimFor(uint32_t ms) { ok = intr_input_claim_for(ms); }
    ~InputClaimFor() { if (ok) intr_input_release(); }
    InputClaimFor(const InputClaimFor &) = delete;
    InputClaimFor &operator=(const InputClaimFor &) = delete;
};

#endif  // RPC_INTERRUPT_H
