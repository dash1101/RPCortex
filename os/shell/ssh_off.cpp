// No SSH in this image: the stand-in for sshc.cpp and sshd.cpp.
//
// On a board whose standard image has SSH (the Pico 2 W), an image built
// without it, or without the Wi-Fi it runs on, keeps `ssh` with its help line
// and has it say why. Where the standard image has never had SSH (the Pico W,
// whose slot is too tight for it, and the plain boards) there has never been an
// `ssh` command, and there still is none: RPC_STD_SSH is 0 there and this
// registers nothing.

#include "command.h"
#include "imagefeatures.h"

#ifndef RPC_STD_SSH
#define RPC_STD_SSH 0
#endif

static int no_ssh(int, char **) { return feature_missing(FEAT_SSH); }

void ssh_register(void) {
    if (!RPC_STD_SSH) return;
    static const Command c{"ssh", "log in to another machine; 'ssh server' for this one", no_ssh,
                           nullptr, LEVEL_USER, FORMS_SSH};
    cmd_register(&c);
}
