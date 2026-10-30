// Desc: MAC prefix to vendor, for naming what is on a network or in the air.
// File: novaoui.h
//
// Ported from novaoui.py. The table is the MicroPython suite's, generated from the
// IEEE MA-L registry and filtered to the vendors that matter for identifying what is
// around you; novaoui_data.inc is that table as bytes (gen_oui_data.py).
//
// A prefix that is not in it returns nothing, and the caller shows the raw prefix
// with no vendor claimed: on a tool whose job is telling you what something is, a
// wrong label is worse than none. A locally-administered address (bit 1 of the first
// octet) is a RANDOMISED one - phones and current laptops send those by default - and
// has no vendor at all, so it is reported as random and never looked up.
//
// The class is a hint about what the silicon usually ends up in, never a claim about
// a device: an Espressif prefix means "an ESP32-class module", which might be a
// camera, a plug or a doorbell. Say it that way in the UI.
#ifndef NOVA_OUI_H
#define NOVA_OUI_H

#include <stdint.h>

namespace nova {
namespace oui {

struct Result {
    const char *vendor;     // null when unknown or random
    const char *klass;      // "network", "camera", ... ; "random" for a randomised address; null when unknown
};

// A MAC as text ("aa:bb:cc:dd:ee:ff", "AA-BB-CC-DD-EE-FF", "AABBCCDDEEFF"; separators
// are dropped and case does not matter) or as its first bytes.
Result lookup(const char *mac_text);
Result lookup_bytes(const uint8_t *mac, unsigned n);

// The prefix as "AA:BB:CC" for showing when no vendor is known; "?" when the address
// is too short. `out` needs 9 bytes.
void prefix(const char *mac_text, char *out, unsigned cap);

// How many prefixes the table holds, for the Hardware screen and the tests.
unsigned table_size(void);

}  // namespace oui
}  // namespace nova

#endif  // NOVA_OUI_H
