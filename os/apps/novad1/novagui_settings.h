// Desc: The settings tree — the five groups the app catalogue opens.
// File: novagui_settings.h
//
// A flat sibling of novagui, the same as novagui_tools. The MicroPython suite
// kept its settings inside the 2,900-line novagui.py because the rows reached
// into the runner for the home screen and the display; here they reach for the
// registry instead, so there is nothing holding them in the runner and they
// start out where they belong.
//
// Five screens rather than one index with thirty-one rows. The MicroPython
// suite had the single list first and spent five screens of scrolling to reach
// anything; splitting it is the change that made settings findable, and the
// groups here are the ones it landed on.
#ifndef NOVA_GUI_SETTINGS_H
#define NOVA_GUI_SETTINGS_H

namespace nova {
namespace screens {

// Every one of these pushes its screen and returns. They are the App table's
// open functions, so their shape is fixed by it.
void open_set_home(void);        // layout, which apps, favourites, the name
void open_set_network(void);     // the radio latch, joining, the clock over NTP
void open_set_security(void);    // the lock, its code, and what the radios say
void open_set_system(void);      // clock speed, timezone, reboot, reset
void open_set_device(void);      // read-only: what this device is

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_SETTINGS_H
