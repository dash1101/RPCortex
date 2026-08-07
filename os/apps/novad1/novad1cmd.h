// Desc: The rest of the `d1` command surface — everything that is not pins or the screen.
// File: novad1cmd.h
#ifndef NOVA_D1CMD_H
#define NOVA_D1CMD_H

namespace nova {
namespace cmd {

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
