// What this image was built with. See imagefeatures.h.
//
// Every value here comes from os/CMakeLists.txt, which takes it from
// tools/features.py: RPC_FEATURES is the list compiled in, RPC_IMAGE_VARIANT is
// "standard" or the name of a custom build, and RPC_CAN_<X> says whether this
// board could carry feature X at all. All of them are always defined, 0 or 1,
// so the choices below are ordinary `if`s rather than more preprocessor.

#include "imagefeatures.h"
#include "out.h"

#ifndef RPC_FEATURES
#define RPC_FEATURES ""
#endif
#ifndef RPC_IMAGE_VARIANT
#define RPC_IMAGE_VARIANT "standard"
#endif
#ifndef RPC_CAN_WIFI
#define RPC_CAN_WIFI 0
#endif
#ifndef RPC_CAN_BLUETOOTH
#define RPC_CAN_BLUETOOTH 0
#endif
#ifndef RPC_CAN_SD
#define RPC_CAN_SD 0
#endif
#ifndef RPC_HAS_WIFI
#define RPC_HAS_WIFI 0
#endif

#include <string.h>

// THE MARKER tools/release.py reads out of a built image, and build.sh checks
// after every build: "rpcimage:" and then the variant. A release refuses any
// image whose marker does not say "standard", so a custom build cannot be
// published by accident, whatever its file is called.
//
// Declared before it is defined so that it has external linkage. A const array
// at namespace scope is otherwise internal, and the compiler would be free to
// fold the one place it is read into a copy of the string without the prefix,
// leaving nothing for the release check to find.
extern const char rpc_image_mark[];
const char rpc_image_mark[] = "rpcimage:" RPC_IMAGE_VARIANT;

const char *features_list(void) {
    return RPC_FEATURES[0] ? RPC_FEATURES : "none";
}

const char *features_variant(void) {
    return rpc_image_mark + (sizeof("rpcimage:") - 1);
}

bool features_standard(void) {
    return strcmp(features_variant(), "standard") == 0;
}

bool feature_board_can(Feature f) {
    switch (f) {
    case FEAT_WIFI:        return RPC_CAN_WIFI;
    case FEAT_BLUETOOTH:   return RPC_CAN_BLUETOOTH;
    case FEAT_SSH:         return RPC_CAN_WIFI;
    case FEAT_SD:          return RPC_CAN_SD;
    case FEAT_PERIPHERALS: return true;
    }
    return false;
}

int feature_missing(Feature f) {
    const char *why = "This image was built without that feature.";
    switch (f) {
    case FEAT_WIFI:
        why = RPC_CAN_WIFI ? "This image was built without Wi-Fi."
                           : "This board has no wireless hardware.";
        break;
    case FEAT_BLUETOOTH:
        why = RPC_CAN_BLUETOOTH ? "This image was built without Bluetooth."
                                : "This board has no Bluetooth hardware.";
        break;
    case FEAT_SSH:
        why = !RPC_CAN_WIFI ? "This board has no wireless hardware, so it has no SSH."
            : !RPC_HAS_WIFI ? "This image was built without Wi-Fi, so it has no SSH."
                            : "This image was built without SSH.";
        break;
    case FEAT_SD:
        why = RPC_CAN_SD ? "This image was built without microSD support."
                         : "This board's image has no microSD support.";
        break;
    case FEAT_PERIPHERALS:
        why = "This image was built without the add-on drivers (nfc, ibutton, subghz, lora).";
        break;
    }
    out_err("%s", why);
    return 1;
}
