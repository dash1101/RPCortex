// Desc: The direct-hardware screens — battery, DHT11, GPS and IR, none of which
//       has a firmware command behind it.
// File: novagui_sensors.h
//
// A flat sibling of novagui, the same shape as novagui_radios and
// novagui_contact. What these four have in common is not their catalogue
// category — Battery, Climate and GPS are Sensors, IR is Wireless — it is that
// none of them is reachable through fw_shell_run: there is no firmware command
// for a DHT11, a GPS or an IR receiver the way there is for the SX1276 or an
// iButton, so each of these talks to its own pin directly over the package
// ABI (ADC, one-wire, UART, a watched GPIO) instead.
#ifndef NOVA_GUI_SENSORS_H
#define NOVA_GUI_SENSORS_H

namespace nova {
namespace screens {

// The App table's open functions, shape fixed by it.
void open_battery(void);    // a live ADC reading, where the board has a divider
void open_climate(void);    // DHT11: temperature and humidity, one-wire
void open_gps(void);        // NEO-M8N: NMEA off the UART, no decode beyond a fix
void open_ir(void);         // VS1838B: proof of a signal, not a decode

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_SENSORS_H
