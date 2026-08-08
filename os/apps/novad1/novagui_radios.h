// Desc: The SPI-radio screens — capture and replay sub-GHz, and a LoRa link test.
// File: novagui_radios.h
//
// A flat sibling of novagui, the same shape as novagui_ble: it drives the
// firmware's own `subghz` and `lora` shell commands through fw_shell_run on a
// worker and parses the text they print. There is no radio call in the package
// ABI, and a blocking receive on tick() would freeze the panel, so every command
// runs on a task exactly the way the BLE screens run `bt`.
//
// The Messages/mesh screen is NOT here — a separate mesh agent owns that and the
// `lora` command's text contract it parses.
#ifndef NOVA_GUI_RADIOS_H
#define NOVA_GUI_RADIOS_H

namespace nova {
namespace screens {

// The App table's open functions, shape fixed by it.
void open_subghz(void);     // CC1101: chip check, capture one OOK burst, replay it
void open_lora(void);       // SX1276: chip/config, send a test packet, listen once

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_RADIOS_H
