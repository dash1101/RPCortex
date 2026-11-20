// sudo: proving it is you, and remembering for a few minutes that you did.
//
// The rule for who may run what is perms.h. This is the other half: the
// password. A command that changes the system runs as `sudo <command>`, and
// sudo asks for the signed-in account's password (not root's), three tries,
// on the terminal the command came from. A pass counts for SUDO_GRACE_MS in
// that session (an interrupt domain: the console, or one remote login), and
// each use starts the time again, so a run of system commands asks once.
//
// WHERE NOBODY CAN ANSWER, IT DOES NOT ASK. A background job, a service, a
// package's worker, a line a package runs, link's EXEC: none of those has a
// person on the other end of a terminal, and a prompt there either waits for
// ever or reads keys meant for something else. They use the session's grace
// if there is one, and otherwise are refused with a line saying what to do.
// An SSH command run without a terminal (`ssh dev 'sudo ...'`) is refused too:
// the password would be typed into something that echoes it.
//
// Work the system starts from its own configuration (startup items, services,
// scheduled tasks, a command staged for a maintenance boot) runs with sudo
// already: that configuration lives in /etc and the registry keys perms.h
// protects, which only somebody who passed sudo could have written.
//
// Pure apart from two seams (the prompt, and the pause after a wrong
// password), so all of it is host-tested: see os/host/sudo_test.cpp.
#ifndef RPC_SUDO_H
#define RPC_SUDO_H

#include <stdint.h>
#include "perms.h"

// Five minutes, as sudo's own default. Long enough to finish what was started,
// short enough that walking away ends it.
#define SUDO_GRACE_MS 300000u

// TaskShell::sudo_flags. BLOCKED: sudo was refused because it could not ask
// (link's EXEC answers `ERR sudo` for it).
#define SUDO_F_BLOCKED 0x01

// How many sessions can hold a grace at once: the console and a remote login
// or two. When full, the oldest is forgotten.
#define SUDO_GRACE_SLOTS 4

// --- seams, set by the shell at boot ------------------------------------------

// Ask for a password on the calling task's terminal, masked. An empty answer
// counts as a wrong one.
typedef void (*SudoReadFn)(const char *prompt, char *buf, unsigned cap);
void sudo_set_reader(SudoReadFn fn);

// A pause after a wrong password, to slow a guesser. Nothing by default.
typedef void (*SudoPauseFn)(uint32_t ms);
void sudo_set_pause(SudoPauseFn fn);

// This boot is a maintenance boot (safeboot, or the recovery ladder). At the
// console, sudo then does not ask: see PERMISSIONS.md, "Forgotten passwords".
void sudo_set_maintenance(bool on);
bool sudo_maintenance(void);

// --- can a person be asked? ------------------------------------------------------

enum SudoAsk {
    SUDO_CAN_ASK,
    SUDO_NO_TERMINAL,   // a background job, a service, a package's worker
    SUDO_NO_PTY,        // an SSH command run without a terminal
    SUDO_NOT_HERE,      // a line a package runs, or link's EXEC
};
SudoAsk sudo_can_ask(void);

// --- the grace -------------------------------------------------------------------

// Does the calling task's session hold a grace for its user right now?
bool sudo_grace_valid(void);
// Forget one session's grace (`sudo -k`, a logout, a remote login ending), or all.
void sudo_grace_forget(uint16_t dom);
void sudo_grace_forget_all(void);

// --- proving it ------------------------------------------------------------------

// True when the calling task may go ahead with something only its user's
// password should allow: it already runs with sudo, the grace holds, or the
// password is typed now. Otherwise false, after saying why. `what_for` ends
// the prompt: "Password for root, to <what_for>".
bool sudo_authenticate(const char *what_for);

// The same check for a password a program supplied (link's AUTH), never
// asking. On SUDO_OK the session's grace starts.
enum SudoCheck { SUDO_OK, SUDO_WRONG, SUDO_NOT_ADMIN, SUDO_NOPASS, SUDO_NOBODY };
SudoCheck sudo_check_password(const char *pw);

// --- the gate in front of every command --------------------------------------------

enum SudoStep {
    SUDO_GO,                // run it, as the task already is
    SUDO_REFUSE_NOT_ADMIN,  // the account cannot run it, with or without sudo
    SUDO_REFUSE_NEED_SUDO,  // an admin, but it changes the system: say "sudo"
    SUDO_AUTHENTICATE,      // `sudo` in front: prove it, then run it with sudo
};
SudoStep sudo_step(CmdLevel level, bool is_admin, bool prefixed, bool elevated);

// The whole gate as the shell uses it: work out the level for these arguments,
// decide, say why on refusal, authenticate when that is the step. True to run
// the command, with *elevated set to whether it runs with sudo. `argv` no
// longer has the `sudo` in front; `prefixed` says it was there.
bool sudo_gate(CmdLevel base, uint8_t forms, int argc, char **argv, bool prefixed,
               bool *elevated);

// `sudo -v` (authenticate now, and start the grace) and `sudo -k` (forget it).
// Returns the exit status. `sudo -l` needs the command table and is the shell's.
// Does sudo carry into a script file at `path` (absolute)? Only when someone
// vouched for it (a person typed sudo for this very command) or when the file
// is in /os or /etc, where only sudo can write. A service or startup item is
// elevated by the system, and a script in /nova, /tmp or a home could have been
// rewritten by an ordinary account since. rpscmd.cpp drops sudo otherwise.
bool sudo_passes_to_script(const char *path, bool typed);

int sudo_validate(void);
int sudo_forget(void);

#endif  // RPC_SUDO_H
