// Desc: The Bluetooth screens — what is around, plotted by strength, and who came home.
// File: novagui_ble.h
//
// A flat sibling of novagui, importing only the UI leaf, the same as
// novagui_tools and novagui_system.
//
// All three screens share ONE device table and ONE scan, because there is only
// one radio and only one output capture in the OS. A per-screen scanner would
// be two tasks in the same driver and two writers on the same buffer, which is
// the failure this package has already paid for once elsewhere.
#ifndef NOVA_GUI_BLE_H
#define NOVA_GUI_BLE_H

namespace nova {
namespace screens {

// Every one of these pushes its screen and returns. They are the App table's
// open functions, so their shape is fixed by it.
void open_ble(void);        // scan, list, and one device in full
void open_radar(void);      // the same table, strongest first, with filters
void open_presence(void);   // the named devices, and whether they are here

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_BLE_H
