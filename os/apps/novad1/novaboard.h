// Desc: One source of truth for pins and buses. Board profiles, and where a pin came from.
// File: novaboard.h
//
// The other leaf. Six drivers used to carry their own defaults; every one of
// them asks here instead, so adding a board is a data change and the drivers do
// not move.
//
// A pin resolves as REGISTRY OVERRIDE -> BOARD PROFILE -> CALLER'S FALLBACK. The
// registry deliberately wins: a device somebody has already wired and configured
// must never be silently re-pinned by a software update that ships a new profile.
#ifndef NOVA_BOARD_H
#define NOVA_BOARD_H

#include "rpc_app.h"

namespace nova {
namespace board {

// A pin that is not assigned. -1 rather than 0, because GPIO 0 is a real pin and
// is used as one on every profile here. A constant rather than a macro so it can
// be named through the namespace like everything else.
constexpr int PIN_NONE = -1;

// Where the answer came from, for `d1 pins` to print. A wiring problem is nearly
// always someone's override from six months ago, so showing the source is what
// turns "that pin is wrong" into "that pin was set by hand".
enum Source {
    SRC_NONE = 0,     // nothing knows this pin
    SRC_REG,          // an override in the registry
    SRC_PROFILE,      // the board's own map
    SRC_FALLBACK,     // what the caller passed in
};

// Every signal the suite knows how to wire. The order is the order `d1 pins`
// prints, grouped the way somebody wiring a board works through it.
enum PinId {
    PIN_SDA = 0, PIN_SCL,
    PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW, PIN_BTN1, PIN_BTN2, PIN_KILLSW,
    PIN_SPI_SCK, PIN_SPI_MOSI, PIN_SPI_MISO,
    PIN_CC_CS, PIN_CC_GDO0,
    PIN_SX_CS, PIN_SX_RST,
    PIN_SD_CS,
    PIN_IR_TX, PIN_IR_RX,
    PIN_GPS_TX, PIN_GPS_RX,
    PIN_BUZZER, PIN_VIBE, PIN_LED, PIN_DHT, PIN_IBUTTON,
    PIN_BATTERY, PIN_VBUS,
    PIN_COUNT
};

// The short name, as `d1 pins set <name> <gpio>` takes it and as the registry
// stores it (Apps.NovaD1_PIN_<name>).
const char *name(PinId id);

// Look up by name. Accepts the short form or the full registry key, because the
// drivers grew both conventions and neither call site had to change.
// Returns PIN_COUNT when nothing matches.
PinId by_name(const char *s);

// The resolved pin, or `fallback` when nothing knows it.
int pin(PinId id, int fallback = PIN_NONE);
int pin(const char *name, int fallback = PIN_NONE);

// Where that answer came from.
Source source(PinId id);

// Set or clear an override. Neither saves to flash — the caller decides when,
// because `d1 pins set` changing four pins should cost one write.
void set(PinId id, int gpio);
void clear(PinId id);

// --- boards -----------------------------------------------------------------

// The profile in use. Configured value wins; otherwise detected from the board
// the firmware reports, so a fresh device is right without being told.
const char *board_id(void);
const char *board_name(void);

// Choose a profile by id, or "auto" to go back to detection. False for an id
// with no profile.
bool board_set(const char *id);

// What the firmware says this board is, mapped to a profile id — or nullptr when
// no profile covers it. An RP2040 board resolves to nothing ON PURPOSE: it
// shares the header pinout but has around 65 KB of heap after the OS, which is
// not enough to hold this package's image. Claiming the board would mislead.
const char *detect(void);

// How many profiles there are, and each one's id, for `d1 pins board`.
unsigned profile_count(void);
const char *profile_id(unsigned i);

// The bus the display is on for this profile: "i2c" or "spi".
const char *display_bus(void);

// --- validation -------------------------------------------------------------
//
// RP2 has no GPIO matrix. A peripheral pin is fixed to a group — the pattern
// repeats every 8 GPIO, role is gpio % 4 (0 = RX/MISO, 1 = CSn, 2 = SCK,
// 3 = TX/MOSI) and the controller is (gpio / 8) % 2 — so a sensible-looking map
// can simply be unassignable. Catching that here is the difference between a
// build-time answer and an afternoon with a multimeter.

// Problems found, written one per line into `out`. Returns how many there were;
// zero means the map is good.
unsigned check(char *out, unsigned cap);

// Is this GPIO one the board keeps for itself? On Pico W-class boards 23, 24,
// 25 and 29 are wired to the CYW43 radio and are not free, however a pinout
// diagram labels them.
bool reserved(int gpio);

}  // namespace board
}  // namespace nova

#endif  // NOVA_BOARD_H
