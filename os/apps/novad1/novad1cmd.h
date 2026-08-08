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
int selfupdate(void);

}  // namespace cmd
}  // namespace nova

#endif  // NOVA_D1CMD_H
