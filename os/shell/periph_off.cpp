// No add-on drivers in this image: the stand-in for nfc.cpp, ibutton.cpp,
// cc1101.cpp and sx1276.cpp.
//
// The commands keep their names, help lines and levels and say the drivers were
// left out. The NFC package calls stay in the ABI table and answer as a reader
// that is not there does (FW_NFC_NOCHIP), so a package that uses them still
// loads and reports "no reader" rather than failing to load.

#include "command.h"
#include "imagefeatures.h"
#include "rpc_app.h"

#include <stdint.h>

int nfc_pkg_select(unsigned, unsigned, FwNfcTag *, unsigned, unsigned) { return FW_NFC_NOCHIP; }
int nfc_pkg_exchange(unsigned, unsigned, const uint8_t *, unsigned, uint8_t *, unsigned) {
    return FW_NFC_NOCHIP;
}

static int no_periph(int, char **) { return feature_missing(FEAT_PERIPHERALS); }

void nfc_register(void) {
    static const Command c{"nfc", "read an NFC tag with a PN532", no_periph,
                           nullptr, LEVEL_USER};
    cmd_register(&c);
}

void ibutton_register(void) {
    static const Command c{"ibutton", "read a DS1990A iButton key", no_periph,
                           nullptr, LEVEL_USER};
    cmd_register(&c);
}

void subghz_register(void) {
    static const Command c{"subghz", "CC1101 sub-GHz: status, freq, capture, replay",
                           no_periph, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}

void lora_register(void) {
    static const Command c{"lora", "SX1276 LoRa: status, config, send, receive",
                           no_periph, nullptr, LEVEL_ADMIN};
    cmd_register(&c);
}
