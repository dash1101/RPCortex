// Desc: The SPI-radio screens — capture and replay sub-GHz, and a LoRa link test.
// File: novagui_radios.h
//
// A flat sibling of novagui, the same shape as novagui_ble: it drives the
// firmware's own `subghz` and `lora` shell commands through fw_shell_run on a
// worker and parses the text they print. There is no radio call in the package
// ABI, and a blocking receive on tick() would freeze the panel, so every command
// runs on a task exactly the way the BLE screens run `bt`.
//
// The Messages screen lives here too, on the same `lora` text contract as the
// link test — send a short line, listen for one, keep the last few. A full mesh
// stack is a separate agent's; this is the handheld messenger.
#ifndef NOVA_GUI_RADIOS_H
#define NOVA_GUI_RADIOS_H

namespace nova {
namespace screens {

// The App table's open functions, shape fixed by it.
void open_subghz(void);     // CC1101: chip check, capture one OOK burst, replay it
void open_lora(void);       // SX1276: chip/config, send a test packet, listen once
void open_messages(void);   // SX1276: send/receive short text over LoRa

// Text <-> hex for the LoRa payload, and the message-list access, exposed so the
// host suite can prove the round trip and the receive path against the exact
// strings the firmware prints.
namespace radios {
bool text_to_hex(const char *text, char *out, unsigned cap);
int  hex_to_text(const char *hex, char *out, unsigned cap);
}

}  // namespace screens
}  // namespace nova

#endif  // NOVA_GUI_RADIOS_H
