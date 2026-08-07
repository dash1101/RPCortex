// Desc: The hardware registry — every module, what it needs, and what it becomes on the home screen.
// File: novamodtab.h
//
// The table the home screen is built from. A module that is present becomes an
// app; one that is not is still listed, greyed, because a device you have not
// finished wiring should tell you what is missing rather than quietly not having
// the feature.
//
// PINS COME FROM novaboard AND NOWHERE ELSE. This table names the SIGNALS a
// module needs, never GPIO numbers — so a board profile change or somebody's
// `d1 pins set` is picked up here without editing anything. The two files
// drifting is exactly the failure this arrangement exists to prevent.
#ifndef NOVA_MODTAB_H
#define NOVA_MODTAB_H

#include "novaboard.h"

namespace nova {

// The five folders on the home screen, in the order they appear.
enum Category {
    CAT_WIRELESS = 0,
    CAT_SENSORS,
    CAT_TOOLS,
    CAT_SYSTEM,
    CAT_TESTING,
    CAT_COUNT
};

const char *category_name(Category c);

// How a module is attached. Shown on the Hardware screen, because "no answer
// from the PN532" is a different job depending on whether it is two wires or
// five.
enum BusKind {
    BUS_NONE = 0,   // onboard — the radio, which is not wired to anything
    BUS_I2C,
    BUS_SPI,
    BUS_UART,
    BUS_GPIO,
    BUS_PWM,
    BUS_ADC,
    BUS_ONEWIRE,
};

const char *bus_name(BusKind b);

// What a probe found.
enum Presence {
    MOD_UNKNOWN = 0,    // never looked
    MOD_ABSENT,         // looked, nothing answered
    MOD_PRESENT,        // answered
    MOD_UNWIRED,        // no pins configured, so there is nothing to look at
};

struct Module {
    const char *id;         // "cc1101" — the registry key and `d1 scan` name
    const char *label;      // "Sub-GHz" — what the home screen calls it
    const char *chip;       // "CC1101" — what to search for when buying one
    BusKind     bus;
    Category    cat;

    // The signals it needs, resolved through novaboard at the time of asking.
    // The first is the one reported when something is unwired, so it should be
    // the module's own select or data line rather than a shared bus pin.
    const board::PinId *pins;
    uint8_t             npins;

    // Folded into the Hardware screen instead of getting a home icon of its own.
    // A buzzer is worth testing and is not worth a place on the home screen.
    bool diag_only;

    // I2C address, for the modules that have one. Zero otherwise.
    uint8_t addr;
};

// Every module, in a fixed order.
const Module *modules(void);
unsigned module_count(void);

// By id, or null.
const Module *module_by_id(const char *id);

// Is every signal this module needs actually assigned? False means there is no
// point probing for it — and that is a different answer from "probed and absent",
// which is why Presence has both.
bool module_wired(const Module &m);

// What the last scan found. Cached rather than probed on every draw: a scan
// talks to the bus, and the home screen redraws far more often than hardware
// appears.
Presence module_presence(const Module &m);
void     module_set_presence(const Module &m, Presence p);

// Probe everything. This is what `d1 scan` runs and what the boot does once.
void modules_scan(void);

}  // namespace nova

#endif  // NOVA_MODTAB_H
