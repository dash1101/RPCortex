// What this image was built with.
//
// An image can be built with or without Wi-Fi, Bluetooth, SSH, the microSD
// driver, the add-on drivers and the development packages (tools/features.py
// has the list, and build.sh --features shows it). A feature that is left out
// is left out of the build, not switched off: its files are not compiled and its
// libraries are not linked. What stays is a small stand-in file per feature
// (net_off.cpp, bt_off.cpp, ssh_off.cpp, periph_off.cpp, sd_off.cpp) that keeps
// the commands and the package calls in place and has them say why they cannot
// work.
//
// This is the one place the device answers "what is in this image", for `ver`,
// `sysinfo`, `update` and the stand-ins.
//
// NOT features.h. The host suites compile with os/shell on the include path,
// and glibc's <stdio.h> includes its own <features.h>; a header of that name
// here replaced it, and every host suite failed to build.
#ifndef RPC_IMAGEFEATURES_H
#define RPC_IMAGEFEATURES_H

enum Feature {
    FEAT_WIFI,
    FEAT_BLUETOOTH,
    FEAT_SSH,
    FEAT_SD,
    FEAT_PERIPHERALS,
};

// The features compiled into this image, space separated ("wifi bluetooth sd
// peripherals"), as `ver` prints them.
const char *features_list(void);

// "standard" for the image a release publishes; otherwise what the build left
// out or added, the same words the image's file name carries ("nobt").
const char *features_variant(void);
bool        features_standard(void);

// Whether this BOARD could carry the feature at all, whatever this image has.
// The difference between "built without Wi-Fi" and "no wireless hardware".
bool feature_board_can(Feature f);

// Print why a command cannot run in this image and return 1, the status the
// command then exits with. The words depend on whether the board could have had
// the feature: "This image was built without Wi-Fi." on a Pico W built without
// it, "This board has no wireless hardware." on a Pico.
int feature_missing(Feature f);

#endif  // RPC_IMAGEFEATURES_H
