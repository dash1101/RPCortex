// Desc: The rest of the `d1` command surface — everything that is not pins or the screen.
// File: novad1cmd.h
#ifndef NOVA_D1CMD_H
#define NOVA_D1CMD_H

namespace nova {
namespace cmd {

// Bring the screen up. `bg` spawns the loop as its own task and returns;
// otherwise the loop runs here and this does not return until it stops.
//
// The ONE implementation, called both by `novad1 gui` and by `service
// start`/`restart`. Those two used to reach it with fw_shell_run("novad1 gui
// --bg"), which reads well and killed the device: a package command runs on the
// shell's task, and the shell dispatching a second package command onto that
// same task overwrote the first one's record of how to get back out of the
// package. The outer command then returned into nothing — IACCVIOL at address
// zero, with the fault handler reporting "no package was running" because by
// the time it looked, the inner call had cleared the flag that says one is.
//
// The firmware refuses that re-entry now and says why. This does not need it:
// it is a function in the same package, and calling a function is what was
// wanted all along.
int screen_start(bool bg);

int setup(void);
int service(int argc, char **argv);
int style(int argc, char **argv);
int apps(int argc, char **argv);
int fav(int argc, char **argv);
int lock(int argc, char **argv);
int incognito(int argc, char **argv);
int logs(int argc, char **argv);
int notifications(int argc, char **argv);
int wifiprobe(void);

// Reproduce, from the shell, exactly what a SCREEN does when it runs a command:
// spawn a task and call fw_shell_run from inside it. Every screen that reads a
// listing goes through that path and several of them came back empty on a real
// device, which is not reproducible from a command running on the shell task.
int selftest(void);

// Drive the screen from the shell: `novad1 tap cw`, `sel`, `back`, `home`,
// `hold`, `homehold`. Injects the gesture the encoder would have produced.
//
// There is one encoder and no other way in, so a screen that misbehaves can
// only be reported second-hand — "Tasks does not work" is where that ends up,
// and it is not enough to find anything with. This makes every screen reachable
// from a terminal, which is also the only way a fault in one can be watched as
// it happens rather than read out of a log afterwards.
int tap(int argc, char **argv);

// Print what is on the panel, as characters, over the serial console.
//
// The single most useful thing this device was missing. Every UI bug this month
// was reported second-hand by somebody looking at a 2.4 inch screen and
// answered by guessing, because there was no way to SEE what was being drawn
// from anywhere else. Eight kilobytes at 115200 is under a second.
int shot(void);
int selfupdate(void);

}  // namespace cmd
}  // namespace nova

#endif  // NOVA_D1CMD_H
