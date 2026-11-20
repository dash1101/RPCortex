// No Bluetooth in this image: the stand-in for bt.cpp and btaudio.cpp.
//
// Compiled when the image is built without Bluetooth, which on the plain boards
// is always. `bt` and `btaudio` still exist, with the help lines and levels they
// always had, and say why they cannot work.

#include "command.h"
#include "imagefeatures.h"

static int no_bt(int, char **) { return feature_missing(FEAT_BLUETOOTH); }

void bt_register(void) {
    static const Command c{"bt", "Bluetooth status, scanning and advertising", no_bt,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}

void btaudio_register(void) {
    static const Command c{"btaudio", "play audio to a Bluetooth speaker", no_bt,
                           nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
