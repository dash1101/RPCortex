// Desc: Battery level and USB power, and the honest gaps in both.
// File: novapower.h
//
// Neither pin has a profile default, deliberately. An unwired ADC input floats,
// and a floating input reads as SOMETHING — so a battery gauge with no divider
// attached does not read zero, it reads a confident wrong number. That is worse
// than no gauge, and it is why both are opt-in.
#ifndef NOVA_POWER_H
#define NOVA_POWER_H

#include <stdint.h>

namespace nova {
namespace power {

enum Source {
    PWR_UNKNOWN = 0,    // nothing could say — not the same as "no power"
    PWR_USB,            // on USB, however the battery is doing
    PWR_BATTERY,        // running off the cell, at some unstated level
};

// Where the power is coming from. USB wins when it is detected at all, because
// on USB the battery reading is a charging voltage rather than a state of charge
// and showing it as a level would be a lie that moves.
//
// THE SOURCE AND THE LEVEL ARE SEPARATE QUESTIONS, and PWR_BATTERY does not
// promise percent() will answer. The firmware knows what is powering the board
// — VBUS sense lives on the radio module and only it can reach it — while the
// level needs a divider on the battery pin, which the profile deliberately
// leaves unset. So a board can know it is on the cell and have nothing to say
// about how full it is; a caller printing a level has to check for itself.
//
// Cached for a second, because the status bar asks on every frame.
Source source(void);

// 0-100, or -1 when there is no divider wired and no way to know. Cached for a
// few seconds: an ADC read crosses the ABI and the number moves slowly.
int percent(void);

// Millivolts at the cell, or 0. Gated for plausibility — a lithium cell between
// 2.5 V and 4.6 V is a reading, and anything outside that is an unwired pin
// rather than a very flat battery.
int millivolts(void);

// True when the level is low enough to warn about.
bool low(void);

}  // namespace power
}  // namespace nova

#endif  // NOVA_POWER_H
