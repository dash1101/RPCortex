// No microSD driver in this image: the stand-in for sd.cpp.
//
// On a board that can carry the driver (the RP2350 ones), `sd` still exists and
// says it was left out. On the RP2040 boards there has never been an `sd`
// command, and an image for one still has none: RPC_CAN_SD is 0 there and this
// registers nothing.

#include "command.h"
#include "imagefeatures.h"

#ifndef RPC_CAN_SD
#define RPC_CAN_SD 0
#endif

static int no_sd(int, char **) { return feature_missing(FEAT_SD); }

void sd_register(void) {
    if (!RPC_CAN_SD) return;
    static const Command c{"sd", "microSD card status and mounting", no_sd,
                           nullptr, LEVEL_USER};
    cmd_register(&c);
}
